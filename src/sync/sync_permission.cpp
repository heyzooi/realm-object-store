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

#include <sole.hpp>

#include "property.hpp"

using namespace realm;

size_t PermissionResults::size() {
    return m_results->size();
}

const Permission PermissionResults::get(size_t index) {
    Object permission(m_results->get_realm(), m_results->get_object_schema(), m_results->get(index));
    Permission::AccessLevel level = Permission::AccessLevel::None;
    CppContext context;

    if (any_cast<bool>(permission.get_property_value<util::Any>(&context, "mayMange"))) level = Permission::AccessLevel::Admin;
    else if (any_cast<bool>(permission.get_property_value<util::Any>(&context, "mayWrite"))) level = Permission::AccessLevel::Write;
    else if (any_cast<bool>(permission.get_property_value<util::Any>(&context, "mayRead"))) level = Permission::AccessLevel::Read;
    return {
        any_cast<std::string>(permission.get_property_value<util::Any>(&context, "path")), level, {
            any_cast<std::string>(permission.get_property_value<util::Any>(&context, "userId"))
        }
    };
}

PermissionResults PermissionResults::filter(Query&& q) const {
    throw new std::runtime_error("not yet supported");
}

void Permissions::get_permissions(std::shared_ptr<SyncUser> user,
                                  std::function<void (std::unique_ptr<PermissionResults>, std::exception_ptr)> callback,
                                  ConfigMaker make_config) {
    auto realm = Permissions::permission_realm(user, make_config);
    auto results = std::make_unique<Results>(realm, *ObjectStore::table_for_object_type(realm->read_group(), "Permission"));
}

void Permissions::set_permission(std::shared_ptr<SyncUser> user,
                                 Permission permission,
                                 PermissionChangeCallback callback,
                                 ConfigMaker make_config) {
    auto realm = Permissions::management_realm(user, make_config);
    realm->begin_transaction();

    CppContext context;
    auto object = Object::create<util::Any>(&context, realm, *realm->schema().find("PermissionChange"), AnyDict{
        {"id", sole::uuid4().str()},
        {"createdAt", Timestamp(0, 0)},
        {"updatedAt", Timestamp(0, 0)},
        {"userId", permission.condition.user_id},
        {"realmUrl", user->server_url() + permission.path},
        {"mayRead", permission.access != Permission::AccessLevel::None},
        {"mayWrite", permission.access == Permission::AccessLevel::Write || permission.access == Permission::AccessLevel::Admin},
        {"mayMange", permission.access == Permission::AccessLevel::Admin},
    }, false);

    realm->commit_transaction();

    NotificationToken *token = new NotificationToken();
    *token = object.add_notification_block([&token, object, callback]
                                           (CollectionChangeSet, std::exception_ptr ex) mutable {
        if (ex) {
            callback(ex);
            
        }
        CppContext context;
        std::cout << any_cast<int>(object.get_property_value<util::Any>(&context, "statusCode")) << std::endl;
    });
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