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
 *      This file defines BluezBlePlatformDelegate class
 *
 *      The BluezBlePlatformDelegate provides the Weave stack with an interface
 *      by which to form and cancel GATT subscriptions, read and write
 *      GATT characteristic values, send GATT characteristic notifications,
 *      respond to GATT read requests, and close BLE connections.
 */

#ifndef BLUEZBLEPLATFORMDELEGATE_H_
#define BLUEZBLEPLATFORMDELEGATE_H_

#include <BuildConfig.h>

#if CONFIG_BLE_PLATFORM_BLUEZ

#include <ble/BleLayer.h>
#include <ble/BleUUID.h>
#include <ble/BlePlatformDelegate.h>
#include <support/CodeUtils.h>
#include <stdbool.h>
#include <stdint.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

typedef bool (*SendIndicationCallback)(void * data, chip::System::PacketBuffer * msgBuf);

typedef uint16_t (*GetMTUCallback)(void * connObj);

class BluezBlePlatformDelegate;

struct InEventParam
{
    enum EventTypeEnum
    {
        kEvent_IndicationConfirmation,
        kEvent_SubscribeReceived,
        kEvent_UnsubscribeReceived,
        kEvent_ConnectionError,
        kEvent_WriteReceived
    };

    EventTypeEnum EventType;
    void * ConnectionObject;
    Ble::BleLayer * Ble;
    BluezBlePlatformDelegate *PlatformDelegate;
    union
    {
        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
        } IndicationConfirmation;

        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
        } SubscriptionChange;

        struct
        {
            BLE_ERROR mErr;
        } ConnectionError;

        struct
        {
            const chip::Ble::ChipBleUUID * SvcId;
            const chip::Ble::ChipBleUUID * CharId;
            chip::System::PacketBuffer * MsgBuf;
        } WriteReceived;
    };
};

class BluezBlePlatformDelegate : public Ble::BlePlatformDelegate
{
public:
    Ble::BleLayer * Ble;
    SendIndicationCallback SendIndicationCb;
    GetMTUCallback GetMTUCb;

    BluezBlePlatformDelegate(Ble::BleLayer * ble);

    uint16_t GetMTU(BLE_CONNECTION_OBJECT connObj) const;

    bool SubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                 const Ble::ChipBleUUID * charId);

    bool UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                   const Ble::ChipBleUUID * charId);

    bool CloseConnection(BLE_CONNECTION_OBJECT connObj);

    bool SendIndication(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                        chip::System::PacketBuffer * pBuf);

    bool SendWriteRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                          chip::System::PacketBuffer * pBuf);

    bool SendReadRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                         chip::System::PacketBuffer * pBuf);

    bool SendReadResponse(BLE_CONNECTION_OBJECT connObj, BLE_READ_REQUEST_CONTEXT requestContext,
                          const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId);

    void SetSendIndicationCallback(SendIndicationCallback cb);

    void SetGetMTUCallback(GetMTUCallback cb);

    chip::System::Error SendToWeaveThread(InEventParam * aParams);

    chip::System::Error NewEventParams(InEventParam ** aParam);

    void ReleaseEventParams(InEventParam * aParam);

    static void HandleBleDelegate(chip::System::Layer * aLayer, void * aAppState, chip::System::Error aError);
};

} // namespace BlueZ
} // namespace Platform
} // namespace Ble

#endif /* CONFIG_BLE_PLATFORM_BLUEZ */

#endif /* BLUEZBLEPLATFORMDELEGATE_H_ */
