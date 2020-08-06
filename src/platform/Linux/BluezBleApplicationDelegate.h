/*
 *
 *    Copyright (c) 2017-2018 Nest Labs, Inc.
 *    All rights reserved.
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
/**
 *    @file
 *      This file defines BluezBleApplicationDelegate class
 *
 *      BluezBleApplicationDelegate provides the interface for Chip
 *      to inform the application regarding activity within the WoBluez
 *      layer
 */

#ifndef BLUEZBLEAPPLICATIONDELEGATE_H_
#define BLUEZBLEAPPLICATIONDELEGATE_H_

#include <BuildConfig.h>

#if CONFIG_BLE_PLATFORM_BLUEZ

#include <BleLayer/BleLayer.h>
#include <BleLayer/BleApplicationDelegate.h>

using namespace ::nl;

namespace chip {
namespace DeviceLayer {
namespace Internal {

// Enum to represent BLE activities
typedef enum
{
    kBleConnect = 0,
    kBleDisconnect,
    kWoBlePktTx,
    kWoBlePktRx
} BleActivity;

class BluezBleApplicationDelegate : public nl::Ble::BleApplicationDelegate
{
public:
    BluezBleApplicationDelegate() {};
    virtual ~BluezBleApplicationDelegate() {};

    // Application can use this callback to get notification regarding
    // BLE activity
    virtual void NotifyBleActivity(BleActivity bleActivity) {};

    // BleApplicationDelegate function overrides
    // Application can use this callback to get notification regarding
    // Chip connection closure
    virtual void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT connObj);
};

} // namespace BlueZ
} // namespace Platform
} // namespace Ble

#endif /* CONFIG_BLE_PLATFORM_BLUEZ */

#endif /* BLUEZBLEAPPLICATIONDELEGATE_H_ */
