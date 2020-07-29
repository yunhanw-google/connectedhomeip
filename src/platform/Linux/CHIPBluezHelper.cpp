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



using namespace ::nl;

namespace chip {
namespace DeviceLayer {
namespace Internal {

#define CHIP_PLAT_BLE_UUID_THREAD_GROUP 0xfffb

#define OT_PLAT_TOBLE_UUID_C1 \
    0x11, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18

#define OT_PLAT_TOBLE_UUID_C2 \
    0x12, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18

/**
 * This type represents different BLE address types
 *
 */
typedef enum BluezAddressType
{
    BLUEZ_ADDRESS_TYPE_PUBLIC                        = 0, ///< Bluetooth public device address.
    BLUEZ_ADDRESS_TYPE_RANDOM_STATIC                 = 1, ///< Bluetooth random static address.
    BLUEZ_ADDRESS_TYPE_RANDOM_PRIVATE_RESOLVABLE     = 2, ///< Bluetooth random private resolvable address.
    BLUEZ_ADDRESS_TYPE_RANDOM_PRIVATE_NON_RESOLVABLE = 3, ///< Bluetooth random private non-resolvable address.
} BluezAddressType;

#define BLUEZ_ADDRESS_SIZE 6 ///< BLE address size (in bytes)

/**
* This type represents a BLE address.
*
*/
typedef struct BluezAddress
{
    BluezAddressType mType;                           ///< Bluetooth device address type.
    uint8_t            mAddress[BLUEZ_ADDRESS_SIZE]; ///< A 48-bit address of Bluetooth device in LSB format.
} BluezAddress;

struct io_channel
{
    GIOChannel *channel;
    guint       watch;
};

struct BluezConnection
{
    char *                    mPeerAddress;
    BluezDevice1 *            mDevice;
    BluezGattService1 *       mService;
    BluezGattCharacteristic1 *c1;
    BluezGattCharacteristic1 *c2;
    bool                      mIsCentral;
    uint16_t                  mMtu;
    struct io_channel         c1Channel;
    struct io_channel         c2Channel;
};

static int sBluezFD[2];

static GMainLoop *          sBluezMainLoop = NULL;
static pthread_t            sBluezThread;
static BluezServerEndpoint *sEndpoint = NULL;

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
                                        gpointer               aClosure)
{
    ChipLogProgress(DeviceLayer, "RELEASE adv object at %s", g_dbus_proxy_get_object_path(G_DBUS_PROXY(aAdv)));

    g_dbus_object_manager_server_unexport(sEndpoint->root, sEndpoint->advPath);
    sAdvertising = FALSE;

    return TRUE;
}

static BluezLEAdvertisement1 *BluezAdvertisingCreate(ChipAdvConfig *aConfig)
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

    if (sEndpoint->advPath == NULL)
        sEndpoint->advPath = g_strdup_printf("%s/advertising", sEndpoint->rootPath);

    array[0] = g_strdup_printf("%s", sEndpoint->advertisingUUID);
    ChipLogProgress(DeviceLayer, "CREATE adv object at %s", sEndpoint->advPath);
    object = bluez_object_skeleton_new(sEndpoint->advPath);

    adv = bluez_leadvertisement1_skeleton_new();

    g_variant_builder_init(&serviceDataBuilder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&serviceUUIDsBuilder, G_VARIANT_TYPE("as"));
    offset = 0;

    g_variant_builder_add(
            &serviceDataBuilder, "{sv}", sEndpoint->advertisingUUID,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, sEndpoint->chipServiceData, sizeof(CHIPServiceData), sizeof(uint8_t)));
    g_variant_builder_add(&serviceUUIDsBuilder, "s", sEndpoint->advertisingUUID);

    localName = g_strdup_printf("T-%04x", getpid() & 0xffff);

    serviceData = g_variant_builder_end(&serviceDataBuilder);
    debugStr    = g_variant_print(serviceData, TRUE);
    ChipLogProgress(DeviceLayer, "SET service data to %s", debugStr);
    g_free(debugStr);

    bluez_leadvertisement1_set_type_(adv,
                                     (aConfig->mType & BLUEZ_ADV_TYPE_CONNECTABLE) ? "peripheral" : "broadcast");
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
    // empty duration, we don't have a clear notion what it would mean to timeslice between toble and anyone else
    bluez_leadvertisement1_set_timeout(adv, 0);
    // empty secondary channel for now

    bluez_object_skeleton_set_leadvertisement1(object, adv);
    g_signal_connect(adv, "handle-release", G_CALLBACK(BluezAdvertisingRelease), NULL);

    g_dbus_object_manager_server_export(sEndpoint->root, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return adv;
}

static void BluezTobleAdvStartDone(GObject *aObject, GAsyncResult *aResult, gpointer aClosure)
{
    BluezLEAdvertisingManager1 *advMgr = BLUEZ_LEADVERTISING_MANAGER1(aObject);
    GError *                    error  = NULL;

    gboolean success = bluez_leadvertising_manager1_call_register_advertisement_finish(advMgr, aResult, &error);

    if (success == FALSE)
    {
        g_dbus_object_manager_server_unexport(sEndpoint->root, sEndpoint->advPath);
    }

    sAdvertising = success;

    VerifyOrExit(success == TRUE, ChipLogProgress(DeviceLayer, "FAIL: RegisterAdvertisement : %s", error->message));

    ChipLogProgress(DeviceLayer, "RegisterAdvertisement complete");

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

static gboolean BluezTobleAdvStart(void *aClosure)
{
    // create and register advertising object; get the LEAdvertisingManager proxy
    // for the default adapter and register the object with the adapter

    GDBusObject *               adapter;
    ChipAdvConfig *          config = (ChipAdvConfig *)aClosure;
    BluezLEAdvertisingManager1 *advMgr = NULL;
    GVariantBuilder             optionsBuilder;
    GVariant *                  options;
    BluezLEAdvertisement1 *     adv;
    gboolean dbusSent = FALSE;

    VerifyOrExit(sAdvertising == FALSE, ChipLogProgress(DeviceLayer, "FAIL: Advertising already enabled in %s", __func__));

    sEndpoint->isCentral = false;
    VerifyOrExit(sEndpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL sEndpoint->adapter in %s", __func__));

    adapter = g_dbus_interface_get_object(G_DBUS_INTERFACE(sEndpoint->adapter));
    VerifyOrExit(adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adapter in %s", __func__));

    advMgr = bluez_object_get_leadvertising_manager1(BLUEZ_OBJECT(adapter));
    VerifyOrExit(advMgr != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL advMgr in %s", __func__));

    adv = BluezAdvertisingCreate(config);
    VerifyOrExit(adv != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adv in %s", __func__));

    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    options = g_variant_builder_end(&optionsBuilder);

    bluez_leadvertising_manager1_call_register_advertisement(advMgr, sEndpoint->advPath, options, NULL,
                                                             BluezTobleAdvStartDone, NULL);

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
static BluezConnection *bluezCharacteristicGetTobleConnection(BluezGattCharacteristic1 *aChar, GVariant * aOptions);

static gboolean BluezCharacteristicReadValue(BluezGattCharacteristic1 *aChar,
                                             GDBusMethodInvocation *   aInvocation,
                                             GVariant *                aOptions)
{
    GVariant *val;

    ChipLogProgress(DeviceLayer, "debug BluezCharacteristicReadValue");
    val = (GVariant *)bluez_gatt_characteristic1_get_value(aChar);

    bluez_gatt_characteristic1_complete_read_value(aChar, aInvocation, (const gchar*)val);

    return TRUE;
}

static gboolean BluezCharacteristicWriteValue(BluezGattCharacteristic1 *aChar,
                                              GDBusMethodInvocation *   aInvocation,
                                              GVariant *                aValue,
                                              GVariant *                aOptions)
{
    const uint8_t *    tmpBuf;
    uint8_t *          buf;
    size_t             len;
    BluezConnection *conn   = bluezCharacteristicGetTobleConnection(aChar, aOptions);
    char *             valStr = g_variant_print(aValue, TRUE);

    ChipLogProgress(DeviceLayer, "TRACE: Char write value %s", valStr);
    g_free(valStr);

    VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                                             "No CHIP Bluez connection"));

    bluez_gatt_characteristic1_set_value(aChar, (const gchar *)g_variant_ref(aValue));

    tmpBuf = (const uint8_t *)g_variant_get_fixed_array(aValue, &len, sizeof(uint8_t));
    buf    = (uint8_t *)g_memdup(tmpBuf, len);


    WoBLEz_WriteReceived(sEndpoint, buf, len);

    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC1Write), gOpenThreadInstance, conn, buf, len);

    bluez_gatt_characteristic1_complete_write_value(aChar, aInvocation);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicWriteValueError(BluezGattCharacteristic1 *aChar,
                                                   GDBusMethodInvocation *   aInvocation,
                                                   GVariant *                aValue,
                                                   GVariant *                aOptions)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "Writing to characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicWriteFD(GIOChannel *aChannel, GIOCondition aCond, gpointer aClosure)
{
    BluezConnection *conn   = (BluezConnection *)aClosure;
    GVariant *         newVal;
    gchar *          buf;
    ssize_t            len;
    int                fd;

    ChipLogProgress(DeviceLayer, "%s mtu=%d", __func__, conn->mMtu);

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
    bluez_gatt_characteristic1_set_value(conn->c1, (const gchar *)newVal);

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
                                                GVariant *                aOptions)
{
    BluezConnection *conn   = bluezCharacteristicGetTobleConnection(aChar, aOptions);
    int                fds[2] = {-1, -1};
    GIOChannel *       channel;
    char *             errStr;
    char *             debugStr;
    GVariantDict       options;

    VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                                             "No Toble connection"));

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
        conn->mMtu  = g_variant_get_uint16(v);
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
    conn->c1Channel.watch  = g_io_add_watch(channel,
                                            (GIOCondition)(G_IO_HUP | G_IO_IN | G_IO_ERR | G_IO_NVAL),
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
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "AcquireWrite for characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicAcquireNotify(BluezGattCharacteristic1 *aChar,
                                                 GDBusMethodInvocation *   aInvocation,
                                                 GVariant *                aOptions)
{
    BluezConnection *conn   = bluezCharacteristicGetTobleConnection(aChar, aOptions);
    int                fds[2] = {-1, -1};
    GIOChannel *       channel;
    char *             errStr;
    char *             debugStr;
    GVariantDict       options;

    VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                                             "No Toble connection"));

    debugStr = g_variant_print(aOptions, TRUE);
    ChipLogProgress(DeviceLayer, "TRACE: AcquireNotify options %s", debugStr);
    g_free(debugStr);

    g_variant_dict_init(&options, aOptions);
    if ((conn->mMtu == 0) && (g_variant_dict_contains(&options, "mtu") == TRUE))
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
    //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleC2Subscribed), gOpenThreadInstance, conn, true);

exit:
    return TRUE;
}

static gboolean BluezCharacteristicAcquireNotifyError(BluezGattCharacteristic1 *aChar,
                                                      GDBusMethodInvocation *   aInvocation,
                                                      GVariant *                aOptions)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.NotSupported",
                                               "AcquireNotify for characteristic is unsupported");
    return TRUE;
}

static gboolean BluezCharacteristicStartNotify(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    BluezConnection *conn = bluezCharacteristicGetTobleConnection(aChar, NULL);

    VerifyOrExit(conn != NULL, g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                                                             "No Toble connection"));

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

        WoBLEz_SubscriptionChange(gBluezServerEndpoint);
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

static gboolean BluezCharacteristicStopNotify(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    BluezConnection *conn = bluezCharacteristicGetTobleConnection(aChar, NULL);

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

exit:
    return TRUE;
}

static gboolean BluezCharacteristicConfirm(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    BluezConnection *conn = bluezCharacteristicGetTobleConnection(aChar, NULL);

    WeaveLogDetail(Ble, "Indication confirmation received at %s", characteristic->path);
    WoBLEz_IndicationConfirmation(sEndpoint);

    return TRUE;
}

static gboolean BluezCharacteristicStopNotifyError(BluezGattCharacteristic1 *aChar, GDBusMethodInvocation *aInvocation)
{
    g_dbus_method_invocation_return_dbus_error(aInvocation, "org.bluez.Error.Failed",
                                               "Unsubscribing from characteristic is unsupported");
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
    return strcmp(bluez_gatt_service1_get_device(aService), g_dbus_proxy_get_object_path(G_DBUS_PROXY(aDevice))) == 0
           ? TRUE
           : FALSE;
}

static gboolean BluezIsCharOnService(BluezGattCharacteristic1 *aChar, BluezGattService1 *aService)
{
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

static void BluezConnectionInit(BluezConnection *aConn)
{
    // populate the service and the characteristics
    GList *objects = NULL;
    GList *l;

    if (!sEndpoint->isCentral)
    {
        aConn->mIsCentral = false;
        aConn->mService   = BLUEZ_GATT_SERVICE1(g_object_ref(sEndpoint->service));
        aConn->c1         = BLUEZ_GATT_CHARACTERISTIC1(g_object_ref(sEndpoint->c1));
        aConn->c2         = BLUEZ_GATT_CHARACTERISTIC1(g_object_ref(sEndpoint->c2));
    }
    else
    {
        aConn->mIsCentral = true;
        objects           = g_dbus_object_manager_get_objects(sEndpoint->objMgr);

        for (l = objects; l != NULL; l = l->next)
        {
            BluezObject *      object  = BLUEZ_OBJECT(l->data);
            BluezGattService1 *service = bluez_object_get_gatt_service1(object);

            if (service != NULL)
            {
                if ((BluezIsServiceOnDevice(service, aConn->mDevice)) == TRUE &&
                    (strcmp(bluez_gatt_service1_get_uuid(service), CHIP_BLE_UUID_SERVICE_STRING) == 0))
                {
                    aConn->mService = service;
                    break;
                }
                g_object_unref(service);
            }
        }

        VerifyOrExit(aConn->mService != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL service in %s", __func__));

        for (l = objects; l != NULL; l = l->next)
        {
            BluezObject *             object = BLUEZ_OBJECT(l->data);
            BluezGattCharacteristic1 *char1  = bluez_object_get_gatt_characteristic1(object);

            if (char1 != NULL)
            {
                if ((BluezIsCharOnService(char1, aConn->mService) == TRUE) &&
                    (strcmp(bluez_gatt_characteristic1_get_uuid(char1), CHIP_PLAT_BLE_UUID_C1_STRING) == 0))
                {
                    aConn->c1 = char1;
                }
                else if ((BluezIsCharOnService(char1, aConn->mService) == TRUE) &&
                         (strcmp(bluez_gatt_characteristic1_get_uuid(char1), CHIP_PLAT_BLE_UUID_C2_STRING) == 0))
                {
                    aConn->c2 = char1;
                }
                else
                {
                    g_object_unref(char1);
                }
                if ((aConn->c1 != NULL) && (aConn->c2 != NULL))
                {
                    break;
                }
            }
        }

        VerifyOrExit(aConn->c1 != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL C1 in %s", __func__));
        VerifyOrExit(aConn->c2 != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL C2 in %s", __func__));
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
                                                           const char *       aUUID)
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
    g_dbus_object_manager_server_export(sEndpoint->root, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return characteristic;
}

static void BluezPeripheralRegisterAppDone(GObject *aObject, GAsyncResult *aResult, gpointer aClosure)
{
    GError *           error   = NULL;
    BluezGattManager1 *gattMgr = BLUEZ_GATT_MANAGER1(aObject);

    gboolean success = bluez_gatt_manager1_call_register_application_finish(gattMgr, aResult, &error);

    VerifyOrExit(success == TRUE, ChipLogProgress(DeviceLayer, "FAIL: RegisterApplication : %s", error->message));

exit:
    if (error != NULL)
        g_error_free(error);
    return;
}

gboolean BluezPeripheralRegisterApp(void *aClosure)
{
    GDBusObject *      adapter;
    BluezGattManager1 *gattMgr;
    GVariantBuilder    optionsBuilder;
    GVariant *         options;

    VerifyOrExit(sEndpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL sEndpoint->adapter in %s", __func__));

    adapter = g_dbus_interface_get_object(G_DBUS_INTERFACE(sEndpoint->adapter));
    VerifyOrExit(adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL adapter in %s", __func__));

    gattMgr = bluez_object_get_gatt_manager1(BLUEZ_OBJECT(adapter));
    VerifyOrExit(gattMgr != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL gattMgr in %s", __func__));

    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    options = g_variant_builder_end(&optionsBuilder);

    bluez_gatt_manager1_call_register_application(gattMgr, sEndpoint->rootPath, options, NULL,
                                                  BluezPeripheralRegisterAppDone, NULL);

exit:
    return G_SOURCE_REMOVE;
}

/***********************************************************************
 * GATT Characteristic object
 ***********************************************************************/

static BluezConnection *BluezCharacteristicGetBluezConnection(BluezGattCharacteristic1 *aChar, GVariant * aOptions)
{
    BluezConnection *retval = NULL;
    const gchar *path = NULL;
    GVariantDict options;
    GVariant *v;

    /* TODO Unfortunately StartNotify/StopNotify doesn't provide info about
     * peer device in call params so we need look this up ourselves.
     */
    if (aOptions == NULL)
    {
        GList * objects;
        GList * l;
        GList * ll;

        objects = g_dbus_object_manager_get_objects(sEndpoint->objMgr);
        for (l = objects; l != NULL; l = l->next)
        {
            BluezDevice1 *device = bluez_object_get_device1(BLUEZ_OBJECT(l->data));
            if (device != NULL)
            {
                if (BluezIsDeviceOnAdapter(device, sEndpoint->adapter))
                {
                    for (ll = objects; ll != NULL; ll = ll->next)
                    {
                        BluezGattService1 *service = bluez_object_get_gatt_service1(BLUEZ_OBJECT(ll->data));
                        if (service != NULL)
                        {
                            if (BluezIsServiceOnDevice(service, device))
                            {
                                if (BluezIsCharOnService(aChar, service))
                                {
                                    retval = (BluezConnection *)g_hash_table_lookup(sEndpoint->connMap,
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
        g_variant_dict_init(&options, aOptions);

        v = g_variant_dict_lookup_value(&options, "device", G_VARIANT_TYPE_OBJECT_PATH);

        VerifyOrExit(v != NULL,
                        ChipLogProgress(DeviceLayer, "FAIL: No device option in dictionary (%s)", __func__));

        path = g_variant_get_string(v, NULL);

        retval = (BluezConnection*) g_hash_table_lookup(sEndpoint->connMap, path);
    }

exit:
    return retval;
}

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
    type     = BLUEZ_ADV_TYPE_CONNECTABLE;
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
    SuccessOrExit(sEndpoint->adapter != NULL);

    if (strcmp(g_dbus_proxy_get_interface_name(aInterface), CHARACTERISTIC_INTERFACE) == 0)
    {
        BluezGattCharacteristic1 *chr = BLUEZ_GATT_CHARACTERISTIC1(aInterface);
        BluezConnection *conn = BluezCharacteristicGetBluezConnection(chr, NULL);

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

        if (BluezIsDeviceOnAdapter(device, sEndpoint->adapter))
        {
            BluezConnection *conn =
                    (BluezConnection *)g_hash_table_lookup(sEndpoint->connMap, g_dbus_proxy_get_object_path(aInterface));

            g_variant_iter_init(&iter, aChangedProperties);
            while (g_variant_iter_next(&iter, "{&sv}", &key, &value))
            {
                if (strcmp(key, "Connected") == 0)
                {
                    gboolean connected;
                    connected = g_variant_get_boolean(value);
                    if (connected)
                    {
                        // for a central, the connection has been already allocated.  For a peripheral, it has not.
                        // todo do we need this ? we could handle all connection the same wa...
                        if (sEndpoint->isCentral)
                            SuccessOrExit(conn != NULL);

                        if (!sEndpoint->isCentral)
                        {
                            VerifyOrExit(conn == NULL,
                                            ChipLogProgress(DeviceLayer, "FAIL: connection already tracked: conn: %x device: %s", conn,
                                                         g_dbus_proxy_get_object_path(aInterface)));
                            conn               = g_new0(BluezConnection, 1);
                            conn->mPeerAddress = g_strdup(bluez_device1_get_address(device));
                            conn->mDevice      = (BluezDevice1 *)g_object_ref(device);
                            BluezConnectionInit(conn);

                            g_hash_table_insert(sEndpoint->connMap, g_strdup(g_dbus_proxy_get_object_path(aInterface)),
                                                conn);
                        }
                        // for central, we do not call BluezConnectionInit until the services have been resolved

                        WoBLEz_NewConnection(gBluezServerEndpoint);
                        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleConnected), gOpenThreadInstance, conn);
                    }
                    else
                    {
                        WoBLEz_ConnectionClosed(gBluezServerEndpoint);
                        //bluezRunOnOTThread(TO_GENERIC_FUN(otPlatTobleHandleDisconnected), gOpenThreadInstance, conn);
                        g_hash_table_remove(sEndpoint->connMap, g_dbus_proxy_get_object_path(aInterface));
                    }
                }
                else if (strcmp(key, "ServicesResolved") == 0)
                {
                    gboolean resolved;
                    resolved = g_variant_get_boolean(value);

                    if (sEndpoint->isCentral && conn != NULL && resolved == TRUE)
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
                    if (sEndpoint->isCentral)
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

static void BluezHandleNewDevice(BluezDevice1 *device)
{
    if (sEndpoint->isCentral)
    {
        BluezHandleAdvertisementFromDevice(device);
    }
    else
    {
        BluezConnection *conn;

        SuccessOrExit(bluez_device1_get_connected(device));

        conn = (BluezConnection *)g_hash_table_lookup(sEndpoint->connMap,
                                                        g_dbus_proxy_get_object_path(G_DBUS_PROXY(device)));

        VerifyOrExit(conn == NULL,
                     ChipLogProgress(DeviceLayer, "FAIL: connection already tracked: conn: %x new device: %s", conn,
                                     g_dbus_proxy_get_object_path(G_DBUS_PROXY(device))));

        conn               = g_new0(BluezConnection, 1);
        conn->mPeerAddress = g_strdup(bluez_device1_get_address(device));
        conn->mDevice      = (BluezDevice1 *)g_object_ref(device);
        BluezConnectionInit(conn);

        g_hash_table_insert(sEndpoint->connMap, g_strdup(g_dbus_proxy_get_object_path(G_DBUS_PROXY(device))),
                            conn);
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

    if (device != NULL)
    {
        if (BluezIsDeviceOnAdapter(device, sEndpoint->adapter) == TRUE)
        {
            BluezHandleNewDevice(device);
        }

        g_object_unref(device);
    }
}

static void BluezSignalOnObjectRemoved(GDBusObjectManager *aManager, GDBusObject *aObject, gpointer aClosure)
{
    // TODO: for Device1, lookup connection, and call otPlatTobleHandleDisconnected
    // for Adapter1: unclear, crash if this pertains to our adapter? at least null out the sEndpoint->adapter.
    // for Characteristic1, or GattService -- handle here via calling otPlatTobleHandleDisconnected, or ignore.
}

/***********************************************************************
 * GATT service object
 ***********************************************************************/
static BluezGattService1 *BluezServiceCreate(void)
{
    BluezObjectSkeleton *object;
    BluezGattService1 *  service;

    sEndpoint->servicePath = g_strdup_printf("%s/service", sEndpoint->rootPath);
    ChipLogProgress(DeviceLayer, "CREATE service object at %s", sEndpoint->servicePath);
    object = bluez_object_skeleton_new(sEndpoint->servicePath);

    service = bluez_gatt_service1_skeleton_new();
    bluez_gatt_service1_set_uuid(service, "0xFEAF");
    // device is only valid for remote services
    bluez_gatt_service1_set_primary(service, TRUE);

    // includes -- unclear whether required.  Might be filled in later
    bluez_object_skeleton_set_gatt_service1(object, service);
    g_dbus_object_manager_server_export(sEndpoint->root, G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return service;
}

static void bluezObjectsSetup(GDBusObjectManager *aManager)
{
    GList *objects;
    GList *l;
    char * expectedPath = g_strdup_printf("%s/hci%d", BLUEZ_PATH, sEndpoint->nodeId);

    objects = g_dbus_object_manager_get_objects(aManager);
    ChipLogProgress(DeviceLayer, "debug1");
    for (l = objects; l != NULL && sEndpoint->adapter == NULL; l = l->next)
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
                if (sEndpoint->adapterAddr == NULL) // no adapter address provided, bind to the hci indicated by nodeid
                {
                    ChipLogProgress(DeviceLayer, "debug4");
                    if (strcmp(g_dbus_proxy_get_object_path(G_DBUS_PROXY(adapter)), expectedPath) == 0)
                    {
                        ChipLogProgress(DeviceLayer, "debug6");
                        sEndpoint->adapter = (BluezAdapter1 *)g_object_ref(adapter);
                    }
                }
                else
                {
                    ChipLogProgress(DeviceLayer, "debug5");
                    if (strcmp(sEndpoint->adapterAddr, addr) == 0)
                    {
                        sEndpoint->adapter = (BluezAdapter1 *)g_object_ref(adapter);
                    }
                }
            }
        }
        g_list_free_full(interfaces, g_object_unref);
    }
    VerifyOrExit(sEndpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL sEndpoint->adapter in %s", __func__));
    bluez_adapter1_set_powered(sEndpoint->adapter, TRUE);

    // with BLE we are discoverable only when advertising so this can be
    // set once on init
    bluez_adapter1_set_discoverable_timeout(sEndpoint->adapter, 0);
    bluez_adapter1_set_discoverable(sEndpoint->adapter, TRUE);

exit:
    g_list_free_full(objects, g_object_unref);
    g_free(expectedPath);
}

/***********************************************************************
 * GATT Characteristic object
 ***********************************************************************/

static BluezConnection *bluezCharacteristicGetTobleConnection(BluezGattCharacteristic1 *aChar, GVariant * aOptions)
{
    BluezConnection *retval = NULL;
    const gchar *path = NULL;
    GVariantDict options;
    GVariant *v;

    /* TODO Unfortunately StartNotify/StopNotify doesn't provide info about
     * peer device in call params so we need look this up ourselves.
     */
    if (aOptions == NULL)
    {
        GList * objects;
        GList * l;
        GList * ll;

        objects = g_dbus_object_manager_get_objects(sEndpoint->objMgr);
        for (l = objects; l != NULL; l = l->next)
        {
            BluezDevice1 *device = bluez_object_get_device1(BLUEZ_OBJECT(l->data));
            if (device != NULL)
            {
                if (BluezIsDeviceOnAdapter(device, sEndpoint->adapter))
                {
                    for (ll = objects; ll != NULL; ll = ll->next)
                    {
                        BluezGattService1 *service = bluez_object_get_gatt_service1(BLUEZ_OBJECT(ll->data));
                        if (service != NULL)
                        {
                            if (BluezIsServiceOnDevice(service, device))
                            {
                                if (BluezIsCharOnService(aChar, service))
                                {
                                    retval = (BluezConnection *)g_hash_table_lookup(sEndpoint->connMap,
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
        g_variant_dict_init(&options, aOptions);

        v = g_variant_dict_lookup_value(&options, "device", G_VARIANT_TYPE_OBJECT_PATH);

        VerifyOrExit(v != NULL,
                     ChipLogProgress(DeviceLayer, "FAIL: No device option in dictionary (%s)", __func__));

        path = g_variant_get_string(v, NULL);

        retval = (BluezConnection *)g_hash_table_lookup(sEndpoint->connMap, path);
    }

    exit:
    return retval;
}

void BluezObjectsCleanup(GDBusObjectManager *aManager)
{
    g_object_unref(sEndpoint->adapter);
}

static void BluezPeripheralObjectsSetup(void)
{

    static const char *const c1_flags[] = {"write", NULL};
    static const char *const c2_flags[] = {"read", "indicate", NULL};
    sEndpoint->service                  = BluezServiceCreate();
    // C1 characteristic
    sEndpoint->c1 =
            BluezCharacteristicCreate(sEndpoint->service, g_strdup("c1"), g_strdup(CHIP_PLAT_BLE_UUID_C1_STRING));
    bluez_gatt_characteristic1_set_flags(sEndpoint->c1, c1_flags);
    g_signal_connect(sEndpoint->c1, "handle-read-value", G_CALLBACK(BluezCharacteristicReadValue), NULL);
    g_signal_connect(sEndpoint->c1, "handle-write-value", G_CALLBACK(BluezCharacteristicWriteValue), NULL);
    g_signal_connect(sEndpoint->c1, "handle-acquire-write", G_CALLBACK(BluezCharacteristicAcquireWrite), NULL);
    g_signal_connect(sEndpoint->c1, "handle-acquire-notify", G_CALLBACK(BluezCharacteristicAcquireNotifyError), NULL);
    g_signal_connect(sEndpoint->c1, "handle-start-notify", G_CALLBACK(BluezCharacteristicStartNotifyError), NULL);
    g_signal_connect(sEndpoint->c1, "handle-stop-notify", G_CALLBACK(BluezCharacteristicStopNotifyError), NULL);

    sEndpoint->c2 =
            BluezCharacteristicCreate(sEndpoint->service, g_strdup("c2"), g_strdup(CHIP_PLAT_BLE_UUID_C2_STRING));
    bluez_gatt_characteristic1_set_flags(sEndpoint->c2, c2_flags);
    g_signal_connect(sEndpoint->c2, "handle-read-value", G_CALLBACK(BluezCharacteristicReadValue), NULL);
    g_signal_connect(sEndpoint->c2, "handle-write-value", G_CALLBACK(BluezCharacteristicWriteValueError), NULL);
    g_signal_connect(sEndpoint->c2, "handle-acquire-write", G_CALLBACK(BluezCharacteristicAcquireWriteError), NULL);
    g_signal_connect(sEndpoint->c2, "handle-acquire-notify", G_CALLBACK(BluezCharacteristicAcquireNotify), NULL);
    g_signal_connect(sEndpoint->c2, "handle-start-notify", G_CALLBACK(BluezCharacteristicStartNotify), NULL);
    g_signal_connect(sEndpoint->c2, "handle-stop-notify", G_CALLBACK(BluezCharacteristicStopNotify), NULL);
    g_signal_connect(sEndpoint->c2, "handle-confirm", G_CALLBACK(BluezCharacteristicConfirm), NULL);
#if 0
    VerifyOrExit(sEndpoint->adapter != NULL, ChipLogProgress(DeviceLayer, "FAIL: NULL sEndpoint->adapter in %s", __func__));

exit:
if (error != NULL)
    g_error_free(error);
return;
#endif

}

static void BluezOnBusAcquired(GDBusConnection *aConn, const gchar *aName, gpointer aClosure)
{
    ChipLogProgress(DeviceLayer, "TRACE: Bus acquired for name %s", aName);

    sEndpoint->rootPath = g_strdup_printf("/chipoble/%04x", getpid() & 0xffff);
    sEndpoint->root     = g_dbus_object_manager_server_new(sEndpoint->rootPath);
    g_dbus_object_manager_server_set_connection(sEndpoint->root, aConn);
    BluezPeripheralObjectsSetup();
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

    conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    VerifyOrExit(conn != NULL, ChipLogProgress(DeviceLayer, "FAIL: get bus sync in %s, error: %s", __func__, error->message));

    sEndpoint->owningName = g_strdup_printf("ble-%04x", getpid() & 0xffff);

    BluezOnBusAcquired(conn, sEndpoint->owningName, NULL);

    manager = g_dbus_object_manager_client_new_sync(
        conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ_INTERFACE, "/", bluez_object_manager_client_get_proxy_type,
        NULL /* unused user data in the Proxy Type Func */, NULL /*destroy notify */, NULL /* cancellable */, &error);

    //   manager = bluez_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
    //   G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
    //                                                       BLUEZ_INTERFACE, "/", NULL, /* GCancellable */
    //                                                       &error);

    VerifyOrExit(manager != NULL, ChipLogProgress(DeviceLayer, "FAIL: Error getting object manager client: %s", error->message));

    sEndpoint->objMgr = manager;

    bluezObjectsSetup(manager);

#if 0
    // reenable if we want to handle the bluetoothd restart
    g_signal_connect (manager,
                      "notify::name-owner",
                      G_CALLBACK (on_notify_name_owner),
                      NULL);
#endif
    g_signal_connect(manager, "object-added", G_CALLBACK(BluezSignalOnObjectAdded), NULL);
    g_signal_connect(manager, "object-removed", G_CALLBACK(BluezSignalOnObjectRemoved), NULL);
    g_signal_connect(manager, "interface-proxy-properties-changed", G_CALLBACK(BluezSignalInterfacePropertiesChanged),
                     NULL);

    // id    = g_bus_own_name(G_BUS_TYPE_SYSTEM, sEndpoint->owningName,
    //                      G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
    //                      BluezOnBusAcquired,
    //                  BluezOnNameAcquired, BluezOnNameLost, NULL, NULL);

    ChipLogProgress(DeviceLayer, "TRACE: Bluez mainloop starting %s", __func__);
    g_main_loop_run(sBluezMainLoop);
    ChipLogProgress(DeviceLayer, "TRACE: Bluez mainloop stopping %s", __func__);

    g_bus_unown_name(id);
    BluezObjectsCleanup(manager);
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

/**
 * Initialize BlueZ platform beyond the otPlatTobleInit arguments
 *
 * @param[in] aIsCentral  A boolean checking determining whether to configure the device in Central or peripheral mode
 * @param[in] aBleAddr A string corresponding to the BLE address of the HCI interface to bind to.  When NULL, the system
 * will bind to the first adapter that's found
 * @param[in] aBleName When non-null, adapter alias will be set to this string.
 *
 */
void PlatformBlueZInit(bool aIsCentral, char *aBleAddr, char *aBleName, uint32_t aNodeId)
{
    bool retval     = false;
    int  pthreadErr = 0;
    int  tmpErrno;

    VerifyOrExit(pipe2(sBluezFD, O_DIRECT) == 0, ChipLogProgress(DeviceLayer, "FAIL: open pipe in %s", __func__));

    // initialize server endpoint
    sEndpoint = g_new0(BluezServerEndpoint, 1);
    VerifyOrExit(sEndpoint != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory allocation in %s", __func__));

    if (aBleName != NULL)
        sEndpoint->adapterName = g_strdup(aBleName);
    else
        sEndpoint->adapterName = NULL;

    if (aBleAddr != NULL)
        sEndpoint->adapterAddr = g_strdup(aBleAddr);
    else
        sEndpoint->adapterAddr = NULL;

    sEndpoint->nodeId = aNodeId;

    sEndpoint->connMap   = g_hash_table_new(g_str_hash, g_str_equal);
    sEndpoint->mtu       = HCI_MAX_MTU;
    sEndpoint->isCentral = aIsCentral;

    gBluezBlePlatformDelegate->SetSendIndicationCallback(WoBLEz_ScheduleSendIndication);
    gBluezBlePlatformDelegate->SetGetMTUCallback(GetMTUWeaveCb);

    // peripheral properties

    VerifyOrExit(sEndpoint != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory alloc in %s", __func__));

    sBluezMainLoop = g_main_loop_new(NULL, FALSE);
    VerifyOrExit(sBluezMainLoop != NULL, ChipLogProgress(DeviceLayer, "FAIL: memory alloc in %s", __func__));

    pthreadErr = pthread_create(&sBluezThread, NULL, BluezMainLoop, NULL);
    tmpErrno   = errno;
    VerifyOrExit(pthreadErr == 0, ChipLogProgress(DeviceLayer, "FAIL: pthread_create (%s) in %s", strerror(tmpErrno), __func__));
    sleep(1);
    BluezRunOnBluezThread(BluezPeripheralRegisterApp, NULL);
    retval = TRUE;

    {
        ChipAdvConfig *closure;

        closure = g_new0(ChipAdvConfig, 1);
        closure->mType = BLUEZ_ADV_TYPE_UNDIRECTED_CONNECTABLE_SCANNABLE;
        closure->mInterval = 20;

        /**
         * Data arranged in "Length Type Value" pairs inside Weave service data.
         * Length should include size of value + size of Type field, which is 1 byte
         */

        sEndpoint->chipServiceData = (CHIPServiceData * )g_malloc(sizeof(CHIPServiceData) + 1);
        sEndpoint->advertisingUUID = g_strdup("0xFEAF");
        sEndpoint->chipServiceData->dataBlock0Len = sizeof(CHIPIdInfo) + 1;
        sEndpoint->chipServiceData->dataBlock0Type = 1;
        sEndpoint->chipServiceData->idInfo.major         = 1;
        sEndpoint->chipServiceData->idInfo.minor         = 1;
        sEndpoint->chipServiceData->idInfo.vendorId      = 1;
        sEndpoint->chipServiceData->idInfo.productId     = 1;
        sEndpoint->chipServiceData->idInfo.deviceId      = 1;
        sEndpoint->chipServiceData->idInfo.pairingStatus = 1;

        //closure->mData = (uint8_t *) chipServiceData;
        //closure->mLength = sizeof(CHIPServiceData);

        BluezRunOnBluezThread(BluezTobleAdvStart, closure);
    }

exit:
    if (retval)
    {
        ChipLogProgress(DeviceLayer, "PlatformBlueZInit init success");
    }
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

