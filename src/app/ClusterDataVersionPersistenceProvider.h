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
#include <app/ConcreteClusterPath.h>
#include <app/util/basic-types.h>
#include <lib/support/CodeUtils.h>

namespace chip {
namespace app {

/**
* Interface for persisting cluster data version value.
*/

class ClusterDataVersionPersistenceProvider
{
public:
    virtual ~ClusterDataVersionPersistenceProvider() = default;
    ClusterDataVersionPersistenceProvider()          = default;

    /**
     * Write a version value from the attribute store to non-volatile memory.
     *
     * @param [in] aPath the cluster path for the version being written.
     * @param [in] aValue the data to write.  Integer is
     *             represented in native endianness. The length is
     *             stored as little-endian.
     */
    virtual CHIP_ERROR WriteValue(const ConcreteClusterPath & aPath, const chip::DataVersion & aValue) = 0;

    /**
     * Read an attribute value from non-volatile memory.
     *
     * @param [in] aPath the cluster path for the version being persisted.
     * @param [inout] aValue where to place the data.
     *
     * The data is expected to be in native endianness for integer.
     */
    virtual CHIP_ERROR ReadValue(const ConcreteClusterPath & aPath,  chip::DataVersion & aValue) = 0;
};

/**
* Instance getter for the global ClusterDataVersionPersistenceProvider.
*
* Callers have to externally synchronize usage of this function.
*
* @return The global ClusterDataVersionPersistenceProvider.  This must never be null.
*/
ClusterDataVersionPersistenceProvider * GetAttributePersistenceProvider();

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
