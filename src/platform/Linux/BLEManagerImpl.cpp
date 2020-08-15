/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2018 Nest Labs, Inc.
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
 *          Provides an implementation of the BLEManager singleton object
 *          for Linux platforms.
 */
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <ble/CHIPBleServiceData.h>
#include <new>
#include <platform/internal/BLEManager.h>
#include <support/CodeUtils.h>
#include "CHIPBluezHelper.h"

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

using namespace ::nl;
using namespace ::chip::Ble;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {
const uint8_t UUID_CHIPoBLEService[]       = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                         0x00, 0x10, 0x00, 0x00, 0xAF, 0xFE, 0x00, 0x00 };
const uint8_t ShortUUID_CHIPoBLEService[]  = { 0xAF, 0xFE };
const ChipBleUUID ChipUUID_CHIPoBLEChar_RX = { { 0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59, 0x95, 0x9F, 0x4F, 0x9C, 0x42, 0x9F,
                                                 0x9D, 0x11 } };
const ChipBleUUID ChipUUID_CHIPoBLEChar_TX = { { 0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59, 0x95, 0x9F, 0x4F, 0x9C, 0x42, 0x9F,
                                                 0x9D, 0x12 } };

} // namespace

BLEManagerImpl BLEManagerImpl::sInstance;

static int CloseBleconnectionCB(void *aArg);

static int CloseBleconnectionCB(void *aArg)
{
    //CloseBleconnection();

    return G_SOURCE_REMOVE;
}

uint8_t BLEManagerImpl::GetAdvertisingHandle(void)
{
    return mAdvHandle;
}

void BLEManagerImpl::SetAdvertisingHandle(uint8_t handle)
{
    mAdvHandle = handle;
}

BleLayer * BLEManagerImpl::_GetBleLayer() const
{
    return (BleLayer *) (this);
}

BLEManager::CHIPoBLEServiceMode BLEManagerImpl::_GetCHIPoBLEServiceMode(void)
{
    return mServiceMode;
}

bool BLEManagerImpl::_IsAdvertisingEnabled(void)
{
    return GetFlag(mFlags, kFlag_AdvertisingEnabled);
}

bool BLEManagerImpl::_IsFastAdvertisingEnabled(void)
{
    return GetFlag(mFlags, kFlag_FastAdvertisingEnabled);
}

bool BLEManagerImpl::_IsAdvertising(void)
{
    return GetFlag(mFlags, kFlag_Advertising);
}

CHIP_ERROR BLEManagerImpl::_Init()
{
    CHIP_ERROR err;
    char *sBluezBleAddr;
    // Initialize the CHIP BleLayer.
    //err = sBle.Init(this, this, &SystemLayer);
    err = BleLayer::Init(this, this, &SystemLayer);
    SuccessOrExit(err);

    mServiceMode          = ConnectivityManager::kCHIPoBLEServiceMode_Enabled;
    mFlags                = kFlag_AdvertisingEnabled;
    memset(mDeviceName, 0, sizeof(mDeviceName));
    mAppState = this;
    OnChipBleConnectReceived = HandleIncomingBleConnection;
    ChipLogProgress(DeviceLayer, "start ble init");
    PlatformMgr().ScheduleWork(DriveBLEState, 0);

exit:
    return err;
}

void HandleIncomingBleConnection(BLEEndPoint *bleEP)
{
    ChipLogProgress(DeviceLayer, "WoBle con rcvd");
}

CHIP_ERROR BLEManagerImpl::_SetCHIPoBLEServiceMode(CHIPoBLEServiceMode val)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(val != ConnectivityManager::kCHIPoBLEServiceMode_NotSupported, err = CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrExit(mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_NotSupported, err = CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);

    if (val != mServiceMode)
    {
        mServiceMode = val;
        PlatformMgr().ScheduleWork(DriveBLEState, 0);
    }

exit:
    return err;
}

CHIP_ERROR BLEManagerImpl::_SetAdvertisingEnabled(bool val)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    ChipLogProgress(DeviceLayer, "_SetAdvertisingEnabled");
    if (GetFlag(mFlags, kFlag_AdvertisingEnabled) != val)
    {
        ChipLogProgress(DeviceLayer, "_SetAdvertisingEnabled1");
        SetFlag(mFlags, kFlag_AdvertisingEnabled, val);

    }
    PlatformMgr().ScheduleWork(DriveBLEState, 0);

exit:
    return err;
}

CHIP_ERROR BLEManagerImpl::_SetFastAdvertisingEnabled(bool val)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_NotSupported, err = CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);

    if (GetFlag(mFlags, kFlag_FastAdvertisingEnabled) != val)
    {
        SetFlag(mFlags, kFlag_FastAdvertisingEnabled, val);
        PlatformMgr().ScheduleWork(DriveBLEState, 0);
    }

exit:
    return err;
}

CHIP_ERROR BLEManagerImpl::_GetDeviceName(char * buf, size_t bufSize)
{
    if (strlen(mDeviceName) >= bufSize)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    strcpy(buf, mDeviceName);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::_SetDeviceName(const char * deviceName)
{
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_NotSupported)
    {
        return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
    }
    if (deviceName != NULL && deviceName[0] != 0)
    {
        if (strlen(deviceName) >= kMaxDeviceNameLength)
        {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        strcpy(mDeviceName, deviceName);
        SetFlag(mFlags, kFlag_UseCustomDeviceName);
    }
    else
    {
        mDeviceName[0] = 0;
        ClearFlag(mFlags, kFlag_UseCustomDeviceName);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::StartAdvertising(void)
{
    StartBluezAdv(mpAppState);
}

CHIP_ERROR BLEManagerImpl::StopAdvertising(void)
{
    StopBluezAdv(mpAppState);
}

void BLEManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event) {
    switch (event->Type)
    {
    case DeviceEventType::kCHIPoBLESubscribe:
        HandleSubscribeReceived(event->CHIPoBLESubscribe.ConId, &CHIP_BLE_SVC_ID, &ChipUUID_CHIPoBLEChar_TX);
        {
            ChipDeviceEvent connectionEvent;
            connectionEvent.Type = DeviceEventType::kCHIPoBLEConnectionEstablished;
            PlatformMgr().PostEvent(&connectionEvent);
        }
        break;

    case DeviceEventType::kCHIPoBLEUnsubscribe:
        HandleUnsubscribeReceived(event->CHIPoBLEUnsubscribe.ConId, &CHIP_BLE_SVC_ID, &ChipUUID_CHIPoBLEChar_TX);
        break;

    case DeviceEventType::kCHIPoBLEWriteReceived:
        HandleWriteReceived(event->CHIPoBLEWriteReceived.ConId, &CHIP_BLE_SVC_ID, &ChipUUID_CHIPoBLEChar_RX,
                            event->CHIPoBLEWriteReceived.Data);
        break;

    case DeviceEventType::kCHIPoBLEIndicateConfirm:
        HandleIndicationConfirmation(event->CHIPoBLEIndicateConfirm.ConId, &CHIP_BLE_SVC_ID, &ChipUUID_CHIPoBLEChar_TX);
        break;

    case DeviceEventType::kCHIPoBLEConnectionError:
        HandleConnectionError(event->CHIPoBLEConnectionError.ConId, event->CHIPoBLEConnectionError.Reason);
        break;

    case DeviceEventType::kFabricMembershipChange:
    case DeviceEventType::kServiceProvisioningChange:
    case DeviceEventType::kAccountPairingChange:

        // If CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED is enabled, and there is a change to the
        // device's provisioning state, then automatically disable CHIPoBLE advertising if the device
        // is now fully provisioned.
#if CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED
        if (ConfigurationMgr().IsFullyProvisioned())
        {
            ClearFlag(mFlags, kFlag_AdvertisingEnabled);
            ChipLogProgress(DeviceLayer, "CHIPoBLE advertising disabled because device is fully provisioned");
        }
#endif // CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED

        // Force the advertising configuration to be refreshed to reflect new provisioning state.
        ClearFlag(mFlags, kFlag_AdvertisingConfigured);

        DriveBLEState();

    default:
        break;
    }
}

uint16_t BLEManagerImpl::_NumConnections(void)
{
    ChipLogProgress(DeviceLayer, "%s", __FUNCTION__);
    return 0;
}

uint16_t BLEManagerImpl::GetMTU(BLE_CONNECTION_OBJECT conId) const
{
    BluezConnection * con = static_cast<BluezConnection *>(conId);
    return (con != NULL) ? con->mMtu : 0;
}

bool BLEManagerImpl::SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId,
                                                       const ChipBleUUID * charId)
{
    ChipLogError(DeviceLayer, "BLEManagerImpl::SubscribeCharacteristic() not supported");
    return true;
}

bool BLEManagerImpl::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId,
                                                         const ChipBleUUID * charId)
{
    ChipLogError(DeviceLayer, "BLEManagerImpl::UnsubscribeCharacteristic() not supported");
    return true;
}

bool BLEManagerImpl::CloseConnection(BLE_CONNECTION_OBJECT conId)
{
    CHIP_ERROR err;

    bool status = true;

    ChipLogProgress(DeviceLayer, "Closing BLE GATT connection (con %u)", conId);

    status = BluezRunOnBluezThread(CloseBleconnectionCB, NULL);
    if (!status)
    {
        ChipLogError(Ble, "Failed to schedule CloseBleconnection() on chipobluez thread");
    }

    // Force a refresh of the advertising state.
    //SetFlag(mFlags, kFlag_AdvertisingRefreshNeeded);
    //ClearFlag(mFlags, kFlag_AdvertisingConfigured);
    //PlatformMgr().ScheduleWork(DriveBLEState, 0);

    return (err == CHIP_NO_ERROR);
}

bool BLEManagerImpl::SendIndication(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId,
                                              const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    bool rc = true;
    ChipLogDetail(Ble, "Start of SendIndication");

    rc = WoBLEz_ScheduleSendIndication((void *) conId, pBuf);

    return rc;
}

bool BLEManagerImpl::SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                                const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    ChipLogError(Ble, "SendWriteRequest: Not implemented");
    return true;
}

bool BLEManagerImpl::SendReadRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                               const Ble::ChipBleUUID * charId, chip::System::PacketBuffer * pBuf)
{
    ChipLogError(Ble, "SendReadRequest: Not implemented");
    return true;
}

bool BLEManagerImpl::SendReadResponse(BLE_CONNECTION_OBJECT conId, BLE_READ_REQUEST_CONTEXT requestContext,
                                                const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId)
{
    ChipLogError(Ble, "SendReadResponse: Not implemented");
    return true;
}

void BLEManagerImpl::WoBLEz_NewConnection(void * data)
{
    ChipLogProgress(Ble, "WoBLEz_NewConnection: %p", data);
}

void BLEManagerImpl::WoBLEz_WriteReceived(void * data, const uint8_t * value, size_t len)
{
    CHIP_ERROR err     = CHIP_NO_ERROR;
    PacketBuffer * buf = NULL;

    ChipLogProgress(Ble, "Write request received for CHIPoBLE RX characteristic");

    // Copy the data to a PacketBuffer.
    buf = PacketBuffer::New();
    VerifyOrExit(buf != NULL, err = CHIP_ERROR_NO_MEMORY);
    VerifyOrExit(buf->AvailableDataLength() >= len, err = CHIP_ERROR_BUFFER_TOO_SMALL);
    memcpy(buf->Start(), value, len);
    buf->SetDataLength(len);

    // Post an event to the Chip queue to deliver the data into the Chip stack.
    {
        ChipDeviceEvent event;
        event.Type                        = DeviceEventType::kCHIPoBLEWriteReceived;
        event.CHIPoBLEWriteReceived.ConId = data;
        event.CHIPoBLEWriteReceived.Data  = buf;
        PlatformMgr().PostEvent(&event);
        buf = NULL;
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "HandleRXCharWrite() failed: %s", ErrorStr(err));
    }

    if (NULL != buf)
    {
        chip::System::PacketBuffer::Free(buf);
    }
}

void BLEManagerImpl::WoBLEz_ConnectionClosed(void * data)
{
    ChipLogProgress(DeviceLayer, "BLE GATT connection closed");

    // If this was a CHIPoBLE connection, release the associated connection state record
    // and post an event to deliver a connection error to the CHIPoBLE layer.

    {
        ChipDeviceEvent event;
        event.Type                          = DeviceEventType::kCHIPoBLEConnectionError;
        event.CHIPoBLEConnectionError.ConId = data;
        event.CHIPoBLEConnectionError.Reason = BLE_ERROR_REMOTE_DEVICE_DISCONNECTED;
        PlatformMgr().PostEvent(&event);
    }
}

void BLEManagerImpl::WoBLEz_SubscriptionChange(void * data)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    BluezConnection * connection   = static_cast<BluezConnection *>(data);
    const char * msg               = NULL;

    VerifyOrExit(connection != NULL, msg = "connection is NULL in WoBLEz_SubscriptionChange");
    //VerifyOrExit(endpoint == gBluezServerEndpoint, msg = "Unexpected endpoint in WoBLEz_SubscriptionChange");
    VerifyOrExit(connection->c2 != NULL, msg = "weaveC2 is NULL in WoBLEz_SubscriptionChange");

    // Post an event to the Chip queue to process either a CHIPoBLE Subscribe or Unsubscribe based on
    // whether the client is enabling or disabling indications.
    {
        ChipDeviceEvent event;
        event.Type = (connection->mIsNotify) ? DeviceEventType::kCHIPoBLESubscribe : DeviceEventType::kCHIPoBLEUnsubscribe;
        event.CHIPoBLESubscribe.ConId = data;
        PlatformMgr().PostEvent(&event);
    }

    ChipLogProgress(DeviceLayer, "CHIPoBLE %s received", (connection->mIsNotify) ? "subscribe" : "unsubscribe");

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "HandleTXCharCCCDWrite() failed: %s", ErrorStr(err));
        // TODO: fail connection???
    }
}

void BLEManagerImpl::WoBLEz_IndicationConfirmation(void * data)
{
    // Post an event to the Chip queue to process the indicate confirmation.
    ChipDeviceEvent event;
    event.Type                          = DeviceEventType::kCHIPoBLEIndicateConfirm;
    event.CHIPoBLEIndicateConfirm.ConId = data;
    PlatformMgr().PostEvent(&event);
}

void BLEManagerImpl::DriveBLEState()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    // Perform any initialization actions that must occur after the Chip task is running.
    if (!GetFlag(mFlags, kFlag_AsyncInitCompleted))
    {
        SetFlag(mFlags, kFlag_AsyncInitCompleted);

        // If CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED is enabled,
        // disable CHIPoBLE advertising if the device is fully provisioned.
#if CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED
        if (ConfigurationMgr().IsFullyProvisioned())
        {
            ClearFlag(mFlags, kFlag_AdvertisingEnabled);
            ChipLogProgress(DeviceLayer, "CHIPoBLE advertising disabled because device is fully provisioned");
        }
#endif // CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED
    }

    ChipLogProgress(DeviceLayer, "CHIPoBLE before check control in progress");
    // If there's already a control operation in progress, wait until it completes.
    VerifyOrExit(!GetFlag(mFlags, kFlag_ControlOpInProgress), /* */);

    // Initializes the Bluez BLE layer if needed.
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_Enabled && !GetFlag(mFlags, kFlag_BluezBLELayerInitialized))
    {
        err = InitBluezBleLayer(false, NULL, mDeviceName, 1, mpAppState);
        SuccessOrExit(err);
        SetFlag(mFlags, kFlag_BluezBLELayerInitialized);
    }

    // Register the CHIPoBLE application with the ESP BLE layer if needed.
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_Enabled && !GetFlag(mFlags, kFlag_AppRegistered))
    {
        BluezBleGattsAppRegister(mpAppState);
        SetFlag(mFlags, kFlag_ControlOpInProgress);

        ExitNow();
    }

    /*
    // Start the CHIPoBLE GATT service if needed.
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_Enabled && !GetFlag(mFlags, kFlag_GATTServiceStarted))
    {
        err = esp_ble_gatts_start_service(mServiceAttrHandle);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "esp_ble_gatts_start_service() failed: %s", ErrorStr(err));
            ExitNow();
        }

        SetFlag(mFlags, kFlag_ControlOpInProgress);

        ExitNow();
    }
*/
    ChipLogProgress(DeviceLayer, "debug if enable adv");
    // If the application has enabled CHIPoBLE and BLE advertising...
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_Enabled &&
        GetFlag(mFlags, kFlag_AdvertisingEnabled)
    )
    {
        ChipLogProgress(DeviceLayer, "debug if enable adv2");
        // Start/re-start advertising if not already advertising, or if the advertising state of the
        // ESP BLE layer needs to be refreshed.
        if (!GetFlag(mFlags, kFlag_Advertising) || GetFlag(mFlags, kFlag_AdvertisingRefreshNeeded))
        {
            // Configure advertising data if it hasn't been done yet.  This is an asynchronous step which
            // must complete before advertising can be started.  When that happens, this method will
            // be called again, and execution will proceed to the code below.
            if (!GetFlag(mFlags, kFlag_AdvertisingConfigured))
            {
                BluezBleAdvertisementSetup(mpAppState);
                ExitNow();
            }

            // Start advertising.  This is also an asynchronous step.
            StartBluezAdv(mpAppState);
            SetFlag(sInstance.mFlags, kFlag_Advertising);
            ExitNow();
        }
    }

    // Otherwise stop advertising if needed...
    else
    {
        if (GetFlag(mFlags, kFlag_Advertising))
        {
            StopBluezAdv(mpAppState);
            SetFlag(mFlags, kFlag_ControlOpInProgress);

            ExitNow();
        }
    }

    /*
    // Stop the CHIPoBLE GATT service if needed.
    if (mServiceMode != ConnectivityManager::kCHIPoBLEServiceMode_Enabled && GetFlag(mFlags, kFlag_GATTServiceStarted))
    {
        // TODO: what to do about existing connections??

        err = esp_ble_gatts_stop_service(mServiceAttrHandle);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "esp_ble_gatts_stop_service() failed: %s", ErrorStr(err));
            ExitNow();
        }

        SetFlag(mFlags, kFlag_ControlOpInProgress);

        ExitNow();
    }
*/
exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Disabling CHIPoBLE service due to error: %s", ErrorStr(err));
        mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Disabled;
    }
}

void BLEManagerImpl::DriveBLEState(intptr_t arg)
{
    sInstance.DriveBLEState();
}

void BLEManagerImpl::HandleBluezSetupState(InEventParam * apEvent)
{
    CHIP_ERROR err         = CHIP_NO_ERROR;
    bool controlOpComplete = false;
    ChipLogProgress(DeviceLayer, "internal HandleBluezSetupState , %d", apEvent->mEventType);
    switch (apEvent->mEventType)
    {
        case InEventParam::kEvent_BluezAdvertisingConfigured:
            SetFlag(sInstance.mFlags, kFlag_AdvertisingConfigured);
            ClearFlag(sInstance.mFlags, kFlag_ControlOpInProgress);
            controlOpComplete = true;
            break;
        case InEventParam::kEvent_BluezAdvertisingStart:
            ClearFlag(sInstance.mFlags, kFlag_ControlOpInProgress);
            ClearFlag(sInstance.mFlags, kFlag_AdvertisingRefreshNeeded);

            if (!GetFlag(sInstance.mFlags, kFlag_Advertising))
            {
                ChipLogProgress(DeviceLayer, "CHIPoBLE advertising started");

                SetFlag(sInstance.mFlags, kFlag_Advertising);
            }
            break;
        case InEventParam::kEvent_BluezAdvertisingStop:
            ClearFlag(sInstance.mFlags, kFlag_ControlOpInProgress);
            ClearFlag(sInstance.mFlags, kFlag_AdvertisingRefreshNeeded);

            // Transition to the not Advertising state...
            if (GetFlag(sInstance.mFlags, kFlag_Advertising))
            {
                ClearFlag(sInstance.mFlags, kFlag_Advertising);

                ChipLogProgress(DeviceLayer, "CHIPoBLE advertising stopped");
            }
            break;
        case InEventParam::kEvent_BluezPeripheralRegisterApp:
            ChipLogProgress(DeviceLayer, "kBluezPeripheralRegisterApp");
            if (apEvent->mIsSuccess)
            {
                ChipLogProgress(DeviceLayer, "kBluezPeripheralRegisterApp success");
                SetFlag(mFlags, kFlag_AppRegistered);
                controlOpComplete = true;
            }
            else
            {
                err = CHIP_ERROR_INCORRECT_STATE;
            }
            break;
        default:
            break;
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Disabling CHIPoBLE service due to error: %s", ErrorStr(err));
        mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Disabled;
    }
    if (controlOpComplete)
    {
        ChipLogProgress(DeviceLayer, "drive ble state");
        ClearFlag(mFlags, kFlag_ControlOpInProgress);
        DriveBLEState();
    }
}

chip::System::Error BLEManagerImpl::NewEventParams(InEventParam ** aParam)
{
    *aParam = new InEventParam();
    return CHIP_SYSTEM_NO_ERROR;
}

void BLEManagerImpl::ReleaseEventParams(InEventParam * aParam)
{
    if (aParam != NULL)
    {
        delete aParam;
    }
}

void BLEManagerImpl::HandleBluezSetupState(intptr_t arg)
{
    ChipLogProgress(DeviceLayer, "HandleBluezSetupState ");
    if (arg != 0)
    {
        InEventParam * event = (InEventParam *) arg;
            ChipLogProgress(DeviceLayer, "HandleBluezSetupState 1");
        sInstance.HandleBluezSetupState(event);
        ReleaseEventParams(event);
    }
}

void BLEManagerImpl::NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId)
{
    ChipLogRetain(Ble, "Got notification regarding chip connection closure");
}

void BLEManagerImpl::NotifyBluezPeripheralRegisterAppComplete(bool aIsSuccess, void * apAppstate)
{
    InEventParam * pEvent = NULL;

    NewEventParams(&pEvent);
    pEvent->mIsSuccess = aIsSuccess;
    pEvent->mpAppstate = apAppstate;
    pEvent->mEventType  = InEventParam::kEvent_BluezPeripheralRegisterApp;
    PlatformMgr().ScheduleWork(HandleBluezSetupState, (intptr_t)(pEvent));
}

void BLEManagerImpl::NotifyBluezPeripheralAdvConfigueComplete(bool aIsSuccess, void * apAppstate)
{
    InEventParam * pEvent = NULL;

    NewEventParams(&pEvent);
    pEvent->mIsSuccess = aIsSuccess;
    pEvent->mpAppstate = apAppstate;
    pEvent->mEventType  = InEventParam::kEvent_BluezAdvertisingConfigured;
    PlatformMgr().ScheduleWork(HandleBluezSetupState, (intptr_t)(pEvent));
}

void BLEManagerImpl::NotifyBluezPeripheralAdvStartComplete(bool aIsSuccess, void * apAppstate)
{
    InEventParam * pEvent = NULL;

    NewEventParams(&pEvent);
    pEvent->mIsSuccess = aIsSuccess;
    pEvent->mpAppstate = apAppstate;
    pEvent->mEventType  = InEventParam::kEvent_BluezAdvertisingStart;
    PlatformMgr().ScheduleWork(HandleBluezSetupState, (intptr_t)(pEvent));
}

void BLEManagerImpl::NotifyBluezPeripheralAdvStopComplete(bool aIsSuccess, void * apAppstate)
{
    InEventParam * pEvent = NULL;

    NewEventParams(&pEvent);
    pEvent->mIsSuccess = aIsSuccess;
    pEvent->mpAppstate = apAppstate;
    pEvent->mEventType  = InEventParam::kEvent_BluezAdvertisingStop;
    PlatformMgr().ScheduleWork(HandleBluezSetupState, (intptr_t)pEvent);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
