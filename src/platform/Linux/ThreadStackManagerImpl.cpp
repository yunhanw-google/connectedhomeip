/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include <array>
#include <limits.h>
#include <string.h>

#include "platform/internal/CHIPDeviceLayerInternal.h"
#include "platform/internal/DeviceNetworkInfo.h"

#include "platform/PlatformManager.h"
#include "platform/ThreadStackManager.h"
#include "support/CodeUtils.h"
#include "support/logging/CHIPLogging.h"

namespace chip {
namespace DeviceLayer {

ThreadStackManagerImpl ThreadStackManagerImpl::sInstance;

constexpr char ThreadStackManagerImpl::kDBusOpenThreadService[];
constexpr char ThreadStackManagerImpl::kDBusOpenThreadObjectPath[];

constexpr char ThreadStackManagerImpl::kOpenthreadDeviceRoleDisabled[];
constexpr char ThreadStackManagerImpl::kOpenthreadDeviceRoleDetached[];
constexpr char ThreadStackManagerImpl::kOpenthreadDeviceRoleChild[];
constexpr char ThreadStackManagerImpl::kOpenthreadDeviceRoleRouter[];
constexpr char ThreadStackManagerImpl::kOpenthreadDeviceRoleLeader[];

constexpr char ThreadStackManagerImpl::kPropertyDeviceRole[];

ThreadStackManagerImpl::ThreadStackManagerImpl() : mProxy(nullptr), mAttached(false) {}

CHIP_ERROR ThreadStackManagerImpl::_InitThreadStack()
{
    GError * gdbusError = nullptr;
    mProxy = openthread_io_openthread_border_router_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kDBusOpenThreadService, kDBusOpenThreadObjectPath, nullptr, &gdbusError);
    if (mProxy == nullptr || gdbusError != nullptr)
    {
        ChipLogError(DeviceLayer, "openthread: failed to create openthread dbus proxy %s", gdbusError ? gdbusError->message : "unknown error");
        return CHIP_ERROR_INTERNAL;
    }

    g_signal_connect(mProxy, "g-properties-changed", G_CALLBACK(OnDbusPropertiesChanged), this);

    const gchar * role = openthread_io_openthread_border_router_get_device_role(mProxy);
    if (role != nullptr) {
        ThreadDevcieRoleChangedHandler(role);
    }

    return CHIP_NO_ERROR;
}

void ThreadStackManagerImpl::OnDbusPropertiesChanged(OpenthreadIoOpenthreadBorderRouter *proxy, GVariant *changed_properties, const gchar* const *invalidated_properties, gpointer user_data)
{
    ThreadStackManagerImpl * me = reinterpret_cast<ThreadStackManagerImpl*>(user_data);
    if (g_variant_n_children(changed_properties) > 0) {
        GVariantIter *iter;
        const gchar *key;
        GVariant *value;

        g_variant_get(changed_properties, "a{sv}", &iter);
        while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
            if (strcmp(key, kPropertyDeviceRole) == 0) {
                gchar * value_str = g_variant_print(value, TRUE);
                ChipLogProgress(DeviceLayer, "Thread role changed to: %s", value_str);
                me->ThreadDevcieRoleChangedHandler(value_str);
                g_free(value_str);
            }
        }
        g_variant_iter_free (iter);
    }
}

void ThreadStackManagerImpl::ThreadDevcieRoleChangedHandler(const gchar * role)
{
    bool attached = strcmp(role, kOpenthreadDeviceRoleDetached) != 0 && strcmp(role, kOpenthreadDeviceRoleDisabled) !=0;

    ChipDeviceEvent event = ChipDeviceEvent{};

    if (attached != mAttached)
    {
        event.Type = DeviceEventType::kThreadConnectivityChange;
        event.ThreadConnectivityChange.Result =
            attached ? ConnectivityChange::kConnectivity_Established : ConnectivityChange::kConnectivity_Lost;
        PlatformMgr().PostEvent(&event);
    }
    mAttached = attached;

    event.Type                          = DeviceEventType::kThreadStateChange;
    event.ThreadStateChange.RoleChanged = true;
    PlatformMgr().PostEvent(&event);
}

void ThreadStackManagerImpl::_ProcessThreadActivity() {}

bool ThreadStackManagerImpl::_HaveRouteToAddress(const Inet::IPAddress & destAddr)
{
    if (mProxy != nullptr && !_IsThreadAttached()) {
        return false;
    }
    if (destAddr.IsIPv6LinkLocal())
    {
        return true;
    }

    GVariant * routes = openthread_io_openthread_border_router_get_external_routes(mProxy);
    if (routes == nullptr) {
        return false;
    }

    if (g_variant_n_children(routes) > 0) {
        GVariantIter *iter;
        GVariant *route;

        g_variant_get(routes, "av", &iter);
        while(g_variant_iter_loop(iter, "&v", &route)) {
            GVariant *prefix;
            guint16 rloc16;
            guchar preference;
            gboolean stable;
            gboolean nextHopIsThisDevice;
            g_variant_get(route, "(&vqybb)", &prefix, &rloc16, &preference, &stable, &nextHopIsThisDevice);

            GVariant *address;
            guchar prefixLength;
            g_variant_get(prefix, "(&vy)", &address, &prefixLength);

            std::vector<guchar> addressVector;
            GVariantIter *addressIter;
            guchar addressByte;
            g_variant_get(address, "a(y)", &addressIter);
            while (g_variant_iter_loop(addressIter, "(y)", &addressByte))
            {
                addressVector.push_back(addressByte);
            }
            g_variant_iter_free (addressIter);
            g_variant_unref(address);
            g_variant_unref(prefix);

            Inet::IPPrefix p;
            p.IPAddr = Inet::IPAddress::FromIPv6(*reinterpret_cast<struct in6_addr *>(addressVector.data()));
            p.Length = prefixLength;

            if (p.MatchAddress(destAddr)) {
                return true;
            }
        }
        g_variant_iter_free (iter);
    }

    return false;
}

void ThreadStackManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
    (void) event;
    // The otbr-agent processes the Thread state handling by itself so there
    // isn't much to do in the Chip stack.
}

CHIP_ERROR ThreadStackManagerImpl::_SetThreadProvision(ByteSpan netInfo)
{
    VerifyOrReturnError(mProxy != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(Thread::OperationalDataset::IsValid(netInfo), CHIP_ERROR_INVALID_ARGUMENT);
    std::vector<uint8_t> data(netInfo.data(), netInfo.data() + netInfo.size());

    GBytes *bytes = g_bytes_new(netInfo.data(), netInfo.size());
    GVariant * value = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
    openthread_io_openthread_border_router_set_active_dataset_tlvs(mProxy, value);
    g_variant_unref(value);

    // post an event alerting other subsystems about change in provisioning state
    ChipDeviceEvent event;
    event.Type                                           = DeviceEventType::kServiceProvisioningChange;
    event.ServiceProvisioningChange.IsServiceProvisioned = true;
    PlatformMgr().PostEvent(&event);

    return CHIP_NO_ERROR;
}

CHIP_ERROR ThreadStackManagerImpl::_GetThreadProvision(ByteSpan & netInfo)
{
    VerifyOrReturnError(mProxy != nullptr, CHIP_ERROR_INCORRECT_STATE);

    GVariant * value = openthread_io_openthread_border_router_get_active_dataset_tlvs(mProxy);
    GBytes * bytes = g_variant_get_data_as_bytes(value);
    gsize size;
    const uint8_t * data = reinterpret_cast<const uint8_t*>(g_bytes_get_data(bytes, &size));
    ReturnErrorOnFailure(mDataset.Init(ByteSpan(data, size)));

    g_bytes_unref(bytes);

    netInfo = mDataset.AsByteSpan();

    return CHIP_NO_ERROR;
}

bool ThreadStackManagerImpl::_IsThreadProvisioned()
{
    return static_cast<Thread::OperationalDataset &>(mDataset).IsCommissioned();
}

void ThreadStackManagerImpl::_ErasePersistentInfo()
{
    static_cast<Thread::OperationalDataset &>(mDataset).Clear();
}

bool ThreadStackManagerImpl::_IsThreadEnabled()
{
    if (mProxy == nullptr) {
        return false;
    }

    const gchar * role = openthread_io_openthread_border_router_get_device_role(mProxy);
    return (strcmp(role, kOpenthreadDeviceRoleDisabled) != 0);
}

bool ThreadStackManagerImpl::_IsThreadAttached()
{
    return mAttached;
}

CHIP_ERROR ThreadStackManagerImpl::_SetThreadEnabled(bool val)
{
    VerifyOrReturnError(mProxy != nullptr, CHIP_ERROR_INCORRECT_STATE);
    if (val)
    {
        GError * gdbusError = nullptr;
        gboolean result = openthread_io_openthread_border_router_call_attach_sync(mProxy, nullptr, &gdbusError);
        if (gdbusError != nullptr)
        {
            ChipLogError(DeviceLayer, "openthread: _SetThreadEnabled calling %s failed: %s", "Attach", gdbusError ? gdbusError->message : "unknown error");
            return CHIP_ERROR_INTERNAL;
        }

        if (!result) {
            ChipLogError(DeviceLayer, "openthread: _SetThreadEnabled calling %s failed: %s", "Attach", "return false");
            return CHIP_ERROR_INTERNAL;
        }
    }
    else
    {
        GError * gdbusError = nullptr;
        gboolean result = openthread_io_openthread_border_router_call_reset_sync(mProxy, nullptr, &gdbusError);
        if (gdbusError != nullptr)
        {
            ChipLogError(DeviceLayer, "openthread: _SetThreadEnabled calling %s failed: %s", "Reset", gdbusError ? gdbusError->message : "unknown error");
            return CHIP_ERROR_INTERNAL;
        }

        if (!result) {
            ChipLogError(DeviceLayer, "openthread: _SetThreadEnabled calling %s failed: %s", "Reset", "return false");
            return CHIP_ERROR_INTERNAL;
        }
    }
    return CHIP_NO_ERROR;
}

ConnectivityManager::ThreadDeviceType ThreadStackManagerImpl::_GetThreadDeviceType()
{
    ConnectivityManager::ThreadDeviceType type = ConnectivityManager::ThreadDeviceType::kThreadDeviceType_NotSupported;
    if (mProxy == nullptr)
    {
        ChipLogError(DeviceLayer, "Cannot get device role with Thread api client: %s", "");
        return ConnectivityManager::ThreadDeviceType::kThreadDeviceType_NotSupported;
    }

    const gchar * role = openthread_io_openthread_border_router_get_device_role(mProxy);
    if (strcmp(role, kOpenthreadDeviceRoleDetached) == 0 || strcmp(role, kOpenthreadDeviceRoleDisabled) == 0) {
        return ConnectivityManager::ThreadDeviceType::kThreadDeviceType_NotSupported;
    } else if (strcmp(role, kOpenthreadDeviceRoleChild) == 0) {
        GVariant * linkMode = openthread_io_openthread_border_router_get_link_mode(mProxy);
        gboolean rx_on_when_idle;
        gboolean device_type;
        gboolean network_data;
        g_variant_get(linkMode, "(bbb)", &rx_on_when_idle, &device_type, &network_data);
        if (!rx_on_when_idle)
        {
            type = ConnectivityManager::ThreadDeviceType::kThreadDeviceType_SleepyEndDevice;
        }
        else
        {
            type = device_type ? ConnectivityManager::ThreadDeviceType::kThreadDeviceType_FullEndDevice : ConnectivityManager::ThreadDeviceType::kThreadDeviceType_MinimalEndDevice;
        }
        return type;
    } else if (strcmp(role, kOpenthreadDeviceRoleLeader) == 0 || strcmp(role, kOpenthreadDeviceRoleRouter) == 0) {
        return ConnectivityManager::ThreadDeviceType::kThreadDeviceType_Router;
    } else {
        ChipLogError(DeviceLayer, "Unknown Thread role: %s", role);
        return ConnectivityManager::ThreadDeviceType::kThreadDeviceType_NotSupported;
    }
}

CHIP_ERROR ThreadStackManagerImpl::_SetThreadDeviceType(ConnectivityManager::ThreadDeviceType deviceType)
{
    gboolean rx_on_when_idle = true;
    gboolean device_type = true;
    gboolean network_data = true;
    VerifyOrReturnError(mProxy != nullptr, CHIP_ERROR_INCORRECT_STATE);
    if (deviceType == ConnectivityManager::ThreadDeviceType::kThreadDeviceType_MinimalEndDevice)
    {
        network_data = false;
    }
    else if (deviceType == ConnectivityManager::ThreadDeviceType::kThreadDeviceType_SleepyEndDevice)
    {
        rx_on_when_idle = false;
        network_data = false;
    }

    if (!network_data)
    {
        GVariant * linkMode = g_variant_new("(bbb)", rx_on_when_idle, device_type, network_data);
        openthread_io_openthread_border_router_set_link_mode(mProxy, linkMode);
        g_variant_unref(linkMode);
    }

    return CHIP_NO_ERROR;
}

void ThreadStackManagerImpl::_GetThreadPollingConfig(ConnectivityManager::ThreadPollingConfig & pollingConfig)
{
    (void) pollingConfig;

    ChipLogError(DeviceLayer, "Polling config is not supported on linux");
}

CHIP_ERROR ThreadStackManagerImpl::_SetThreadPollingConfig(const ConnectivityManager::ThreadPollingConfig & pollingConfig)
{
    (void) pollingConfig;

    ChipLogError(DeviceLayer, "Polling config is not supported on linux");
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

bool ThreadStackManagerImpl::_HaveMeshConnectivity()
{
    // TODO: Remove Weave legacy APIs
    // For a leader with a child, the child is considered to have mesh connectivity
    // and the leader is not, which is a very confusing definition.
    // This API is Weave legacy and should be removed.

    ChipLogError(DeviceLayer, "HaveMeshConnectivity has confusing behavior and shouldn't be called");
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void ThreadStackManagerImpl::_OnMessageLayerActivityChanged(bool messageLayerIsActive)
{
    (void) messageLayerIsActive;
}

CHIP_ERROR ThreadStackManagerImpl::_GetAndLogThreadStatsCounters()
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ThreadStackManagerImpl::_GetAndLogThreadTopologyMinimal()
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ThreadStackManagerImpl::_GetAndLogThreadTopologyFull()
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ThreadStackManagerImpl::_GetPrimary802154MACAddress(uint8_t * buf)
{
    VerifyOrReturnError(mProxy != nullptr, CHIP_ERROR_INCORRECT_STATE);
    guint64 extAddr = openthread_io_openthread_border_router_get_extended_address (mProxy);

    for (size_t i = 0; i < sizeof(extAddr); i++)
    {
        buf[sizeof(uint64_t) - i - 1] = (extAddr & UINT8_MAX);
        extAddr >>= CHAR_BIT;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ThreadStackManagerImpl::_GetExternalIPv6Address(chip::Inet::IPAddress & addr)
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ThreadStackManagerImpl::_GetPollPeriod(uint32_t & buf)
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ThreadStackManagerImpl::_JoinerStart()
{
    // TODO: Remove Weave legacy APIs
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

ThreadStackManager & ThreadStackMgr()
{
    return chip::DeviceLayer::ThreadStackManagerImpl::sInstance;
}

ThreadStackManagerImpl & ThreadStackMgrImpl()
{
    return chip::DeviceLayer::ThreadStackManagerImpl::sInstance;
}

} // namespace DeviceLayer
} // namespace chip
