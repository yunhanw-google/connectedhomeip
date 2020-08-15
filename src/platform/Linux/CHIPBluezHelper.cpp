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

/**
 *    @file
 *          Provides an implementation of the BLEManager singleton object
 *          for Linux platforms.
 */
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <ble/CHIPBleServiceData.h>
#include <new>
#include <platform/internal/BLEManager.h>

#include <gio/gunixfdlist.h>

#include <errno.h>
#include <stdarg.h>
#include <strings.h>
#include <unistd.h>

#include <support/CodeUtils.h>
#include "CHIPBluezHelper.h"
#include <ble/BlePlatformDelegate.h>


using namespace ::nl;

namespace chip {
namespace DeviceLayer {
namespace Internal {



/**
* This type represents a BLE address.
*
*/


static int sBluezFD[2];



static GMainLoop *          sBluezMainLoop = NULL;
static pthread_t            sBluezThread;

static char *BluezShortServiceUUIDToUUIDString(uint16_t aServiceId)
{
    return g_strdup_printf("%08x%s", aServiceId, CHIP_BLE_BASE_SERVICE_UUID_STRING);
}

/***********************************************************************
 * Advertising start
 ***********************************************************************/

static gboolean sAdvertising = FALSE;

static gboolean BluezAdvertisingRelease(BluezLEAdvertisement1 *aAdv,
                                        GDBusMethodInvocation *aInvocation,
                                        gpointer               apClosure)
{
    BluezServerEndpoint *          endpoint = (BluezServerEndpoint *)apClosure;
    VerifyOrExit(aAdv != NULL, ChipLogProgress(DeviceLayer, "BluezLEAdvertisement1 is null"));
    ChipLogProgress(DeviceLayer, "RELEASE adv object at %s", g_dbus_proxy_get_object_path(G_DBUS_PROXY(aAdv)));

    g_dbus_object_manager_server_unexport(endpoint->root, endpoint->advPath);
    sAdvertising = FALSE;

exit:
    return TRUE;
}

static BluezLEAdvertisement1 *BluezAdvertisingCreate(BluezServerEndpoint *apEndpoint)
{
    BluezLEAdvertisement1 *adv = NULL;
    BluezObjectSkeleton *  object;
    GVariant *             serviceData;
    gchar *                localName;

    GVariantBuilder        serviceDataBuilder;
    GVariantBuilder        serviceUUIDsBuilder;
    uint16_t               offset;
    char *                 debugStr;

    const gchar * array[1];

    if (apEndpoint->advPath == NULL)
        apEndpoint->advPath = g_strdup_printf("%s/advertising", apEndpoint->rootPath);

    array[0] = g_strdup_printf("%s", apEndpoint->advertisingUUID);
    ChipLogProgress(DeviceLayer, "CREATE adv object at %s", apEndpoint->advPath);
    object = bluez_object_skeleton_new(apEndpoint->advPath);

    adv = bluez_leadvertisement1_skeleton_new();

    g_variant_builder_init(&serviceDataBuilder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&serviceUUIDsBuilder, G_VARIANT_TYPE("as"));
    offset = 0;

    g_variant_builder_add(
            &serviceDataBuilder, "{sv}", apEndpoint->advertisingUUID,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, apEndpoint->chipServiceData, sizeof(CHIPServiceData), sizeof(uint8_t)));
    g_variant_builder_add(&serviceUUIDsBuilder, "s", apEndpoint->advertisingUUID);

    if (apEndpoint->adapterName != NULL)
        localName = g_strdup_printf("%s", apEndpoint->adapterName);
    else
        localName= g_strdup_printf("C%04x", getpid() & 0xffff);

    serviceData = g_variant_builder_end(&serviceDataBuilder);
    debugStr    = g_variant_print(serviceData, TRUE);
    ChipLogProgress(DeviceLayer, "SET service data to %s", debugStr);
    g_free(debugStr);

    bluez_leadvertisement1_set_type_(adv,
                                     (apEndpoint->mType & BLUEZ_ADV_TYPE_CONNECTABLE) ? "peripheral" : "broadcast");
    // empty manufacturer data
    // empty solicit UUIDs
    bluez_leadvertisement1_set_service_data(adv, serviceData);
    // empty data

    // TODO why this was set when scannable type was selected?
    //bluez_leadvertisement1_set_discoverable(adv, (aConfig->mType & OT_TOBLE_ADV_TYPE_SCANNABLE) ? TRUE : FALSE); ??
    //bluez_leadvertisement1_set_discoverable_timeout(adv, 1000); // we will kill advertising before then
    // empty includes

    // advertising name corresponding to the PID and object path, for debug purposes
    bluez_leadvertisement1_set_local_name(adv, localName);
    bluez_leadvertisement1_set_service_uuids(adv, array);

    // 0xffff means no appearance
    bluez_leadvertisement1_set_appearance(adv, 0xffff);

    bluez_leadvertisement1_set_duration(adv, apEndpoint->mDuration);
    // empty duration, we don't have a clear notion what it would mean to timeslice between toble and anyone else
    bluez_leadvertisement1_set_timeout(adv, 0);
    // empty secondary channel for now

    bluez_object_skeleton_set_leadvertisement1(object, adv);
    g_signal_connect(adv, "handle-release", G_CALLBACK(BluezAdvertisingRelease), apEndpoint);

    g_dbus_object_manager_server_export(apEndpoint->root, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    BLEManagerImpl::NotifyBluezPeripheralAdvConfigueComplete(true, NULL);

    return adv;
}

static void BluezTobleAdvStartDone(GObject *aObject, GAsyncResult *aResult, gpointer apClosure)
{
    BluezLEAdvertisingManager1 *advMgr = BLUEZ_LEADVERTISING_MANAGER1(aObject);
    GError *                    error  = NULL;
    BluezServerEndpoint *          endpoint = (BluezServerEndpoint *)apClosure;

    gboolean success = bluez_leadvertising_manager1_call_register_advertisement_finish(advMgr, aResult, &error);

    if (success == FALSE)
    {
        g_dbus_object_manager_server_unexport(endpoint->root, endpoint->advPath);
    }

    sAdvertising = success;

    VerifyOrExit(success == TRUE, ChipLogProgress(DeviceLayer, "FAIL: RegisterAdvertisement : %s", error->message));

    ChipLogProgress(DeviceLayer, "RegisterAdvertisement complete");

    BLEManagerImpl::NotifyBluezPeripheralAdvStartComplete(true, NULL);

exit:
    if (error != NULL)
        g_error_free(error);

    /*
    g_mutex_lock (&otPlatMutex);
    otPlatResult = success ? OT_ERROR_NONE : OT_ERROR_FAILED;
    g_cond_signal (&otPlatCond);
    g_mutex_unlock (&otPlatMutex);
     */
}

static void BluezTobleAdvStopDone(GObject *aObject, GAsyncResult *aResult, gpointer aClosure)
{
    BluezLEAdvertisingManager1 *advMgr = BLUEZ_LEADVERTISING_MANAGER1(aObject);
    BluezServerEndpoint *       endpoint = (BluezServerEndpoint *)aClosure;
    GError *                    error  = NULL;

    gboolean success = bluez_leadvertising_manager1_call_unregister_advertisement_finish(advMgr, aResult, &error);

    if (success == FALSE)
    {
        g_dbus_object_manager_server_unexport(endpoint->root, endpoint->advPath);
    }
    else
    {
        sAdvertising = FALSE;
    }

    VerifyOrExit(success == TRUE, ChipLogProgress(DeviceLayer, "FAIL: UnregisterAdvertisement : %s", error->message));

    BLEManagerImpl::NotifyBluezPeripheralAdvStopComplete(true, NULL);
    ChipLogProgress(DeviceLayer, "UnregisterAdvertisement complete");

exit:
    if (error != NULL)
        g_error_free(error);

    /*
    g_mutex_lock (&otPlatMutex);
    otPlatResult = success ? OT_ERROR_NONE : OT_ERROR_FAILED;
    g_cond_signal (&otPlatCond);
    g_mutex_unlock (&otPlatMutex);
     */
}

static gboolean BluezBleAdvSetup(void *aClosure)
{
    // create and register advertising object; get the LEAdvertisingManager proxy
    // for the default adapter and register the object with the adapter

    GDBusObject *               adapter;
    BluezServerEndpoint *             endpoint = (BluezServerEndpoint *)aClosure;
    BluezLEAdvertisingManager1 *advMgr = NULL;
    GVariantBuilder             optionsBuilder;
    GVariant *                  options;
    BluezLEAdvertisement1 *     adv;
    gboolean dbusSent = FALSE;

    VerifyOrExit(endpoint->mIsAdvertising == FALSE, ChipLogProgress(DeviceLayer, "FAIL: Advertising already enabled in %s", __func__));

    endpoint->isCentral = false;
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

    adv = BluezAdvertisingCreate(endpoint);
    VerifyOrExit(adv != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adv in %s", __func__));


    dbusSent = TRUE;

exit:
    //g_free((void *)config->mData); /* TODO should not need to cast this but we reuse ChipAdvConfig here */
    //g_free(config);

    /*
    if (dbusSent == FALSE)
    {
        g_mutex_lock (&otPlatMutex);
        otPlatResult = OT_ERROR_FAILED;
        g_cond_signal (&otPlatCond);
        g_mutex_unlock (&otPlatMutex);
    }
     */

    return G_SOURCE_REMOVE;
}

static gboolean BluezTobleAdvStart(void *apClosure)
{
    // create and register advertising object; get the LEAdvertisingManager proxy
    // for the default adapter and register the object with the adapter

    GDBusObject *               adapter;
    BluezLEAdvertisingManager1 *advMgr = NULL;
    GVariantBuilder             optionsBuilder;
    GVariant *                  options;
    BluezLEAdvertisement1 *     adv;
    gboolean dbusSent = FALSE;
    BluezServerEndpoint *          endpoint = (BluezServerEndpoint *)apClosure;

    ChipLogProgress(DeviceLayer, "start BluezTobleAdvStart", __func__);
    VerifyOrExit(sAdvertising == FALSE, ChipLogProgress(DeviceLayer, "FAIL: Advertising already enabled in %s", __func__));

    endpoint->isCentral = false;
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

    adapter = g_dbus_interface_get_object(G_DBUS_INTERFACE(endpoint->adapter));
    VerifyOrExit(adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adapter in %s", __func__));

    advMgr = bluez_object_get_leadvertising_manager1(BLUEZ_OBJECT(adapter));
    VerifyOrExit(advMgr != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL advMgr in %s", __func__));

    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    options = g_variant_builder_end(&optionsBuilder);

    bluez_leadvertising_manager1_call_register_advertisement(advMgr, endpoint->advPath, options, NULL,
                                                             BluezTobleAdvStartDone, apClosure);

    dbusSent = TRUE;

    exit:
    //g_free((void *)config->mData); /* TODO should not need to cast this but we reuse ChipAdvConfig here */
    //g_free(config);

    /*
    if (dbusSent == FALSE)
    {
        g_mutex_lock (&otPlatMutex);
        otPlatResult = OT_ERROR_FAILED;
        g_cond_signal (&otPlatCond);
        g_mutex_unlock (&otPlatMutex);
    }
     */

    return G_SOURCE_REMOVE;
}

static gboolean BluezTobleAdvStop(void *aClosure)
{
    // create and register advertising object; get the LEAdvertisingManager proxy
    // for the default adapter and register the object with the adapter

    GDBusObject *               adapter;
    BluezServerEndpoint *          endpoint = (BluezServerEndpoint *)aClosure;
    BluezLEAdvertisingManager1 *advMgr = NULL;
    GVariantBuilder             optionsBuilder;
    GVariant *                  options;
    BluezLEAdvertisement1 *     adv;
    gboolean dbusSent = FALSE;

    VerifyOrExit(endpoint->mIsAdvertising == FALSE, ChipLogProgress(DeviceLayer, "FAIL: Advertising already enabled in %s", __func__));

    endpoint->isCentral = false;
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

    adapter = g_dbus_interface_get_object(G_DBUS_INTERFACE(endpoint->adapter));
    VerifyOrExit(adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adapter in %s", __func__));

    advMgr = bluez_object_get_leadvertising_manager1(BLUEZ_OBJECT(adapter));
    VerifyOrExit(advMgr != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL advMgr in %s", __func__));

    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    options = g_variant_builder_end(&optionsBuilder);

    bluez_leadvertising_manager1_call_unregister_advertisement(advMgr, endpoint->advPath, NULL,
                                                             BluezTobleAdvStopDone, aClosure);

    dbusSent = TRUE;

exit:
    //g_free((void *)endpoint->mData); /* TODO should not need to cast this but we reuse ChipAdvConfig here */
    //g_free(endpoint);

    /*
    if (dbusSent == FALSE)
    {
        g_mutex_lock (&otPlatMutex);
        otPlatResult = OT_ERROR_FAILED;
        g_cond_signal (&otPlatCond);
        g_mutex_unlock (&otPlatMutex);
    }
     */

    return G_SOURCE_REMOVE;
}
/*
otError otPlatTobleAdvStart(otInstance *aInstance, const ChipAdvConfig *aAdvConfig)
{
    otError           err;
    uint8_t *         adv;
    ChipAdvConfig *closure;

    closure            = g_new0(ChipAdvConfig, 1);
    closure->mType     = aAdvConfig->mType;
    closure->mInterval = aAdvConfig->mInterval;
    closure->mLength   = aAdvConfig->mLength;
    adv                = g_malloc(closure->mLength);
    memcpy(adv, aAdvConfig->mData, closure->mLength);
    closure->mData = adv;

    //g_mutex_lock (&otPlatMutex);
    bluezRunOnBluezThread(BluezTobleAdvStart, closure);
    //g_cond_wait (&otPlatCond, &otPlatMutex);
    err = otPlatResult;
    //g_mutex_unlock (&otPlatMutex);

    ChipLogProgress(DeviceLayer, "%s err=%d", __func__, err);

    return err;
}
*/

static BluezConnection *GetBluezConnectionViaDevice(BluezServerEndpoint * apEndpoint);
static BluezConnection *BluezCharacteristicGetBluezConnection(BluezGattCharacteristic1 *aChar, GVariant * aOptions, BluezServerEndpoint * apEndpoint);

static gboolean BluezCharacteristicReadValue(BluezGattCharacteristic1 *aChar,
                                             GDBusMethodInvocation *   aInvocation,
                                             GVariant *                aOptions)
{
    GVariant *val;

    ChipLogProgress(DeviceLayer, "debug BluezCharacteristicReadValue");
    val = (GVariant *)bluez_gatt_characteristic1_get_value(aChar);

    bluez_gatt_characteristic1_complete_read_value(aChar, aInvocation, val);

    return TRUE;
}

static gboolean BluezCharacteristicWriteValue(BluezGattCharacteristic1 *aChar,
                                              GDBusMethodInvocation *   aInvocation,
                                              GVariant *                aValue,
                                              GVariant *                aOptions,
                                              gpointer apClosure)
{
    const uint8_t *    tmpBuf;
    uint8_t *          buf;
    size_t             len;

    if (aValue != NULL)
    {
        ChipLogProgress(DeviceLayer, "It is not null");
    }

    BluezServerEndpoint *endpoint = (BluezServerEndpoint *) apClosure;
    BluezConnection *conn   = GetBluezConnectionViaDevice(endpoint);
    GVariant * temp = g_variant_get_child_value (aValue, 0);
    char *             valStr = g_variant_print(temp, TRUE);

    //GVariantIter* iter1;
    //g_variant_get( result, "ay", &iter1 );
    //GVariant* child = g_variant_iter_next_value( iter1 );
    //ChipLogProgress(DeviceLayer, "TRACE: Char write value children %s", g_variant_n_children(aValue));
    //g_free(valStr);

    //VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
    //                                                                         "No CHIP Bluez connection"));

    bluez_gatt_characteristic1_set_value(aChar, g_variant_ref(aValue));

    tmpBuf = (const uint8_t *)g_variant_get_fixed_array(aValue, &len, sizeof(uint8_t));
    buf    = (uint8_t *)g_memdup(tmpBuf, len);


    BLEManagerImpl::WoBLEz_WriteReceived(conn, buf, len);

    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC1Write), gOpenThreadInstance, conn, buf, len);

    bluez_gatt_characteristic1_complete_write_value(aChar, aInvocation);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicWriteValueError(BluezGattCharacteristic1 *aChar,
                                                   GDBusMethodInvocation *   aInvocation,
                                                   GVariant *                aValue,
                                                   GVariant *                aOptions,
                                                   gpointer apClosure)
{


    const uint8_t *    tmpBuf;
    uint8_t *          buf;
    size_t             len;
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) apClosure;
    BluezConnection *conn   = BluezCharacteristicGetBluezConnection(aChar, aOptions, endpoint);

    ChipLogProgress(DeviceLayer, "BluezCharacteristicWriteValueError");


    //bluez_gatt_characteristic1_set_value(aChar, (const gchar *)g_variant_ref(aValue));

    //tmpBuf = (const uint8_t *)g_variant_get_fixed_array(aValue, &len, sizeof(uint8_t));
    //buf    = (uint8_t *)g_memdup(tmpBuf, len);


    //BLEManagerImpl::WoBLEz_WriteReceived(conn, buf, len);

    //bluez_gatt_characteristic1_complete_write_value(aChar, aInvocation);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicWriteFD(GIOChannel *aChannel, GIOCondition aCond, gpointer aClosure)
{
    BluezConnection *conn   = (BluezConnection *)aClosure;
    GVariant *         newVal;
    gchar *          buf;
    ssize_t            len;
    int                fd;

    ChipLogProgress(DeviceLayer, "%s mtu, %d", __func__, conn->mMtu);

    if (aCond & G_IO_HUP)
    {
        ChipLogProgress(DeviceLayer, "INFO: socket disconnected in %s", __func__);
        return FALSE;
    }

    if (aCond & (G_IO_ERR | G_IO_NVAL))
    {
        ChipLogProgress(DeviceLayer, "INFO: socket error in %s", __func__);
        return FALSE;
    }

    VerifyOrExit(aCond == G_IO_IN, ChipLogProgress(DeviceLayer, "FAIL: error in %s", __func__));
    buf = (gchar *)g_malloc(conn->mMtu);
    fd  = g_io_channel_unix_get_fd(aChannel);

    len = read(fd, buf, conn->mMtu);

    VerifyOrExit(len > 0, ChipLogProgress(DeviceLayer, "FAIL: short read in %s (%d)", __func__, len));

    newVal = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, buf, len, sizeof(uint8_t));
    bluez_gatt_characteristic1_set_value(conn->c1, newVal);
    BLEManagerImpl::WoBLEz_WriteReceived(conn, (const uint8_t*)buf, len);

//    bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC1Write), gOpenThreadInstance, conn, buf, len);
exit:
    return TRUE;
}

// Not sure why codegen generates invalid code for bluez_gatt_characteristic1_complete_acquire_write
// Probably it is not handling fd passing correctly
static void
Bluez_gatt_characteristic1_complete_acquire_write_with_fd(GDBusMethodInvocation *invocation,
                                                          int fd,
                                                          guint16 mtu)
{
    GUnixFDList *fd_list = g_unix_fd_list_new();
    int index;

    index = g_unix_fd_list_append(fd_list, fd, NULL);

    g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
                                                            g_variant_new ("(@hq)", g_variant_new_handle(index), mtu),
                                                            fd_list);
}

static gboolean bluezCharacteristicDestroyFD(GIOChannel *aChannel, GIOCondition aCond, gpointer aClosure)
{
    return G_SOURCE_REMOVE;
}


static gboolean BluezCharacteristicAcquireWrite(BluezGattCharacteristic1 *aChar,
                                                GDBusMethodInvocation *   aInvocation,
                                                GVariant *                aOptions,
                                                gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    BluezConnection *conn = GetBluezConnectionViaDevice(endpoint);
    int                fds[2] = {-1, -1};
    GIOChannel *       channel;
    char *             errStr;
    char *             debugStr;
    GVariantDict       options;

    ChipLogProgress(DeviceLayer, "BluezCharacteristicAcquireWrite is called, conn: %p", conn);

    //VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
    //                                                                         "No Toble connection"));

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
    {
        errStr = strerror(errno);
        ChipLogProgress(DeviceLayer, "FAIL: socketpair: %s in %s", errStr, __func__);
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed", "FD creation failed");
        SuccessOrExit(false);
    }
    debugStr = g_variant_print(aOptions, TRUE);
    ChipLogProgress(DeviceLayer, "TRACE: AcquireWrite options %s", debugStr);
    g_free(debugStr);
    g_variant_dict_init(&options, aOptions);
    if (g_variant_dict_contains(&options, "mtu") == TRUE)
    {
        GVariant *v = g_variant_dict_lookup_value(&options, "mtu", G_VARIANT_TYPE_UINT16);
        conn->mMtu = g_variant_get_uint16(v);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "FAIL: no MTU in options in %s", __func__);
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.InvalidArguments",
                                                   "MTU negotiation failed");
        SuccessOrExit(false);
    }
    channel = g_io_channel_unix_new(fds[0]);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_close_on_unref(channel, TRUE);
    g_io_channel_set_buffered(channel, FALSE);
    conn->c1Channel.channel  = channel;
    conn->c1Channel.watch = g_io_add_watch(channel,(GIOCondition)(G_IO_HUP | G_IO_IN | G_IO_ERR | G_IO_NVAL),
                                            BluezCharacteristicWriteFD, conn);

    bluez_gatt_characteristic1_set_write_acquired(aChar, TRUE);

    Bluez_gatt_characteristic1_complete_acquire_write_with_fd(aInvocation, fds[1], conn->mMtu);
    close(fds[1]);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicAcquireWriteError(BluezGattCharacteristic1 *aChar,
                                                     GDBusMethodInvocation *   aInvocation,
                                                     GVariant *                aOptions)
{
    ChipLogProgress(DeviceLayer, "BluezCharacteristicAcquireWriteError is called");
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "AcquireWrite for characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicAcquireNotify(BluezGattCharacteristic1 *aChar,
                                                 GDBusMethodInvocation *   aInvocation,
                                                 GVariant *                aOptions,
                                                 gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    BluezConnection *conn = GetBluezConnectionViaDevice(endpoint);
    int                fds[2] = {-1, -1};
    GIOChannel *       channel;
    char *             errStr;
    char *             debugStr;
    GVariantDict       options;

    //VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
    //                                                                         "No Toble connection"));

    debugStr = g_variant_print(aOptions, TRUE);
    ChipLogProgress(DeviceLayer, "TRACE: AcquireNotify options %s", debugStr);
    g_free(debugStr);

    g_variant_dict_init(&options, aOptions);
    if ((g_variant_dict_contains(&options, "mtu") == TRUE))
    {
        GVariant *v = g_variant_dict_lookup_value(&options, "mtu", G_VARIANT_TYPE_UINT16);
        conn->mMtu  = g_variant_get_uint16(v);
    }

    if (bluez_gatt_characteristic1_get_notifying(aChar))
    {
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotPermitted", "Already notifying");
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
    {
        errStr = strerror(errno);
        ChipLogProgress(DeviceLayer, "FAIL: socketpair: %s in %s", errStr, __func__);
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed", "FD creation failed");
        SuccessOrExit(false);
    }
    channel = g_io_channel_unix_new(fds[0]);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_close_on_unref(channel, TRUE);
    g_io_channel_set_buffered(channel, FALSE);
    conn->c2Channel.channel = channel;
    // no read watch on c2Channel characteristic; its just for writing
    conn->c2Channel.watch = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT_IDLE, (GIOCondition)(G_IO_HUP | G_IO_ERR | G_IO_NVAL),
                                                bluezCharacteristicDestroyFD, conn, NULL);

    bluez_gatt_characteristic1_set_notify_acquired(aChar, TRUE);

    // same reply as for AcquireWrite
    Bluez_gatt_characteristic1_complete_acquire_write_with_fd(aInvocation, fds[1], conn->mMtu);
    close(fds[1]);

    conn->mIsNotify = true;
    BLEManagerImpl::WoBLEz_SubscriptionChange((void *)conn);
    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC2Subscribed), gOpenThreadInstance, conn, true);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicAcquireNotifyError(BluezGattCharacteristic1 *aChar,
                                                      GDBusMethodInvocation *   aInvocation,
                                                      GVariant *                aOptions)
{
    ChipLogProgress(DeviceLayer, "TRACE: AcquireNotify is called");
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "AcquireNotify for characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicStartNotify(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation, gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    BluezConnection *conn = GetBluezConnectionViaDevice(endpoint);

    ChipLogProgress(DeviceLayer, "start notify1");
    //VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
    //                                                                         "No Toble connection"));

    ChipLogProgress(DeviceLayer, "start notify");

    if (bluez_gatt_characteristic1_get_notifying(aChar) == TRUE)
    {
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                   "Characteristic is already subscribed");
    }
    else
    {
        bluez_gatt_characteristic1_complete_start_notify(aChar, aInvocation);
        bluez_gatt_characteristic1_set_notifying(aChar, TRUE);
        conn->mIsNotify = true;
        BLEManagerImpl::WoBLEz_SubscriptionChange(conn);
        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC2Subscribed), conn, true);
    }

exit:
    return TRUE;
}

static gboolean BluezCharacteristicStartNotifyError(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "Subscribing to characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicStopNotify(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation, gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    BluezConnection *conn = GetBluezConnectionViaDevice(endpoint);

    VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                                             "No Toble connection"));

    if (bluez_gatt_characteristic1_get_notifying(aChar) == FALSE)
    {
        g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                   "Characteristic is already unsubscribed");
    }
    else
    {
        bluez_gatt_characteristic1_complete_start_notify(aChar, aInvocation);
        bluez_gatt_characteristic1_set_notifying(aChar, FALSE);

        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC2Subscribed), conn, false);
    }
    conn->mIsNotify = false;

exit:
    return TRUE;
}

static gboolean BluezCharacteristicConfirm(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation, gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    BluezConnection *conn = GetBluezConnectionViaDevice(endpoint);

    ChipLogDetail(Ble, "Indication confirmation, %p", conn);
    BLEManagerImpl::WoBLEz_IndicationConfirmation(conn);

    return TRUE;
}

static gboolean BluezCharacteristicStopNotifyError(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                               "Unsubscribing from characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicConfirmError(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                               "Confirm from characteristic is unsupported");
    return TRUE;
}

// both arguments allocated, and non-null
static void BluezStringAddressToCHIPAddress(const char *aAddressString, BluezAddress *apAddress)
{
    size_t i, j = 0;
    for (i = 0; i < BLUEZ_ADDRESS_SIZE; i++)
    {
        apAddress->mAddress[i] = (CHAR_TO_NIBBLE(aAddressString[j]) << 4) + CHAR_TO_NIBBLE(aAddressString[j + 1]);
        j += 2;
        if (aAddressString[j] == ':')
            j++;
    }
}

static void BluezCHIPAddressToStringAddress(const BluezAddress *apAddress, char *aAddressString)
{
    sprintf(aAddressString, "%02X:%02X:%02X:%02X:%02X:%02X", apAddress->mAddress[0], apAddress->mAddress[1],
            apAddress->mAddress[2], apAddress->mAddress[3], apAddress->mAddress[4],
            apAddress->mAddress[5]);
}

static gboolean BluezIsDeviceOnAdapter(BluezDevice1 *aDevice, BluezAdapter1 *aAdapter)
{
    return strcmp(bluez_device1_get_adapter(aDevice), g_dbus_proxy_get_object_path(G_DBUS_PROXY(aAdapter))) == 0
           ? TRUE
           : FALSE;
}

static gboolean BluezIsServiceOnDevice(BluezGattService1 *aService, BluezDevice1 *aDevice)
{
    ChipLogProgress(DeviceLayer, "Device1 %s", bluez_gatt_service1_get_device(aService));
    ChipLogProgress(DeviceLayer, "Device1 %s", g_dbus_proxy_get_object_path(G_DBUS_PROXY(aDevice)));

    return strcmp(bluez_gatt_service1_get_device(aService), g_dbus_proxy_get_object_path(G_DBUS_PROXY(aDevice))) == 0
           ? TRUE
           : FALSE;
}

static gboolean BluezIsCharOnService(BluezGattCharacteristic1 *aChar, BluezGattService1 *aService)
{
    ChipLogProgress(DeviceLayer, "Char1 %s", bluez_gatt_characteristic1_get_service(aChar));
    ChipLogProgress(DeviceLayer, "Char1 %s", g_dbus_proxy_get_object_path(G_DBUS_PROXY(aService)));
    return strcmp(bluez_gatt_characteristic1_get_service(aChar),
                  g_dbus_proxy_get_object_path(G_DBUS_PROXY(aService))) == 0
           ? TRUE
           : FALSE;
}

static uint16_t BluezUUIDStringToShortServiceID(const char *aService)
{
    uint32_t shortService = 0;
    size_t   i;
    // check that the service is a short UUID

    if (strncasecmp(CHIP_BLE_BASE_SERVICE_UUID_STRING, aService + CHIP_BLE_SERVICE_PREFIX_LENGTH,
                    sizeof(CHIP_BLE_BASE_SERVICE_UUID_STRING)) == 0)
    {
        for (i = 0; i < 4; i++)
        {
            shortService =
                    (shortService << 8) | (CHAR_TO_NIBBLE(aService[2 * i]) << 4) | CHAR_TO_NIBBLE(aService[2 * i + 1]);
        }

        ChipLogProgress(DeviceLayer, "TRACE: full service UUID: %s, short service %08x", aService, shortService);

        if (shortService > UINT16_MAX)
        {
            shortService = 0;
        }
    }
    return (uint16_t)shortService;
}

/***********************************************************************
 * connection to the peripheral
 ***********************************************************************/

/**
 * Initialize the otPlatTobleConnection based on the current state of the bluez objects
 */

static void BluezConnectionInit(BluezConnection *apConn)
{
    // populate the service and the characteristics
    GList *objects = NULL;
    GList *l;

    BluezServerEndpoint *endpoint = apConn->mpEndpoint;

    if (!endpoint->isCentral)
    {
        apConn->mIsCentral = false;
        apConn->mService   = BLUEZ_GATT_SERVICE1(g_object_ref(apConn->mpEndpoint->service));
        apConn->c1         = BLUEZ_GATT_CHARACTERISTIC1(g_object_ref(endpoint->c1));
        apConn->c2         = BLUEZ_GATT_CHARACTERISTIC1(g_object_ref(endpoint->c2));
    }
    else
    {
        apConn->mIsCentral = true;
        objects           = g_dbus_object_manager_get_objects(endpoint->objMgr);

        for (l = objects; l != NULL; l = l->next)
        {
            BluezObject *      object  = BLUEZ_OBJECT(l->data);
            BluezGattService1 *service = bluez_object_get_gatt_service1(object);

            if (service != NULL)
            {
                if ((BluezIsServiceOnDevice(service, apConn->mDevice)) == TRUE &&
                    (strcmp(bluez_gatt_service1_get_uuid(service), CHIP_BLE_UUID_SERVICE_STRING) == 0))
                {
                    apConn->mService = service;
                    break;
                }
                g_object_unref(service);
            }
        }

        VerifyOrExit(apConn->mService != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL service in %s", __func__));

        for (l = objects; l != NULL; l = l->next)
        {
            BluezObject *             object = BLUEZ_OBJECT(l->data);
            BluezGattCharacteristic1 *char1  = bluez_object_get_gatt_characteristic1(object);

            if (char1 != NULL)
            {
                if ((BluezIsCharOnService(char1, apConn->mService) == TRUE) &&
                    (strcmp(bluez_gatt_characteristic1_get_uuid(char1), CHIP_PLAT_BLE_UUID_C1_STRING) == 0))
                {
                    apConn->c1 = char1;
                }
                else if ((BluezIsCharOnService(char1, apConn->mService) == TRUE) &&
                         (strcmp(bluez_gatt_characteristic1_get_uuid(char1), CHIP_PLAT_BLE_UUID_C2_STRING) == 0))
                {
                    apConn->c2 = char1;
                }
                else
                {
                    g_object_unref(char1);
                }
                if ((apConn->c1 != NULL) && (apConn->c2 != NULL))
                {
                    break;
                }
            }
        }

        VerifyOrExit(apConn->c1 != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL C1 in %s", __func__));
        VerifyOrExit(apConn->c2 != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL C2 in %s", __func__));
    }

exit:
    if (objects != NULL)
        g_list_free_full(objects, g_object_unref);
}

static gboolean
BluezConnectionInitIdle(gpointer user_data)
{
    BluezConnection *conn = (BluezConnection *)user_data;

    ChipLogProgress(DeviceLayer, "%s", __func__);

    BluezConnectionInit(conn);
    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleConnectionIsReady), gOpenThreadInstance, conn);

    return FALSE;
}

static void BluezOTConnectionDestroy(BluezConnection *aConn)
{
    if (aConn)
    {
        if (aConn->mDevice)
            g_object_unref(aConn->mDevice);
        if (aConn->mService)
            g_object_unref(aConn->mService);
        if (aConn->c1)
            g_object_unref(aConn->c1);
        if (aConn->c2)
            g_object_unref(aConn->c2);
        if (aConn->mPeerAddress)
            g_free(aConn->mPeerAddress);
        if (aConn->c1Channel.watch > 0)
            g_source_remove(aConn->c1Channel.watch );
        if (aConn->c1Channel.channel)
            g_io_channel_unref(aConn->c1Channel.channel);
        if (aConn->c2Channel.watch > 0)
            g_source_remove(aConn->c2Channel.watch );
        if (aConn->c2Channel.channel)
            g_io_channel_unref(aConn->c2Channel.channel);

        g_free(aConn);
    }
}

static BluezGattCharacteristic1 *BluezCharacteristicCreate(BluezGattService1 *aService,
                                                           const char *       aCharName,
                                                           const char *       aUUID,
                                                           GDBusObjectManagerServer *aRoot)
{
    char *servicePath =
            g_strdup(g_dbus_object_get_object_path(g_dbus_interface_get_object(G_DBUS_INTERFACE(aService))));
    char *                    charPath = g_strdup_printf("%s/%s", servicePath, aCharName);
    BluezObjectSkeleton *     object;
    BluezGattCharacteristic1 *characteristic;

    ChipLogProgress(DeviceLayer, "CREATE characteristic object at %s", charPath);
    object = bluez_object_skeleton_new(charPath);

    characteristic = bluez_gatt_characteristic1_skeleton_new();
    bluez_gatt_characteristic1_set_uuid(characteristic, aUUID);
    bluez_gatt_characteristic1_set_service(characteristic, servicePath);
    // Value unset at this point
    // WriteAcquired unset at this point
    // NotifyAcquired unset at this point
    // Notifying unset at this point
    // Flags unset at this point

    bluez_object_skeleton_set_gatt_characteristic1(object, characteristic);
    g_dbus_object_manager_server_export(aRoot, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return characteristic;
}

static void BluezPeripheralRegisterAppDone(GObject *aObject, GAsyncResult *aResult, gpointer aClosure)
{
    GError *           error   = NULL;
    BluezGattManager1 *gattMgr = BLUEZ_GATT_MANAGER1(aObject);

    gboolean success = bluez_gatt_manager1_call_register_application_finish(gattMgr, aResult, &error);

    VerifyOrExit(success == TRUE, ChipLogProgress(DeviceLayer, "FAIL: RegisterApplication : %s", error->message));

    BLEManagerImpl::NotifyBluezPeripheralRegisterAppComplete(true, NULL);
    ChipLogProgress(DeviceLayer, "BluezPeripheralRegisterAppDone done");
exit:
    if (error != NULL)
    {
        BLEManagerImpl::NotifyBluezPeripheralRegisterAppComplete(false, NULL);
        g_error_free(error);
    }
    return;
}

gboolean BluezPeripheralRegisterApp(void *apClosure)
{
    GDBusObject *      adapter;
    BluezGattManager1 *gattMgr;
    GVariantBuilder    optionsBuilder;
    GVariant *         options;

    BluezServerEndpoint *endpoint = (BluezServerEndpoint *) apClosure;
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

    adapter = g_dbus_interface_get_object(G_DBUS_INTERFACE(endpoint->adapter));
    VerifyOrExit(adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adapter in %s", __func__));

    gattMgr = bluez_object_get_gatt_manager1(BLUEZ_OBJECT(adapter));
    VerifyOrExit(gattMgr != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL gattMgr in %s", __func__));

    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    options = g_variant_builder_end(&optionsBuilder);

    bluez_gatt_manager1_call_register_application(gattMgr, endpoint->rootPath, options, NULL,
                                                  BluezPeripheralRegisterAppDone, NULL);

exit:
    return G_SOURCE_REMOVE;
}

/***********************************************************************
 * GATT Characteristic object
 ***********************************************************************/

static void BluezHandleAdvertisementFromDevice(BluezDevice1 *aDevice)
{
    const char *    address     = bluez_device1_get_address(aDevice);
    int16_t         rssi        = bluez_device1_get_rssi(aDevice);
    GVariant *      serviceData = bluez_device1_get_service_data(aDevice);
    GVariant *      entry;
    GVariantIter    iter;
    ChipAdvType  type;
    BluezAddress *src;
    const uint8_t * tmpBuf;
    uint8_t *       buf;
    size_t          len, dataLen;
    size_t          i, j;
    uint16_t        serviceId;
    char *          debugStr;

    // service data is optional and may not be present
    SuccessOrExit(serviceData != NULL);

    src = g_new0(BluezAddress, 1);

    BluezStringAddressToCHIPAddress(address, src);
    type     = ChipAdvType::BLUEZ_ADV_TYPE_CONNECTABLE;
    debugStr = g_variant_print(serviceData, TRUE);
    ChipLogProgress(DeviceLayer, "TRACE: Device %s Service data: %s", address, debugStr);
    g_free(debugStr);
    // advertising flags
    ChipLogProgress(DeviceLayer, "TRACE: Device %s Advertising flags: %s", address, bluez_device1_get_advertising_flags(aDevice));

    g_variant_iter_init(&iter, serviceData);

    len = 0;
    while ((entry = g_variant_iter_next_value(&iter)) != NULL)
    {
        GVariant *key    = g_variant_get_child_value(entry, 0);
        GVariant *val    = g_variant_get_child_value(entry, 1);
        GVariant *rawVal = g_variant_get_variant(val);

        serviceId = BluezUUIDStringToShortServiceID(g_variant_get_string(key, &dataLen));

        if (serviceId != 0)
        {
            len += 1 + sizeof(uint8_t) + sizeof(serviceId) + g_variant_n_children(rawVal);
        }
    }

    if (len != 0)
    {
        // we only parsed services, add 3 bytes for flags
        len      = len + 3;
        buf      = (uint8_t *)g_malloc(len);
        i        = 0;
        buf[i++] = 2; // length of flags
        buf[i++] = BLUEZ_ADV_TYPE_FLAGS;
        buf[i++] = BLUEZ_ADV_FLAGS_LE_DISCOVERABLE | BLUEZ_ADV_FLAGS_EDR_UNSUPPORTED;
        g_variant_iter_init(&iter, serviceData);
        while ((entry = g_variant_iter_next_value(&iter)) != NULL)
        {
            GVariant *key    = g_variant_get_child_value(entry, 0);
            GVariant *val    = g_variant_get_child_value(entry, 1);
            GVariant *rawVal = g_variant_get_variant(val);
            serviceId        = BluezUUIDStringToShortServiceID(g_variant_get_string(key, &dataLen));

            if (serviceId != 0)
            {
                buf[i++] = g_variant_n_children(rawVal) + 2 + 1;
                buf[i++] = BLUEZ_ADV_TYPE_SERVICE_DATA;
                buf[i++] = serviceId & 0xff;
                buf[i++] = (serviceId >> 8) & 0xff;
                tmpBuf   = (const uint8_t *)g_variant_get_fixed_array(rawVal, &dataLen, sizeof(uint8_t));
                for (j = 0; j < dataLen; j++)
                {
                    buf[i++] = tmpBuf[j];
                }
            }
        }

        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleAdv), gOpenThreadInstance, type, src, buf, len, rssi);
    }

exit:
    return;
}

static void BluezSignalInterfacePropertiesChanged(GDBusObjectManagerClient *aManager,
                                                  GDBusObjectProxy *        aObject,
                                                  GDBusProxy *              aInterface,
                                                  GVariant *                aChangedProperties,
                                                  const gchar *const *      aInvalidatedProps,
                                                  gpointer                  aClosure)
{

    BluezServerEndpoint * endpoint = (BluezServerEndpoint *) aClosure;
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

    ChipLogProgress(DeviceLayer, "property change comes");
    if (strcmp(g_dbus_proxy_get_interface_name(aInterface), CHARACTERISTIC_INTERFACE) == 0)
    {
        BluezGattCharacteristic1 *chr = BLUEZ_GATT_CHARACTERISTIC1(aInterface);
        BluezConnection *conn = BluezCharacteristicGetBluezConnection(chr, NULL, endpoint);

        if (conn)
        {
            GVariantIter  iter;
            GVariant *    value;
            char *        key;

            g_variant_iter_init(&iter, aChangedProperties);
            while (g_variant_iter_next(&iter, "{&sv}", &key, &value))
            {
                if (strcmp(key, "Value") == 0)
                {
                    gconstpointer data;
                    uint8_t * buf;
                    size_t len;

                    data = g_variant_get_fixed_array(value, &len, sizeof(uint8_t));

                    /* TODO why is otPlat API limited to 255 bytes? */
                    if (len > 255)
                        len = 255;

                    buf = (uint8_t *)g_memdup(data, len);

                    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC2Indication), gOpenThreadInstance, conn, buf, len);
                }
                g_variant_unref(value);
            }
        }
    }
    else if (strcmp(g_dbus_proxy_get_interface_name(aInterface), DEVICE_INTERFACE) == 0)
    {
        BluezDevice1 *device = BLUEZ_DEVICE1(aInterface);
        GVariantIter  iter;
        GVariant *    value;
        char *        key;

        ChipLogProgress(DeviceLayer, "device interface property change comes");
        if (BluezIsDeviceOnAdapter(device, endpoint->adapter))
        {
            BluezConnection *conn =
                    (BluezConnection *)g_hash_table_lookup(endpoint->connMap, g_dbus_proxy_get_object_path(aInterface));

            g_variant_iter_init(&iter, aChangedProperties);
            ChipLogProgress(DeviceLayer, "look up bluez connection comes");
            while (g_variant_iter_next(&iter, "{&sv}", &key, &value))
            {
                ChipLogProgress(DeviceLayer, "coonnected?");
                if (strcmp(key, "Connected") == 0)
                {
                    gboolean connected;
                    connected = g_variant_get_boolean(value);

                    if (connected)
                    {
                        ChipLogProgress(DeviceLayer, "coonnected!");
                        // for a central, the connection has been already allocated.  For a peripheral, it has not.
                        // todo do we need this ? we could handle all connection the same wa...
                        if (endpoint->isCentral)
                            SuccessOrExit(conn != NULL);

                        if (!endpoint->isCentral)
                        {
                            VerifyOrExit(conn == NULL,
                                            ChipLogProgress(DeviceLayer, "FAIL: connection already tracked: conn: %x device: %s", conn,
                                                         g_dbus_proxy_get_object_path(aInterface)));
                            conn               = g_new0(BluezConnection, 1);
                            conn->mPeerAddress = g_strdup(bluez_device1_get_address(device));
                            conn->mDevice      = (BluezDevice1 *)g_object_ref(device);
                            conn->mpEndpoint   = endpoint;
                            BluezConnectionInit(conn);
                            endpoint->mpPeerDevicePath = g_strdup(g_dbus_proxy_get_object_path(aInterface));
                            ChipLogProgress(DeviceLayer, "coonnected, insert, conn:%p, c1:%p, c2:%p and %s", conn, conn->c1, conn->c2, endpoint->mpPeerDevicePath);
                            g_hash_table_insert(endpoint->connMap, endpoint->mpPeerDevicePath, conn);
                        }
                        // for central, we do not call BluezConnectionInit until the services have been resolved

                        BLEManagerImpl::WoBLEz_NewConnection(endpoint);
                        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleConnected), gOpenThreadInstance, conn);
                    }
                    else
                    {
                        ChipLogProgress(DeviceLayer, "try to clean up connection from connMap %s", g_strdup(g_dbus_proxy_get_object_path(aInterface)));

                        BLEManagerImpl::WoBLEz_ConnectionClosed(endpoint);
                        BluezOTConnectionDestroy(conn);
                        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleDisconnected), gOpenThreadInstance, conn);
                        g_hash_table_remove(endpoint->connMap, g_dbus_proxy_get_object_path(aInterface));
                    }
                }
                else if (strcmp(key, "ServicesResolved") == 0)
                {
                    gboolean resolved;
                    resolved = g_variant_get_boolean(value);

                    if (endpoint->isCentral && conn != NULL && resolved == TRUE)
                    {
                        /* delay to idle, this is to workaround race in handling
                         * of interface-added and properites-changed signals
                         * it looks like we cannot specify order of those
                         * handlers and currently implementation assumes
                         * that interfaces-added is called first.
                         *
                         * TODO figure out if we can avoid this
                         */
                        g_idle_add(BluezConnectionInitIdle, conn);
                    }
                }
                else if (strcmp(key, "RSSI") == 0)
                {
                    /* when discovery starts we will get this one device is
                     * found even if device object was already present
                     */
                    if (endpoint->isCentral)
                    {
                        BluezHandleAdvertisementFromDevice(device);
                    }
                }

                g_variant_unref(value);
            }
        }
    }
    exit:
    return;
}

static void BluezHandleNewDevice(BluezDevice1 *device, BluezServerEndpoint *apEndpoint)
{
    if (apEndpoint->isCentral)
    {
        BluezHandleAdvertisementFromDevice(device);
    }
    else
    {
        BluezConnection *conn;

        ChipLogProgress(DeviceLayer, "new device comes");
        SuccessOrExit(bluez_device1_get_connected(device));

        conn = (BluezConnection *)g_hash_table_lookup(apEndpoint->connMap,
                                                        g_dbus_proxy_get_object_path(G_DBUS_PROXY(device)));

        VerifyOrExit(conn == NULL,
                     ChipLogProgress(DeviceLayer, "FAIL: connection already tracked: conn: %x new device: %s", conn,
                                     g_dbus_proxy_get_object_path(G_DBUS_PROXY(device))));

        conn               = g_new0(BluezConnection, 1);
        conn->mPeerAddress = g_strdup(bluez_device1_get_address(device));
        conn->mDevice      = (BluezDevice1 *)g_object_ref(device);
        conn->mpEndpoint   = apEndpoint;
        BluezConnectionInit(conn);

        g_hash_table_insert(apEndpoint->connMap, g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(device))),
                            conn);
        ChipLogProgress(DeviceLayer, "new device comes end");
    }

exit:
    return;
}

static void BluezSignalOnObjectAdded(GDBusObjectManager *aManager, GDBusObject *aObject, gpointer aClosure)
{
    // TODO: right now we do not handle addition/removal of adapters
    // Primary focus here is to handle addition of a device

    BluezObject * o      = BLUEZ_OBJECT(aObject);
    BluezDevice1 *device = bluez_object_get_device1(o);
    BluezServerEndpoint *endpoint = (BluezServerEndpoint *)aClosure;
    if (device != NULL)
    {
        if (BluezIsDeviceOnAdapter(device, endpoint->adapter) == TRUE)
        {
            BluezHandleNewDevice(device, endpoint);
        }

        g_object_unref(device);
    }
}

static void BluezSignalOnObjectRemoved(GDBusObjectManager *aManager, GDBusObject *aObject, gpointer aClosure)
{
    // TODO: for Device1, lookup connection, and call otPlatTobleHandleDisconnected
    // for Adapter1: unclear, crash if this pertains to our adapter? at least null out the endpoint->adapter.
    // for Characteristic1, or GattService -- handle here via calling otPlatTobleHandleDisconnected, or ignore.
}

/***********************************************************************
 * GATT service object
 ***********************************************************************/
static BluezGattService1 *BluezServiceCreate(gpointer aClosure)
{
    BluezObjectSkeleton *object;
    BluezGattService1 *  service;
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *)aClosure;

    endpoint->servicePath = g_strdup_printf("%s/service", endpoint->rootPath);
    ChipLogProgress(DeviceLayer, "CREATE service object at %s", endpoint->servicePath);
    object = bluez_object_skeleton_new(endpoint->servicePath);

    service = bluez_gatt_service1_skeleton_new();
    bluez_gatt_service1_set_uuid(service, "0xFEAF");
    // device is only valid for remote services
    bluez_gatt_service1_set_primary(service, TRUE);

    // includes -- unclear whether required.  Might be filled in later
    bluez_object_skeleton_set_gatt_service1(object, service);
    g_dbus_object_manager_server_export(endpoint->root, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return service;
}

static void bluezObjectsSetup(BluezServerEndpoint *apEndpoint)
{
    GList *objects;
    GList *l;
    char * expectedPath = g_strdup_printf("%s/hci%d", BLUEZ_PATH, apEndpoint->nodeId);

    objects = g_dbus_object_manager_get_objects(apEndpoint->objMgr);
    ChipLogProgress(DeviceLayer, "debug1");
    for (l = objects; l != NULL && apEndpoint->adapter == NULL; l = l->next)
    {
        BluezObject *object = BLUEZ_OBJECT(l->data);
        GList *      interfaces;
        GList *      ll;
        interfaces = g_dbus_object_get_interfaces(G_DBUS_OBJECT(object));
        ChipLogProgress(DeviceLayer, "debug2");
        for (ll = interfaces; ll != NULL; ll = ll->next)
        {
            if (BLUEZ_IS_ADAPTER1(ll->data))
            { // we found the adapter
                ChipLogProgress(DeviceLayer, "debug3");
                BluezAdapter1 *adapter = BLUEZ_ADAPTER1(ll->data);
                char *         addr    = (char *)bluez_adapter1_get_address(adapter);
                if (apEndpoint->adapterAddr == NULL) // no adapter address provided, bind to the hci indicated by nodeid
                {
                    ChipLogProgress(DeviceLayer, "debug4");
                    if (strcmp(g_dbus_proxy_get_object_path(G_DBUS_PROXY(adapter)), expectedPath) == 0)
                    {
                        ChipLogProgress(DeviceLayer, "debug6");
                        apEndpoint->adapter = (BluezAdapter1 *)g_object_ref(adapter);
                    }
                }
                else
                {
                    ChipLogProgress(DeviceLayer, "debug5");
                    if (strcmp(apEndpoint->adapterAddr, addr) == 0)
                    {
                        apEndpoint->adapter = (BluezAdapter1 *)g_object_ref(adapter);
                    }
                }
            }
        }
        g_list_free_full(interfaces, g_object_unref);
    }
    VerifyOrExit(apEndpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL apEndpoint->adapter in %s", __func__));
    bluez_adapter1_set_powered(apEndpoint->adapter, TRUE);

    // with BLE we are discoverable only when advertising so this can be
    // set once on init
    bluez_adapter1_set_discoverable_timeout(apEndpoint->adapter, 0);
    bluez_adapter1_set_discoverable(apEndpoint->adapter, TRUE);

exit:
    g_list_free_full(objects, g_object_unref);
    g_free(expectedPath);
}

/***********************************************************************
 * GATT Characteristic object
 ***********************************************************************/

static BluezConnection *GetBluezConnectionViaDevice(BluezServerEndpoint * apEndpoint)
{
    BluezConnection *retval = (BluezConnection *)g_hash_table_lookup(apEndpoint->connMap, apEndpoint->mpPeerDevicePath);
    ChipLogProgress(DeviceLayer, "print connection object %p in (%s)", retval,  __func__);
    return retval;
}

static BluezConnection *BluezCharacteristicGetBluezConnection(BluezGattCharacteristic1 *aChar, GVariant * aOptions, BluezServerEndpoint * apEndpoint)
{
    BluezConnection *retval = NULL;
    const gchar *path = NULL;
    GVariantDict options;
    GVariant *v;

    VerifyOrExit(apEndpoint->isCentral, ChipLogProgress(DeviceLayer, "it is not central"));
    ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection");
    /* TODO Unfortunately StartNotify/StopNotify doesn't provide info about
     * peer device in call params so we need look this up ourselves.
     */
    if (aOptions == NULL)
    {
        GList * objects;
        GList * l;
        GList * ll;

        objects = g_dbus_object_manager_get_objects(apEndpoint->objMgr);
        for (l = objects; l != NULL; l = l->next)
        {
            BluezDevice1 *device = bluez_object_get_device1(BLUEZ_OBJECT(l->data));
            ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection1");
            if (device != NULL)
            {
                ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection1.1");
                if (BluezIsDeviceOnAdapter(device, apEndpoint->adapter))
                {
                    ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection2");
                    for (ll = objects; ll != NULL; ll = ll->next)
                    {
                        BluezGattService1 *service = bluez_object_get_gatt_service1(BLUEZ_OBJECT(ll->data));
                        if (service != NULL)
                        {
                            ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection3");
                            if (BluezIsServiceOnDevice(service, device))
                            {
                                ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection4");
                                if (BluezIsCharOnService(aChar, service))
                                {
                                    ChipLogProgress(DeviceLayer, "loopup BluezCharacteristicGetBluezConnection5");
                                    retval = (BluezConnection *)g_hash_table_lookup(apEndpoint->connMap,
                                                                                    g_dbus_proxy_get_object_path(G_DBUS_PROXY(device)));
                                }
                            }
                            g_object_unref(service);
                            if (retval != NULL)
                                break;
                        }
                    }
                }
                g_object_unref(device);
                if (retval != NULL)
                    break;
            }
        }

        g_list_free_full(objects, g_object_unref);
    }
    else {
        ChipLogProgress(DeviceLayer, "lookup device in aoptions");
        g_variant_dict_init(&options, aOptions);

        v = g_variant_dict_lookup_value(&options, "device", G_VARIANT_TYPE_OBJECT_PATH);

        VerifyOrExit(v != NULL,
                     ChipLogProgress(DeviceLayer, "FAIL: No device option in dictionary (%s)", __func__));

        path = g_variant_get_string(v, NULL);

        retval = (BluezConnection *)g_hash_table_lookup(apEndpoint->connMap, path);
    }
    ChipLogProgress(DeviceLayer, "print connection object %p in (%s)", retval,  __func__);
exit:
    return retval;
}

void BluezObjectsCleanup(BluezServerEndpoint *apEndpoint)
{
    g_object_unref(apEndpoint->adapter);
}

static void BluezPeripheralObjectsSetup(gpointer aClosure)
{

    static const char *const c1_flags[] = {"write", NULL};
    static const char *const c2_flags[] = {"read", "indicate", NULL};

    BluezServerEndpoint *endpoint = (BluezServerEndpoint *)aClosure;
    endpoint->service             = BluezServiceCreate(aClosure);
    // C1 characteristic
    endpoint->c1 =
            BluezCharacteristicCreate(endpoint->service, g_strdup("c1"), g_strdup(CHIP_PLAT_BLE_UUID_C1_STRING), endpoint->root);
    bluez_gatt_characteristic1_set_flags(endpoint->c1, c1_flags);

    g_signal_connect(endpoint->c1, "handle-read-value", G_CALLBACK(BluezCharacteristicReadValue), aClosure);
    g_signal_connect(endpoint->c1, "handle-write-value", G_CALLBACK(BluezCharacteristicWriteValueError), NULL);
    g_signal_connect(endpoint->c1, "handle-acquire-write", G_CALLBACK(BluezCharacteristicAcquireWrite), aClosure);
    g_signal_connect(endpoint->c1, "handle-acquire-notify", G_CALLBACK(BluezCharacteristicAcquireNotifyError), NULL);
    g_signal_connect(endpoint->c1, "handle-start-notify", G_CALLBACK(BluezCharacteristicStartNotifyError), NULL);
    g_signal_connect(endpoint->c1, "handle-stop-notify", G_CALLBACK(BluezCharacteristicStopNotifyError), NULL);
    g_signal_connect(endpoint->c1, "handle-confirm", G_CALLBACK(BluezCharacteristicConfirmError), NULL);

    endpoint->c2 =
            BluezCharacteristicCreate(endpoint->service, g_strdup("c2"), g_strdup(CHIP_PLAT_BLE_UUID_C2_STRING), endpoint->root);
    bluez_gatt_characteristic1_set_flags(endpoint->c2, c2_flags);
    g_signal_connect(endpoint->c2, "handle-read-value", G_CALLBACK(BluezCharacteristicReadValue), aClosure);
    g_signal_connect(endpoint->c2, "handle-write-value", G_CALLBACK(BluezCharacteristicWriteValueError), NULL);
    g_signal_connect(endpoint->c2, "handle-acquire-write", G_CALLBACK(BluezCharacteristicAcquireWriteError), NULL);
    g_signal_connect(endpoint->c2, "handle-acquire-notify", G_CALLBACK(BluezCharacteristicAcquireNotify), aClosure);
    g_signal_connect(endpoint->c2, "handle-start-notify", G_CALLBACK(BluezCharacteristicStartNotify), aClosure);
    g_signal_connect(endpoint->c2, "handle-stop-notify", G_CALLBACK(BluezCharacteristicStopNotify), aClosure);
    g_signal_connect(endpoint->c2, "handle-confirm", G_CALLBACK(BluezCharacteristicConfirm), aClosure);

    ChipLogProgress(DeviceLayer, "CHIP BTP C1 %s", bluez_gatt_characteristic1_get_service(endpoint->c1));
    ChipLogProgress(DeviceLayer, "CHIP BTP C2 %s", bluez_gatt_characteristic1_get_service(endpoint->c2));

#if 0
    VerifyOrExit(endpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL endpoint->adapter in %s", __func__));

exit:
if (error != NULL)
    g_error_free(error);
return;
#endif

}

static void BluezOnBusAcquired(GDBusConnection *aConn, const gchar *aName, gpointer aClosure)
{
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *)aClosure;
    ChipLogProgress(DeviceLayer, "TRACE: Bus acquired for name %s", aName);

    endpoint->rootPath = g_strdup_printf("/chipoble/%04x", getpid() & 0xffff);
    endpoint->root     = g_dbus_object_manager_server_new(endpoint->rootPath);
    g_dbus_object_manager_server_set_connection(endpoint->root, aConn);
    BluezPeripheralObjectsSetup(aClosure);
}

static void BluezOnNameAcquired(GDBusConnection *aConn, const gchar *aName, gpointer aClosure)
{
    ChipLogProgress(DeviceLayer, "TRACE: Owning name: Acquired %s", aName);
}

static void BluezOnNameLost(GDBusConnection *aConn, const gchar *aName, gpointer aClosure)
{
    ChipLogProgress(DeviceLayer, "TRACE: Owning name: lost %s", aName);
}

static void *BluezMainLoop(void *aClosure)
{
    GDBusObjectManager *manager;
    GError *            error = NULL;
    guint               id;
    GDBusConnection *   conn;
    BluezServerEndpoint * endpoint = (BluezServerEndpoint *)aClosure;
    conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    VerifyOrExit(conn != NULL, ChipLogProgress(DeviceLayer, "FAIL: get bus sync in %s, error: %s", __func__, error->message));

    if (endpoint->adapterName != NULL)
        endpoint->owningName = g_strdup_printf("%s", endpoint->adapterName);
    else
        endpoint->owningName = g_strdup_printf("C-%04x", getpid() & 0xffff);

    BluezOnBusAcquired(conn, endpoint->owningName, aClosure);

    manager = g_dbus_object_manager_client_new_sync(
        conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ_INTERFACE, "/", bluez_object_manager_client_get_proxy_type,
        NULL /* unused user data in the Proxy Type Func */, NULL /*destroy notify */, NULL /* cancellable */, &error);

    //   manager = bluez_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
    //   G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
    //                                                       BLUEZ_INTERFACE, "/", NULL, /* GCancellable */
    //                                                       &error);

    VerifyOrExit(manager != NULL, ChipLogProgress(DeviceLayer, "FAIL: Error getting object manager client: %s", error->message));

    endpoint->objMgr = manager;

    bluezObjectsSetup(endpoint);

#if 0
    // reenable if we want to handle the bluetoothd restart
    g_signal_connect (manager,
                      "notify::name-owner",
                      G_CALLBACK (on_notify_name_owner),
                      NULL);
#endif
    g_signal_connect(manager, "object-added", G_CALLBACK(BluezSignalOnObjectAdded), aClosure);
    g_signal_connect(manager, "object-removed", G_CALLBACK(BluezSignalOnObjectRemoved), aClosure);
    g_signal_connect(manager, "interface-proxy-properties-changed", G_CALLBACK(BluezSignalInterfacePropertiesChanged),
                     aClosure);

    // id    = g_bus_own_name(G_BUS_TYPE_SYSTEM, endpoint->owningName,
    //                      G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
    //                      BluezOnBusAcquired,
    //                  BluezOnNameAcquired, BluezOnNameLost, NULL, NULL);

    ChipLogProgress(DeviceLayer, "TRACE: Bluez mainloop starting %s", __func__);
    g_main_loop_run(sBluezMainLoop);
    ChipLogProgress(DeviceLayer, "TRACE: Bluez mainloop stopping %s", __func__);

    g_bus_unown_name(id);
    BluezObjectsCleanup(endpoint);

exit:
    if (error != NULL)
        g_error_free(error);
    return NULL;
}

bool BluezRunOnBluezThread(int (*aCallback)(void *), void *aClosure)
{
    GMainContext *context = NULL;
    const char *  msg     = NULL;

    VerifyOrExit(sBluezMainLoop != NULL, msg = "FAIL: NULL sBluezMainLoop");
    VerifyOrExit(g_main_loop_is_running(sBluezMainLoop),
                    msg = "FAIL: sBluezMainLoop not running");

    context = g_main_loop_get_context(sBluezMainLoop);
    VerifyOrExit(context != NULL, msg = "FAIL: NULL main context");
    g_main_context_invoke(context, aCallback, aClosure);

exit:
    if (msg != NULL)
    {
        ChipLogProgress(DeviceLayer, "%s in %s", msg, __func__);
    }

    return msg == NULL;
}

uint16_t GetBluezMTU(BLE_CONNECTION_OBJECT connObj)
{
    return 104;
}

/***********************************************************************
 * C2 indication
 ***********************************************************************/

static gboolean bluezTobleC2Indicate(void *aClosure)
{
    connectionDataBundle *closure = (connectionDataBundle *)aClosure;
    BluezConnection *     conn    = closure->mpConn;
    GError *              error   = NULL;
    GIOStatus             status;
    const char *          buf;
    size_t                len, written;

    VerifyOrExit(conn->c2 != NULL, ChipLogProgress(DeviceLayer, "FAIL: C2 Indicate: %s", "NULL C2"));

    ChipLogProgress(DeviceLayer, "bluezTobleC2Indicate");

    if (bluez_gatt_characteristic1_get_notify_acquired(conn->c2) == TRUE)
    {
        ChipLogProgress(DeviceLayer, "bluezTobleC2Indicate acquire");
        buf    = (char *)g_variant_get_fixed_array(closure->mpVal, &len, sizeof(uint8_t));
        status = g_io_channel_write_chars(conn->c2Channel.channel, buf, len, &written, &error);
        g_variant_unref(closure->mpVal);
        closure->mpVal = NULL;

        VerifyOrExit(status == G_IO_STATUS_NORMAL, ChipLogProgress(DeviceLayer, "FAIL: C2 Indicate: %s", error->message));
    }
    else
    {
        ChipLogProgress(DeviceLayer, "bluezTobleC2Indicate not acquire");
        bluez_gatt_characteristic1_set_value(conn->c2, closure->mpVal);
        closure->mpVal = NULL;
    }

exit:
    if (closure->mpVal)
        g_variant_unref(closure->mpVal);
    g_free(closure);

    if (error != NULL)
        g_error_free(error);
    return G_SOURCE_REMOVE;
}

bool WoBLEz_ScheduleSendIndication(void * apConn, chip::System::PacketBuffer * apBuf)
{
    connectionDataBundle *closure;
    const char * msg               = NULL;
    bool success                   = false;
    uint8_t * buffer                          = apBuf->Start();
    size_t len                                = apBuf->DataLength();

    closure = g_new(connectionDataBundle, 1);
    closure->mpConn = (BluezConnection*)apConn;
    closure->mpVal  = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, buffer, len * sizeof(uint8_t), sizeof(uint8_t));

    ChipLogProgress(DeviceLayer, "WoBLEz_ScheduleSendIndication");
    success = BluezRunOnBluezThread(bluezTobleC2Indicate, closure);

    if (NULL != msg)
    {
        ChipLogError(Ble, msg);
    }

    if (NULL != apBuf)
    {
        chip::System::PacketBuffer::Free(apBuf);
    }

    return success;
}

void StartBluezAdv(void * apAppState)
{
    BluezRunOnBluezThread(BluezTobleAdvStart, apAppState);
}

void StopBluezAdv(void * apAppState)
{
    BluezRunOnBluezThread(BluezTobleAdvStop, apAppState);
}

void BluezBleAdvertisementSetup(void *apAppState)
{
    BluezRunOnBluezThread(BluezBleAdvSetup, apAppState);
}

void BluezBleGattsAppRegister(void *apAppState)
{
    BluezRunOnBluezThread(BluezPeripheralRegisterApp, apAppState);
}

/**
 * Initialize BlueZ platform beyond the otPlatTobleInit arguments
 *
 * @param[in] aIsCentral  A boolean checking determining whether to configure the device in Central or peripheral mode
 * @param[in] aBleAddr A string corresponding to the BLE address of the HCI interface to bind to.  When NULL, the system
 * will bind to the first adapter that's found
 * @param[in] aBleName When non-null, adapter alias will be set to this string.
 *
 */
CHIP_ERROR InitBluezBleLayer(BleConfig & aBleConfig, void *& apEndpoint)
{
    bool retval     = false;
    int  pthreadErr = 0;
    int  tmpErrno;
    BLEManagerImpl * bLEManagerImpl = NULL;
    BluezServerEndpoint * endpoint = NULL;
    VerifyOrExit(pipe2(sBluezFD, O_DIRECT) == 0, ChipLogProgress(DeviceLayer, "FAIL: open pipe in %s", __func__));

    // initialize server endpoint
    endpoint = g_new0(BluezServerEndpoint, 1);
    VerifyOrExit(endpoint != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory allocation in %s", __func__));

    // initialize server endpoint
    VerifyOrExit(endpoint != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory allocation in %s", __func__));

    ChipLogProgress(DeviceLayer, "debug aBleName %s", aBleConfig.mpBleName);
    if (aBleConfig.mpBleName != NULL)
        endpoint->adapterName = g_strdup(aBleConfig.mpBleName);
    else
        endpoint->adapterName = NULL;

    if (aBleConfig.mpBleAddr != NULL)
        endpoint->adapterAddr = g_strdup(aBleConfig.mpBleAddr);
    else
        endpoint->adapterAddr = NULL;

    endpoint->nodeId = aBleConfig.mNodeId;

    endpoint->connMap   = g_hash_table_new(g_str_hash, g_str_equal);
    endpoint->isCentral = aBleConfig.mIsCentral;

    /**
* Data arranged in "Length Type Value" pairs inside Weave service data.
* Length should include size of value + size of Type field, which is 1 byte
*/
    endpoint->mType = aBleConfig.mType;
    endpoint->mDuration = aBleConfig.mDuration;
    endpoint->chipServiceData = (CHIPServiceData * )g_malloc(sizeof(CHIPServiceData) + 1);
    endpoint->advertisingUUID = g_strdup("0xFEAF");
    endpoint->chipServiceData->dataBlock0Len = sizeof(CHIPIdInfo) + 1;
    endpoint->chipServiceData->dataBlock0Type = 1;
    endpoint->chipServiceData->idInfo.major         = aBleConfig.mMajor;
    endpoint->chipServiceData->idInfo.minor         = aBleConfig.mMinor;
    endpoint->chipServiceData->idInfo.vendorId      = aBleConfig.mVendorId;
    endpoint->chipServiceData->idInfo.productId     = aBleConfig.mProductId;
    endpoint->chipServiceData->idInfo.deviceId      = aBleConfig.mDeviceId;
    endpoint->chipServiceData->idInfo.pairingStatus = aBleConfig.mPairingStatus;
    endpoint->mDuration = aBleConfig.mDuration;
    endpoint->mIsAdvertising = false;

    // peripheral properties

    VerifyOrExit(endpoint != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory alloc in %s", __func__));

    sBluezMainLoop = g_main_loop_new(NULL, FALSE);
    VerifyOrExit(sBluezMainLoop != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory alloc in %s", __func__));

    pthreadErr = pthread_create(&sBluezThread, NULL, BluezMainLoop, endpoint);
    tmpErrno   = errno;
    VerifyOrExit(pthreadErr == 0, ChipLogProgress(DeviceLayer, "FAIL: pthread_create (%s) in %s", strerror(tmpErrno), __func__));
    sleep(1);



    //BluezBleGattsAppRegister(endpoint);
    retval = TRUE;

    //StartBluezAdv();
    //BluezRunOnBluezThread(BluezTobleAdvStart, NULL);
exit:
    if (retval)
    {
        apEndpoint = endpoint;
        ChipLogProgress(DeviceLayer, "PlatformBlueZInit init success");
    }
    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

