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

#ifndef REALM_OS_SUBSCRIPTION_STATE_HPP
#define REALM_OS_SUBSCRIPTION_STATE_HPP

namespace realm {
namespace partial_sync {

// Enum describing the various states a partial sync subscription can have.
// These states are propagated using the standard collection notification system.
enum class SubscriptionState {
    UNDEFINED = -3,         // Unknown which state Partial Sync is in.
    NOT_SUPPORTED = - 2,    // Partial Sync not supported.
    ERROR = -1,             // An error was detect in Partial Sync.
    UNINITIALIZED = 0,      // The subscription was just created, but not handled by sync yet.
    INITIALIZED = 1         // The subscription have been initialized successfully and are syncing data to the device.
};

static inline SubscriptionState status_code_to_state(int status_code) {
    switch(status_code) {
        case -3: return SubscriptionState::UNDEFINED;
        case -2: return SubscriptionState::NOT_SUPPORTED;
        case -1: return SubscriptionState::ERROR;
        case 0: return SubscriptionState::UNINITIALIZED;
        case 1: return SubscriptionState::INITIALIZED;
        default: return SubscriptionState::UNDEFINED;
    }
}

static inline int state_to_status_code(SubscriptionState state) {
    return (int) state;
}

}
}

#endif // REALM_OS_SUBSCRIPTION_STATE_HPP
