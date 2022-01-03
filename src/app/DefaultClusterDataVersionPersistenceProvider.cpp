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

#include <app/DefaultClusterDataVersionPersistenceProvider.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/DefaultStorageKeyAllocator.h>
#include <lib/support/SafeInt.h>

namespace chip {
namespace app {

CHIP_ERROR DefaultClusterDataVersionPersistenceProvider::WriteValue(const ConcreteClusterPath & aPath, const chip::DataVersion & aValue)
{
    // TODO: we may want to have a small cache for values that change a lot, so
    // we only write them once a bunch of changes happen or on timer or
    // shutdown.
    DefaultStorageKeyAllocator key;
    return mStorage.SyncSetKeyValue(key.ClusterValue(aPath), &aValue, sizeof(aValue));
}

CHIP_ERROR DefaultClusterDataVersionPersistenceProvider::ReadValue(const ConcreteClusterPath & aPath, chip::DataVersion & aValue)
{
    DefaultStorageKeyAllocator key;
    uint16_t size = sizeof(aValue);
    ReturnErrorOnFailure(mStorage.SyncGetKeyValue(key.ClusterValue(aPath), &aValue, size));
    return CHIP_NO_ERROR;
}

namespace {

ClusterDataVersionPersistenceProvider * gDataVersionSaver = nullptr;

} // anonymous namespace

ClusterDataVersionPersistenceProvider * GetClusterDataVersionPersistenceProvider()
{
    return gDataVersionSaver;
}

void SetClusterDataVersionPersistenceProvider(ClusterDataVersionPersistenceProvider * aProvider)
{
    if (aProvider != nullptr)
    {
        gDataVersionSaver = aProvider;
    }
}

} // namespace app
} // namespace chip