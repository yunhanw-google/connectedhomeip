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
 *      This file provides the platform's implementation of the BluezBlePlatformDelegate
 *
 *      The BluezBlePlatformDelegate provides the Weave stack with an interface
 *      by which to form and cancel GATT subscriptions, read and write
 *      GATT characteristic values, send GATT characteristic notifications,
 *      respond to GATT read requests, and close Ble connections.
 */

#include <support/CodeUtils.h>
#include "BluezBlePlatformDelegate.h"

#if CONFIG_BLE_PLATFORM_BLUEZ

namespace chip {
namespace DeviceLayer {
namespace Internal {

BluezBlePlatformDelegate::BluezBlePlatformDelegate(Ble::BleLayer * Ble) : Ble(Ble), SendIndicationCb(NULL), GetMTUCb(NULL) { };

uint16_t BluezBlePlatformDelegate::GetMTU(BLE_CONNECTION_OBJECT connObj) const
{
    uint16_t mtu = 0;
    if (GetMTUCb != NULL)
    {
        mtu = GetMTUCb((void *) connObj);
    }
    return mtu;
}

bool BluezBlePlatformDelegate::SubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                       const Ble::ChipBleUUID * charId)
{
    ChipLogError(Ble, "SubscribeCharacteristic: Not implemented");
    return true;
}

bool BluezBlePlatformDelegate::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                         const Ble::ChipBleUUID * charId)
{
    ChipLogError(Ble, "UnsubscribeCharacteristic: Not implemented");
    return true;
}

bool BluezBlePlatformDelegate::CloseConnection(BLE_CONNECTION_OBJECT connObj)
{
    ChipLogError(Ble, "CloseConnection: Not implemented");
    return true;
}

bool BluezBlePlatformDelegate::SendIndication(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                              const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    bool rc = true;
    ChipLogDetail(Ble, "Start of SendIndication");

    if (SendIndicationCb)
    {
        rc = SendIndicationCb((void *) connObj, pBuf);
    }

    return rc;
}

bool BluezBlePlatformDelegate::SendWriteRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                                const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    ChipLogError(Ble, "SendWriteRequest: Not implemented");
    return true;
}

bool BluezBlePlatformDelegate::SendReadRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                               const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    ChipLogError(Ble, "SendReadRequest: Not implemented");
    return true;
}

bool BluezBlePlatformDelegate::SendReadResponse(BLE_CONNECTION_OBJECT connObj, BLE_READ_REQUEST_CONTEXT requestContext,
                                                const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId)
{
    ChipLogError(Ble, "SendReadResponse: Not implemented");
    return true;
}

void BluezBlePlatformDelegate::SetSendIndicationCallback(SendIndicationCallback cb)
{
    SendIndicationCb = cb;
}

void BluezBlePlatformDelegate::SetGetMTUCallback(GetMTUCallback cb)
{
    GetMTUCb = cb;
}

chip::System::Error BluezBlePlatformDelegate::SendToWeaveThread(InEventParam * aParams)
{
    aParams->Ble = Ble;

    return Ble->ScheduleWork(HandleBleDelegate, aParams);
}

void BluezBlePlatformDelegate::HandleBleDelegate(chip::System::Layer * aLayer, void * aAppState,
                                                 chip::System::Error aError)
{
    InEventParam * args = static_cast<InEventParam *>(aAppState);

    VerifyOrExit(args != NULL, );

    switch (args->EventType)
    {

    case InEventParam::EventTypeEnum::kEvent_IndicationConfirmation:
        if (!args->Ble->HandleIndicationConfirmation((void *)(args->ConnectionObject), args->IndicationConfirmation.SvcId,
                                                     args->IndicationConfirmation.CharId))
        {
            ChipLogError(Ble, "HandleIndicationConfirmation failed");
        }
        break;

    case InEventParam::EventTypeEnum::kEvent_SubscribeReceived:
        if (!args->Ble->HandleSubscribeReceived(args->ConnectionObject, args->SubscriptionChange.SvcId,
                                                args->SubscriptionChange.CharId))
        {
            ChipLogError(Ble, "HandleSubscribeReceived failed");
        }
        break;

    case InEventParam::EventTypeEnum::kEvent_UnsubscribeReceived:
        if (!args->Ble->HandleUnsubscribeReceived(args->ConnectionObject, args->SubscriptionChange.SvcId,
                                                  args->SubscriptionChange.CharId))
        {
            ChipLogError(Ble, "HandleUnsubscribeReceived failed");
        }
        break;

    case InEventParam::EventTypeEnum::kEvent_ConnectionError:
        args->Ble->HandleConnectionError(args->ConnectionObject, args->ConnectionError.mErr);
        break;

    case InEventParam::EventTypeEnum::kEvent_WriteReceived:
        if (!args->Ble->HandleWriteReceived(args->ConnectionObject, args->WriteReceived.SvcId, args->WriteReceived.CharId,
                                            args->WriteReceived.MsgBuf))
        {
            ChipLogError(Ble, "HandleWriteReceived failed");
        }
        args->WriteReceived.MsgBuf = NULL;
        break;

    default:
        ChipLogError(Ble, "Unknown or unimplemented event: %d", args->EventType);
        break;
    }

exit:
    if (args != NULL)
    {
        args->PlatformDelegate->ReleaseEventParams(args);
    }
}

chip::System::Error BluezBlePlatformDelegate::NewEventParams(InEventParam ** aParam)
{
    *aParam = new InEventParam();
    (*aParam)->PlatformDelegate = this;

    return CHIP_SYSTEM_NO_ERROR;
}

void BluezBlePlatformDelegate::ReleaseEventParams(InEventParam * aParam)
{
    if (aParam != NULL)
    {
        delete aParam;
    }
}

} // namespace BlueZ
} // namespace Platform
} // namespace Ble

#endif /* CONFIG_BLE_PLATFORM_BLUEZ */
