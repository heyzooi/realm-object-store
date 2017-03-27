////////////////////////////////////////////////////////////////////////////
//
// Copyright 2017 Realm Inc.
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

#include "sync_permission.hpp"
#include "sync_user.hpp"
#include "sync_config.hpp"
#include "sync_manager.hpp"
#include "sync_session.hpp"
#include "object_schema.hpp"
#include "impl/object_accessor_impl.hpp"

#if REALM_PLATFORM_APPLE
#include <realm/util/cf_ptr.hpp>
#else
#include <sole.hpp>
#endif

#include "property.hpp"

using namespace realm;

namespace {

// TODO: this should already exist somewhere.
struct MallocBuffer {
    void* buffer_ptr;

    MallocBuffer(size_t bytes)
    : buffer_ptr(malloc(bytes))
    {
        if (!buffer_ptr)
            throw std::runtime_error("Could not successfully malloc memory.");
    }

    ~MallocBuffer()
    {
        free(buffer_ptr);
    }

    template<typename T>
    T* get()
    {
        return static_cast<T*>(buffer_ptr);
    }
};

std::string make_uuid()
{
#if REALM_PLATFORM_APPLE
    CFPtr<CFUUIDRef> uuid = adoptCF(CFUUIDCreate(NULL));
    CFPtr<CFStringRef> uuid_str = adoptCF(CFUUIDCreateString(NULL, uuid.get()));
    size_t buffer_len = CFStringGetLength(uuid_str.get()) + 1;
    MallocBuffer buffer(buffer_len);
    CFStringGetCString(uuid_str.get(), buffer.get<char>(), buffer_len, kCFStringEncodingASCII);
    std::string uuid_cpp_str = buffer.get<char>();
    return uuid_cpp_str;
#else
    return sole::uuid4().str();
#endif
}

}

size_t PermissionResults::size() {
    REALM_ASSERT(m_results->size());
    return m_results->size() - 1;
}

#pragma mark - Permission

Permission::Permission(const Permission& p)
: path(p.path)
, access(p.access)
, condition(p.condition)
{ }

Permission::Permission(std::string path, AccessLevel access, Condition condition)
: path(std::move(path))
, access(std::move(access))
, condition(std::move(condition))
{ }

Permission& Permission::operator=(const Permission& p)
{
    if (&p != this) {
        path = p.path;
        access = p.access;
        condition = p.condition;
    }
    return *this;
}

Permission::Condition& Permission::Condition::operator=(const Permission::Condition& c)
{
    if (&c != this) {
        type = c.type;
        switch (type) {
            case Type::UserId:
                user_id = c.user_id;
                break;
            case Type::KeyValue:
                key_value = c.key_value;
                break;
        }
    }
    return *this;
}

Permission::Condition::Condition(const Permission::Condition& c)
: type(c.type)
{
    switch (type) {
        case Type::UserId:
            user_id = c.user_id;
            break;
        case Type::KeyValue:
            key_value = c.key_value;
            break;
    }
}

Permission::Condition::~Condition()
{
    switch (type) {
        case Type::UserId:
            user_id.std::basic_string<char>::~basic_string<char>();
            break;
        case Type::KeyValue:
            std::get<0>(key_value).std::basic_string<char>::~basic_string<char>();
            std::get<1>(key_value).std::basic_string<char>::~basic_string<char>();
            break;
    }
}

#pragma mark - PermissionResults

const Permission PermissionResults::get(size_t index) {
    Object permission(m_results->get_realm(), m_results->get_object_schema(), m_results->get(index + 1));
    Permission::AccessLevel level = Permission::AccessLevel::None;
    CppContext context;

    auto may_manage = permission.get_property_value<util::Any>(&context, "mayManage");
    if (may_manage.has_value() && any_cast<bool>(may_manage)) level = Permission::AccessLevel::Admin;
    else if (any_cast<bool>(permission.get_property_value<util::Any>(&context, "mayWrite"))) level = Permission::AccessLevel::Write;
    else if (any_cast<bool>(permission.get_property_value<util::Any>(&context, "mayRead"))) level = Permission::AccessLevel::Read;

    std::string path = any_cast<std::string>(permission.get_property_value<util::Any>(&context, "path"));
    std::string userId = any_cast<std::string>(permission.get_property_value<util::Any>(&context, "userId"));
    REALM_ASSERT(path != std::string("/") + userId + "/__permissions");
    return { path, level, { userId } };
}

PermissionResults PermissionResults::filter(Query&& q) const {
    throw new std::runtime_error("not yet supported");
}

#pragma mark - Permissions

void Permissions::get_permissions(std::shared_ptr<SyncUser> user,
                                  std::function<void (std::unique_ptr<PermissionResults>, std::exception_ptr)> callback,
                                  ConfigMaker make_config) {
    auto realm = Permissions::permission_realm(user, make_config);
    auto session = SyncManager::shared().get_session(realm->config().path, *realm->config().sync_config);

    // FIXME - download api would accomplish this in a safer way without relying on the fact that
    // m_results is only downloaded once it contains and entry for __permission which we subsequently hide
    struct ResultsNotificationWrapper {
        std::unique_ptr<Results> results;
        NotificationToken token;
    };
    std::shared_ptr<ResultsNotificationWrapper> results_notification = std::make_shared<ResultsNotificationWrapper>();
    results_notification->results = std::make_unique<Results>(realm, *ObjectStore::table_for_object_type(realm->read_group(), "Permission"));
    results_notification->token = results_notification->results->async([results_notification, callback] (auto ex) mutable {
        if (ex) {
            callback(nullptr, ex);
            results_notification.reset();
        }
        else if (results_notification->results->size() > 0) {
            callback(std::make_unique<PermissionResults>(std::move(results_notification->results)), nullptr);
            results_notification.reset();
        }
    });
}

void Permissions::set_permission(std::shared_ptr<SyncUser> user,
                                 Permission permission,
                                 PermissionChangeCallback callback,
                                 ConfigMaker make_config) {
    auto realm = Permissions::management_realm(user, make_config);
    realm->begin_transaction();

    // use this to keep object and notification tokens alive until callback has fired
    struct ObjectNotification {
        Object object;
        NotificationToken token;
    };
    std::shared_ptr<ObjectNotification> object_notification = std::make_shared<ObjectNotification>();

    CppContext context;
    object_notification->object = Object::create<util::Any>(&context, realm, *realm->schema().find("PermissionChange"), AnyDict{
        {"id", make_uuid()},
        {"createdAt", Timestamp(0, 0)},
        {"updatedAt", Timestamp(0, 0)},
        {"userId", permission.condition.user_id},
        {"realmUrl", user->server_url() + permission.path},
        {"mayRead", permission.access != Permission::AccessLevel::None},
        {"mayWrite", permission.access == Permission::AccessLevel::Write || permission.access == Permission::AccessLevel::Admin},
        {"mayManage", permission.access == Permission::AccessLevel::Admin},
    }, false);

    realm->commit_transaction();

    object_notification->token = object_notification->object.add_notification_block(
        [object_notification, callback] (CollectionChangeSet, std::exception_ptr ex) mutable {
            if (ex) {
                callback(ex);
                object_notification.reset();
                return;
            }

            CppContext context;
            auto statusCode = object_notification->object.get_property_value<util::Any>(&context, "statusCode");
            if (statusCode.has_value()) {
                auto code = any_cast<long long>(statusCode);
                std::exception_ptr exc_ptr = nullptr;
                if (code) {
                    auto status = object_notification->object.get_property_value<util::Any>(&context, "statusMessage");
                    std::string error_str = status.has_value() ? any_cast<std::string>(status) :
                        std::string("Error code: ") + std::to_string(code);
                    exc_ptr = std::make_exception_ptr(std::runtime_error(error_str));
                }
                callback(exc_ptr);
                object_notification.reset();
            }
        }
    );
}

void Permissions::delete_permission(std::shared_ptr<SyncUser> user,
                                    Permission permission,
                                    PermissionChangeCallback callback,
                                    ConfigMaker make_config) {
    permission.access = Permission::AccessLevel::None;
    set_permission(user, permission, callback, make_config);
}

SharedRealm Permissions::management_realm(std::shared_ptr<SyncUser> user, ConfigMaker make_config) {
    Realm::Config config = make_config(user, std::string("realm") + user->server_url().substr(4) + "/~/__management");
    config.schema = Schema{
        {"PermissionChange", {
            {"id", PropertyType::String, "", "", true, true, false},
            {"createdAt", PropertyType::Date, "", "", false, false, false},
            {"updatedAt", PropertyType::Date, "", "", false, false, false},
            {"statusCode", PropertyType::Int, "", "", false, false, true},
            {"statusMessage", PropertyType::String, "", "", false, false, true},
            {"userId", PropertyType::String, "", "", false, false, false},
            {"realmUrl", PropertyType::String, "", "", false, false, false},
            {"mayRead", PropertyType::Bool, "", "", false, false, true},
            {"mayWrite", PropertyType::Bool, "", "", false, false, true},
            {"mayManage", PropertyType::Bool, "", "", false, false, true},
        }}
    };
    config.schema_version = 0;
    return Realm::get_shared_realm(std::move(config));
}

SharedRealm Permissions::permission_realm(std::shared_ptr<SyncUser> user, ConfigMaker make_config) {
    Realm::Config config = make_config(user, std::string("realm") + user->server_url().substr(4) + "/~/__permission");
    config.schema = Schema{
        {"Permission", {
            {"updatedAt", PropertyType::Date, "", "", false, false, false},
            {"userId", PropertyType::String, "", "", false, false, false},
            {"path", PropertyType::String, "", "", false, false, false},
            {"mayRead", PropertyType::Bool, "", "", false, false, false},
            {"mayWrite", PropertyType::Bool, "", "", false, false, false},
            {"mayManage", PropertyType::Bool, "", "", false, false, false},
        }}
    };
    config.schema_version = 0;
    return Realm::get_shared_realm(std::move(config));
}
