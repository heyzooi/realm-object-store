////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "global_notifier.hpp"

#include "impl/realm_coordinator.hpp"
#include "impl/transact_log_handler.hpp"
#include "object_store.hpp"
#include "results.hpp"
#include "object_schema.hpp"
#include "util/event_loop_signal.hpp"

#include <realm/util/file.hpp>
#include <realm/util/uri.hpp>
#include <realm/lang_bind_helper.hpp>

#include <utility>
#include <stdexcept>

using namespace realm;
using namespace realm::_impl;

AdminRealmManager::AdminRealmManager(std::string local_root, std::string server_base_url, std::string access_token)
: m_regular_realms_dir(util::File::resolve("realms", local_root)) // Throws
, m_server_base_url(std::move(server_base_url))
, m_access_token(std::move(access_token))
{
    util::try_make_dir(m_regular_realms_dir); // Throws

    Realm::Config config;
    config.cache = false;
    config.path = util::File::resolve("admin.realm", local_root);
    config.schema_mode = SchemaMode::Additive;
    config.sync_server_url = m_server_base_url + "/admin";
    config.sync_user_token = m_access_token;
    config.schema = Schema{
        {"RealmFile", {
            {"id", PropertyType::String, "", "", true, true, false},
            {"path", PropertyType::String, "", "", false, false, false},
        }}
    };
   m_realm = Realm::get_shared_realm(std::move(config));
}

void AdminRealmManager::start(std::function<void(std::string, std::string)> callback)
{
    m_results = Results(m_realm, *ObjectStore::table_for_object_type(m_realm->read_group(), "RealmFile"));
    m_notification_token = m_results.add_notification_callback([=](CollectionChangeSet changes, std::exception_ptr) {
        auto& table = *ObjectStore::table_for_object_type(m_realm->read_group(), "RealmFile");
        size_t id_col_ndx   = table.get_column_index("id");
        size_t name_col_ndx = table.get_column_index("path");
        if (changes.empty() || m_first) {
            for (size_t i = 0, size = table.size(); i < size; ++i) {
                callback(table.get_string(id_col_ndx, i), table.get_string(name_col_ndx, i));
            }
            m_first = false;
        }
        else {
            for (auto i : changes.insertions.as_indexes()) {
                callback(table.get_string(id_col_ndx, i), table.get_string(name_col_ndx, i));
            }
        }
    });
}

Realm::Config AdminRealmManager::get_config(StringData realm_id, StringData realm_name)
{
    Realm::Config config;
    config.path =  util::File::resolve(std::string(realm_id) + ".realm", m_regular_realms_dir);
    config.sync_server_url = m_server_base_url + "/" + realm_name.data();
    config.sync_user_token = m_access_token;
    config.schema_mode = SchemaMode::Additive;
    return config;
}

void AdminRealmManager::create_realm(StringData realm_id, StringData realm_name)
{
    m_realm->begin_transaction();
    auto& table = *ObjectStore::table_for_object_type(m_realm->read_group(), "RealmFile");
    size_t row_ndx      = table.add_empty_row();
    size_t id_col_ndx   = table.get_column_index("id");
    size_t name_col_ndx = table.get_column_index("path");
    table.set_string(id_col_ndx, row_ndx, realm_id);
    table.set_string(name_col_ndx, row_ndx, realm_name);
    m_realm->commit_transaction();
}

GlobalNotifier::GlobalNotifier(std::unique_ptr<Callback> async_target,
                               std::string local_root_dir, std::string server_base_url,
                               std::string access_token)
: m_admin(local_root_dir, server_base_url, access_token)
, m_target(std::move(async_target))
, m_signal(std::make_shared<util::EventLoopSignal<SignalCallback>>(SignalCallback{this, &GlobalNotifier::on_change}))
{
}

GlobalNotifier::~GlobalNotifier()
{
    {
        std::unique_lock<std::mutex> l(m_work_queue_mutex);
        m_shutdown = true;
        m_work_queue_cv.notify_all();
    }
    if (m_work_thread.joinable()) // will be false if `start()` was never called
        m_work_thread.join();
}

void GlobalNotifier::start()
{
    m_admin.start([this](auto&& realm_id, auto&& realm_name) {
        register_realm(realm_id, realm_name);
    });
    m_work_thread = std::thread([this] { calculate(); });
}

void GlobalNotifier::calculate()
{
    while (true) {
        std::unique_lock<std::mutex> l(m_work_queue_mutex);
        m_work_queue_cv.wait(l, [=] { return m_shutdown || !m_work_queue.empty(); });
        if (m_shutdown)
            return;

        auto next = std::move(m_work_queue.front());
        m_work_queue.pop();
        l.unlock();

        auto& realm = *next.realm;
        auto& sg = Realm::Internal::get_shared_group(realm);

        auto config = realm.config();
        config.cache = false;
        auto realm2 = Realm::make_shared_realm(config);
        auto& sg2 = Realm::Internal::get_shared_group(*realm2);

        Group const& g = sg2.begin_read(sg.get_version_of_current_transaction());
        _impl::TransactionChangeInfo info;
        info.track_all = true;
        _impl::transaction::advance(sg2, info, next.target_version);

        std::unordered_map<std::string, CollectionChangeSet> changes;
        changes.reserve(info.tables.size());
        for (size_t i = 0; i < info.tables.size(); ++i) {
            auto& change = info.tables[i];
            if (!change.empty()) {
                auto name = ObjectStore::object_type_for_table_name(g.get_table_name(i));
                if (name) {
                    changes[name] = std::move(change).finalize();
                }
            }
        }
        if (changes.empty() && !realm.read_group().is_empty())
            continue; // nothing to notify about

        std::lock_guard<std::mutex> l2(m_deliver_queue_mutex);
        m_pending_deliveries.push({
            sg.get_version_of_current_transaction(),
            next.target_version,
            std::move(next.realm),
            std::move(changes)
        });
        m_signal->notify();
    }
}

void GlobalNotifier::register_realm(std::string const& realm_id, std::string const& realm_name)
{
    if (m_listen_entries.count(realm_id) > 0)
        return;
    if (!m_target->filter_callback(realm_name))
        return;

    auto config = m_admin.get_config(realm_id, realm_name);
    auto coordinator = _impl::RealmCoordinator::get_coordinator(config);
    m_listen_entries[realm_id] = coordinator;

    auto realm = Realm::make_shared_realm(std::move(config));
    if (realm->read_group().is_empty())
        realm = nullptr;
    else {
        std::lock_guard<std::mutex> l(m_deliver_queue_mutex);
        auto version = Realm::Internal::get_shared_group(*realm).get_version_of_current_transaction();
        m_pending_deliveries.push({{}, version, std::move(realm), {}});
        m_signal->notify();
    }

    auto unowned_coordinator = coordinator.get();
    coordinator->set_transaction_callback([this, unowned_coordinator](VersionID old_version, VersionID new_version) {
        auto config = unowned_coordinator->get_config();
        config.schema = util::none;
        auto realm = Realm::make_shared_realm(std::move(config));
        Realm::Internal::begin_read(*realm, old_version);
        REALM_ASSERT(!realm->config().schema);

        std::lock_guard<std::mutex> l(m_work_queue_mutex);
        m_work_queue.push(RealmToCalculate{std::move(realm), new_version});
        m_work_queue_cv.notify_one();
    });
}

void GlobalNotifier::on_change()
{
    while (!m_waiting) {
        GlobalNotifier::ChangeNotification change;
        {
            std::lock_guard<std::mutex> l(m_deliver_queue_mutex);
            if (m_pending_deliveries.empty())
                return;
            change = std::move(m_pending_deliveries.front());
            m_pending_deliveries.pop();
        }

        m_target->realm_changed(std::move(change));
        // FIXME: needs to actually close the notification pipe at some point
    }
}

void GlobalNotifier::pause()
{
    m_waiting = true;
}

void GlobalNotifier::resume()
{
    m_waiting = false;
    on_change();
}

bool GlobalNotifier::has_pending()
{
    std::lock_guard<std::mutex> l(m_deliver_queue_mutex);
    return !m_pending_deliveries.empty();
}

GlobalNotifier::ChangeNotification::ChangeNotification(VersionID old_version,
                                                       VersionID new_version,
                                                       SharedRealm realm,
                                                       std::unordered_map<std::string, CollectionChangeSet> changes)
: m_old_version(old_version)
, m_new_version(new_version)
, m_realm(std::move(realm))
, m_changes(std::move(changes))
{
}

SharedRealm GlobalNotifier::ChangeNotification::get_old_realm() const
{
    if (const_cast<VersionID&>(m_old_version) == VersionID{})
        return nullptr;

    auto config = m_realm->config();
    config.cache = false;
    auto old_realm = Realm::get_shared_realm(std::move(config));
    Realm::Internal::begin_read(*old_realm, m_old_version);
    return old_realm;
}

SharedRealm GlobalNotifier::ChangeNotification::get_new_realm() const
{
    auto config = m_realm->config();
    config.cache = false;
    auto new_realm = Realm::get_shared_realm(std::move(config));
    Realm::Internal::begin_read(*new_realm, m_new_version);
    return new_realm;
}