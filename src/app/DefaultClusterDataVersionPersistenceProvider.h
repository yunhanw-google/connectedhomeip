/*
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
#pragma once

#include <app/ClusterDataVersionPersistenceProvider.h>
#include <lib/core/CHIPPersistentStorageDelegate.h>
#include <lib/support/DefaultStorageKeyAllocator.h>

namespace chip {
namespace app {

/**
* Default implementation of ClusterDataVersionPersistenceProvider.  This uses
* PersistentStorageDelegate to store the version.
*
* NOTE: SetAttributePersistenceProvider must still be called with an instance
* of this class, since it can't be constructed automatically without knowing
* what PersistentStorageDelegate is to be used.
*/
class DefaultClusterDataVersionPersistenceProvider : public ClusterDataVersionPersistenceProvider
{
public:
    // aStorage must outlive this object.
    DefaultClusterDataVersionPersistenceProvider(PersistentStorageDelegate & aStorage) : mStorage(aStorage) {}

    // AttributePersistenceProvider implementation.
    CHIP_ERROR WriteValue(const ConcreteClusterPath & aPath, const chip::DataVersion & aValue) override;
    CHIP_ERROR ReadValue(const ConcreteClusterPath & aPath, chip::DataVersion & aValue) override;

private:
    PersistentStorageDelegate & mStorage;
};

/**
 * Instance getter for the global ClusterDataVersionPersistenceProvider.
 *
 * Callers have to externally synchronize usage of this function.
 *
 * @return The global ClusterDataVersionPersistenceProvider.  This must never be null.
 */
ClusterDataVersionPersistenceProvider * GetClusterDataVersionPersistenceProvider();

/**
 * Instance setter for the global ClusterDataVersionPersistenceProvider.
 *
 * Callers have to externally synchronize usage of this function.
 *
 * If the `provider` is nullptr, the value is not changed.
 *
 * @param[in] provider the ClusterDataVersionPersistenceProvider implementation to use.
 */
void SetClusterDataVersionPersistenceProvider(ClusterDataVersionPersistenceProvider * aProvider);

} // namespace app
} // namespace chip