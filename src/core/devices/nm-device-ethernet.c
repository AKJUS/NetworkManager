/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2005 - 2014 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-device-ethernet.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>
#include <libudev.h>
#include <linux/if_ether.h>

#include "NetworkManagerUtils.h"
#include "libnm-core-aux-intern/nm-libnm-core-utils.h"
#include "libnm-core-intern/nm-core-internal.h"
#include "libnm-glib-aux/nm-uuid.h"
#include "libnm-platform/nm-platform-utils.h"
#include "libnm-platform/nm-platform.h"
#include "libnm-udev-aux/nm-udev-utils.h"
#include "nm-act-request.h"
#include "nm-config.h"
#include "nm-dcb.h"
#include "nm-device-ethernet-utils.h"
#include "nm-device-factory.h"
#include "nm-device-private.h"
#include "nm-device-veth.h"
#include "nm-manager.h"
#include "ppp/nm-ppp-mgr.h"
#include "settings/nm-settings-connection.h"
#include "settings/nm-settings.h"
#include "supplicant/nm-supplicant-config.h"
#include "supplicant/nm-supplicant-interface.h"
#include "supplicant/nm-supplicant-manager.h"

#define _NMLOG_DEVICE_TYPE NMDeviceEthernet
#include "nm-device-logging.h"

/*****************************************************************************/

#define PPPOE_RECONNECT_DELAY_MSEC 7000
#define PPPOE_ENCAP_OVERHEAD       8 /* 2 bytes for PPP, 6 for PPPoE */

#define SUPPLICANT_LNK_TIMEOUT_SEC 15

/*****************************************************************************/

typedef enum {
    DCB_WAIT_UNKNOWN = 0,
    /* Ensure carrier is up before enabling DCB */
    DCB_WAIT_CARRIER_PREENABLE_UP,
    /* Wait for carrier down when device starts enabling */
    DCB_WAIT_CARRIER_PRECONFIG_DOWN,
    /* Wait for carrier up when device has finished enabling */
    DCB_WAIT_CARRIER_PRECONFIG_UP,
    /* Wait carrier down when device starts configuring */
    DCB_WAIT_CARRIER_POSTCONFIG_DOWN,
    /* Wait carrier up when device has finished configuring */
    DCB_WAIT_CARRIER_POSTCONFIG_UP,
} DcbWait;

typedef struct _NMDeviceEthernetPrivate {
    /* s390 */
    char       *subchan1;
    char       *subchan2;
    char       *subchan3;
    char       *subchannels;      /* Composite used for checking unmanaged specs */
    char      **subchannels_dbus; /* Array exported on D-Bus */
    char       *s390_nettype;
    GHashTable *s390_options;

    guint32 speed;
    gulong  carrier_id;

    struct {
        NMSupplicantManager         *mgr;
        NMSupplMgrCreateIfaceHandle *create_handle;
        NMSupplicantInterface       *iface;

        gulong iface_state_id;
        gulong auth_state_id;

        guint con_timeout_id;

        guint lnk_timeout_id;

        bool is_associated : 1;
        bool ready : 1;
    } supplicant;

    NMActRequestGetSecretsCallId *wired_secrets_id;

    struct {
        NMPppMgr *ppp_mgr;
        GSource  *wait_source;
        gint64    last_pppoe_time_msec;
    } ppp_data;

    /* DCB */
    DcbWait dcb_wait;
    guint   dcb_timeout_id;

    guint32 ethtool_prev_speed;

    NMPlatformLinkDuplexType ethtool_prev_duplex : 3;

    bool dcb_handle_carrier_changes : 1;

    bool ethtool_prev_set : 1;
    bool ethtool_prev_autoneg : 1;

    bool stage2_ready_dcb : 1;

} NMDeviceEthernetPrivate;

NM_GOBJECT_PROPERTIES_DEFINE(NMDeviceEthernet, PROP_SPEED, PROP_S390_SUBCHANNELS, );

/*****************************************************************************/

G_DEFINE_TYPE(NMDeviceEthernet, nm_device_ethernet, NM_TYPE_DEVICE)

#define NM_DEVICE_ETHERNET_GET_PRIVATE(self) \
    _NM_GET_PRIVATE_PTR(self, NMDeviceEthernet, NM_IS_DEVICE_ETHERNET, NMDevice)

/*****************************************************************************/

static void wired_secrets_cancel(NMDeviceEthernet *self);

/*****************************************************************************/

static char *
get_link_basename(const char *parent_path, const char *name, GError **error)
{
    char *link_dest, *path;
    char *result = NULL;

    path      = g_strdup_printf("%s/%s", parent_path, name);
    link_dest = g_file_read_link(path, error);
    if (link_dest) {
        result = g_path_get_basename(link_dest);
        g_free(link_dest);
    }
    g_free(path);
    return result;
}

static void
_update_s390_subchannels(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    struct udev_device      *dev    = NULL;
    struct udev_device      *parent = NULL;
    const char              *parent_path, *item;
    int                      ifindex;
    GDir                    *dir;
    GError                  *error = NULL;

    if (priv->subchannels) {
        /* only read the subchannels once. For one, we don't expect them to change
         * on multiple invocations. Second, we didn't implement proper reloading.
         * Proper reloading might also be complicated, because the subchannels are
         * used to match on devices based on a device-spec. Thus, it's not clear
         * what it means to change afterwards. */
        return;
    }

    ifindex = nm_device_get_ifindex((NMDevice *) self);
    dev     = nm_platform_link_get_udev_device(nm_device_get_platform(NM_DEVICE(self)), ifindex);
    if (!dev)
        return;

    /* Try for the "ccwgroup" parent */
    parent = udev_device_get_parent_with_subsystem_devtype(dev, "ccwgroup", NULL);
    if (!parent) {
        /* FIXME: whatever 'lcs' devices' subsystem is here... */

        /* Not an s390 device */
        return;
    }

    parent_path = udev_device_get_syspath(parent);
    dir         = g_dir_open(parent_path, 0, &error);
    if (!dir) {
        _LOGW(LOGD_DEVICE | LOGD_PLATFORM,
              "update-s390: failed to open directory '%s': %s",
              parent_path,
              error->message);
        g_clear_error(&error);
        return;
    }

    while ((item = g_dir_read_name(dir))) {
        if (!strcmp(item, "cdev0")) {
            priv->subchan1 = get_link_basename(parent_path, "cdev0", &error);
        } else if (!strcmp(item, "cdev1")) {
            priv->subchan2 = get_link_basename(parent_path, "cdev1", &error);
        } else if (!strcmp(item, "cdev2")) {
            priv->subchan3 = get_link_basename(parent_path, "cdev2", &error);
        } else if (!strcmp(item, "driver")) {
            priv->s390_nettype = get_link_basename(parent_path, "driver", &error);
        } else if (!strcmp(item, "layer2") || !strcmp(item, "portname")
                   || !strcmp(item, "portno")) {
            gs_free char *path = NULL, *value = NULL;

            path  = g_strdup_printf("%s/%s", parent_path, item);
            value = nm_platform_sysctl_get(nm_device_get_platform(NM_DEVICE(self)),
                                           NMP_SYSCTL_PATHID_ABSOLUTE(path));

            if (!strcmp(item, "portname") && !g_strcmp0(value, "no portname required")) {
                /* Do nothing */
            } else if (value && *value) {
                g_hash_table_insert(priv->s390_options, g_strdup(item), value);
                value = NULL;
            } else
                _LOGW(LOGD_DEVICE | LOGD_PLATFORM, "update-s390: error reading %s", path);
        }

        if (error) {
            _LOGW(LOGD_DEVICE | LOGD_PLATFORM,
                  "update-s390: failed reading sysfs for %s (%s)",
                  item,
                  error->message);
            g_clear_error(&error);
        }
    }

    g_dir_close(dir);

    if (priv->subchan3) {
        priv->subchannels =
            g_strdup_printf("%s,%s,%s", priv->subchan1, priv->subchan2, priv->subchan3);
    } else if (priv->subchan2) {
        priv->subchannels = g_strdup_printf("%s,%s", priv->subchan1, priv->subchan2);
    } else
        priv->subchannels = g_strdup(priv->subchan1);

    priv->subchannels_dbus    = g_new(char *, 3 + 1);
    priv->subchannels_dbus[0] = g_strdup(priv->subchan1);
    priv->subchannels_dbus[1] = g_strdup(priv->subchan2);
    priv->subchannels_dbus[2] = g_strdup(priv->subchan3);
    priv->subchannels_dbus[3] = NULL;

    _LOGI(LOGD_DEVICE | LOGD_PLATFORM,
          "update-s390: found s390 '%s' subchannels [%s]",
          nm_device_get_driver((NMDevice *) self) ?: "(unknown driver)",
          priv->subchannels);

    _notify(self, PROP_S390_SUBCHANNELS);
}

static void
device_state_changed(NMDevice           *device,
                     NMDeviceState       new_state,
                     NMDeviceState       old_state,
                     NMDeviceStateReason reason)
{
    if (new_state > NM_DEVICE_STATE_ACTIVATED)
        wired_secrets_cancel(NM_DEVICE_ETHERNET(device));
}

static void
nm_device_ethernet_init(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE(self, NM_TYPE_DEVICE_ETHERNET, NMDeviceEthernetPrivate);
    self->_priv = priv;

    priv->s390_options = g_hash_table_new_full(nm_str_hash, g_str_equal, g_free, g_free);
}

static NMDeviceCapabilities
get_generic_capabilities(NMDevice *device)
{
    NMDeviceEthernet *self    = NM_DEVICE_ETHERNET(device);
    int               ifindex = nm_device_get_ifindex(device);

    if (ifindex > 0) {
        if (nm_platform_link_supports_carrier_detect(nm_device_get_platform(device), ifindex))
            return NM_DEVICE_CAP_CARRIER_DETECT;
        else {
            _LOGI(LOGD_PLATFORM,
                  "driver '%s' does not support carrier detection.",
                  nm_device_get_driver(device));
        }
    }

    return NM_DEVICE_CAP_NONE;
}

static guint32
_subchannels_count_num(const char *const *array)
{
    int i;

    if (!array)
        return 0;
    for (i = 0; array[i]; i++)
        /* NOP */;
    return i;
}

static gboolean
match_subchans(NMDeviceEthernet *self, NMSettingWired *s_wired, gboolean *try_mac)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    const char *const       *subchans;
    guint32                  num1, num2;
    int                      i;

    *try_mac = TRUE;

    subchans = nm_setting_wired_get_s390_subchannels(s_wired);
    num1     = _subchannels_count_num(subchans);
    num2     = _subchannels_count_num((const char *const *) priv->subchannels_dbus);
    /* connection has no subchannels */
    if (num1 == 0)
        return TRUE;
    /* connection requires subchannels but the device has none */
    if (num2 == 0)
        return FALSE;
    /* number of subchannels differ */
    if (num1 != num2)
        return FALSE;

    /* Make sure each subchannel in the connection is a subchannel of this device */
    for (i = 0; subchans[i]; i++) {
        const char *candidate = subchans[i];

        if ((priv->subchan1 && !strcmp(priv->subchan1, candidate))
            || (priv->subchan2 && !strcmp(priv->subchan2, candidate))
            || (priv->subchan3 && !strcmp(priv->subchan3, candidate)))
            continue;

        return FALSE; /* a subchannel was not found */
    }

    *try_mac = FALSE;
    return TRUE;
}

static gboolean
check_connection_compatible(NMDevice     *device,
                            NMConnection *connection,
                            gboolean      check_properties,
                            GError      **error)
{
    NMDeviceEthernet *self = NM_DEVICE_ETHERNET(device);
    NMSettingWired   *s_wired;

    if (!NM_DEVICE_CLASS(nm_device_ethernet_parent_class)
             ->check_connection_compatible(device, connection, check_properties, error))
        return FALSE;

    if (nm_connection_is_type(connection, NM_SETTING_PPPOE_SETTING_NAME)
        || (nm_connection_is_type(connection, NM_SETTING_VETH_SETTING_NAME)
            && NM_IS_DEVICE_VETH(device))) {
        s_wired = nm_connection_get_setting_wired(connection);
    } else {
        s_wired =
            _nm_connection_check_main_setting(connection, NM_SETTING_WIRED_SETTING_NAME, error);
        if (!s_wired)
            return FALSE;
    }

    if (s_wired) {
        const char        *mac, *perm_hw_addr;
        gboolean           try_mac = TRUE;
        const char *const *mac_denylist;
        int                i;

        if (!match_subchans(self, s_wired, &try_mac)) {
            nm_utils_error_set_literal(error,
                                       NM_UTILS_ERROR_CONNECTION_AVAILABLE_TEMPORARY,
                                       "s390 subchannels don't match");
            return FALSE;
        }

        perm_hw_addr = nm_device_get_permanent_hw_address(device);
        mac          = nm_setting_wired_get_mac_address(s_wired);
        if (perm_hw_addr) {
            if (try_mac && mac && !nm_utils_hwaddr_matches(mac, -1, perm_hw_addr, -1)) {
                nm_utils_error_set_literal(error,
                                           NM_UTILS_ERROR_CONNECTION_AVAILABLE_TEMPORARY,
                                           "permanent MAC address doesn't match");
                return FALSE;
            }

            /* Check for MAC address denylist */
            mac_denylist = nm_setting_wired_get_mac_address_denylist(s_wired);
            for (i = 0; mac_denylist[i]; i++) {
                if (!nm_utils_hwaddr_valid(mac_denylist[i], ETH_ALEN)) {
                    nm_utils_error_set_literal(error,
                                               NM_UTILS_ERROR_CONNECTION_AVAILABLE_TEMPORARY,
                                               "invalid MAC in blacklist");
                    return FALSE;
                }

                if (nm_utils_hwaddr_matches(mac_denylist[i], -1, perm_hw_addr, -1)) {
                    nm_utils_error_set_literal(error,
                                               NM_UTILS_ERROR_CONNECTION_AVAILABLE_TEMPORARY,
                                               "permanent MAC address of device blacklisted");
                    return FALSE;
                }
            }
        } else if (mac) {
            nm_utils_error_set_literal(error,
                                       NM_UTILS_ERROR_CONNECTION_AVAILABLE_TEMPORARY,
                                       "device has no permanent MAC address to match");
            return FALSE;
        }
    }

    return TRUE;
}

/*****************************************************************************/
/* 802.1X */

static void
supplicant_interface_release(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    nm_clear_pointer(&priv->supplicant.create_handle,
                     nm_supplicant_manager_create_interface_cancel);

    nm_clear_g_source(&priv->supplicant.lnk_timeout_id);
    nm_clear_g_source(&priv->supplicant.con_timeout_id);
    nm_clear_g_signal_handler(priv->supplicant.iface, &priv->supplicant.iface_state_id);
    nm_clear_g_signal_handler(priv->supplicant.iface, &priv->supplicant.auth_state_id);
    priv->supplicant.ready = FALSE;

    if (priv->supplicant.iface) {
        nm_supplicant_interface_disconnect(priv->supplicant.iface);
        g_clear_object(&priv->supplicant.iface);
    }
}

static void
supplicant_auth_state_changed(NMSupplicantInterface *iface,
                              GParamSpec            *pspec,
                              NMDeviceEthernet      *self)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMSupplicantAuthState    state;

    state = nm_supplicant_interface_get_auth_state(priv->supplicant.iface);
    _LOGD(LOGD_CORE, "supplicant auth state changed to %u", (unsigned) state);

    if (state == NM_SUPPLICANT_AUTH_STATE_SUCCESS) {
        nm_clear_g_signal_handler(priv->supplicant.iface, &priv->supplicant.iface_state_id);
        nm_device_update_dynamic_ip_setup(NM_DEVICE(self), "supplicant auth state changed");
    }
}

static gboolean
wired_auth_is_optional(NMDeviceEthernet *self)
{
    NMSetting8021x *s_8021x;

    s_8021x = nm_device_get_applied_setting(NM_DEVICE(self), NM_TYPE_SETTING_802_1X);
    g_return_val_if_fail(s_8021x, FALSE);
    return nm_setting_802_1x_get_optional(s_8021x);
}

static void
wired_auth_cond_fail(NMDeviceEthernet *self, NMDeviceStateReason reason)
{
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMDevice                *device = NM_DEVICE(self);

    if (!wired_auth_is_optional(self)) {
        supplicant_interface_release(self);
        nm_device_state_changed(NM_DEVICE(self), NM_DEVICE_STATE_FAILED, reason);
        return;
    }

    _LOGI(LOGD_DEVICE | LOGD_ETHER,
          "Activation: (ethernet) 802.1X authentication is optional, continuing after a failure");
    priv->supplicant.ready = TRUE;

    if (NM_IN_SET(nm_device_get_state(device), NM_DEVICE_STATE_CONFIG, NM_DEVICE_STATE_NEED_AUTH))
        nm_device_activate_schedule_stage2_device_config(device, FALSE);

    if (!priv->supplicant.auth_state_id) {
        priv->supplicant.auth_state_id =
            g_signal_connect(priv->supplicant.iface,
                             "notify::" NM_SUPPLICANT_INTERFACE_AUTH_STATE,
                             G_CALLBACK(supplicant_auth_state_changed),
                             self);
    }
}

static void
wired_secrets_cb(NMActRequest                 *req,
                 NMActRequestGetSecretsCallId *call_id,
                 NMSettingsConnection         *connection,
                 GError                       *error,
                 gpointer                      user_data)
{
    NMDeviceEthernet        *self   = user_data;
    NMDevice                *device = user_data;
    NMDeviceEthernetPrivate *priv;

    g_return_if_fail(NM_IS_DEVICE_ETHERNET(self));
    g_return_if_fail(NM_IS_ACT_REQUEST(req));

    priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    g_return_if_fail(priv->wired_secrets_id == call_id);

    priv->wired_secrets_id = NULL;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    g_return_if_fail(req == nm_device_get_act_request(device));
    g_return_if_fail(nm_device_get_state(device) == NM_DEVICE_STATE_NEED_AUTH);
    g_return_if_fail(nm_act_request_get_settings_connection(req) == connection);

    if (error) {
        _LOGW(LOGD_ETHER, "%s", error->message);
        wired_auth_cond_fail(self, NM_DEVICE_STATE_REASON_NO_SECRETS);
        return;
    }

    supplicant_interface_release(self);
    nm_device_activate_schedule_stage1_device_prepare(device, FALSE);
}

static void
wired_secrets_cancel(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    if (priv->wired_secrets_id)
        nm_act_request_cancel_secrets(NULL, priv->wired_secrets_id);
    nm_assert(!priv->wired_secrets_id);
}

static void
wired_secrets_get_secrets(NMDeviceEthernet            *self,
                          const char                  *setting_name,
                          NMSecretAgentGetSecretsFlags flags)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMActRequest            *req;

    wired_secrets_cancel(self);

    req = nm_device_get_act_request(NM_DEVICE(self));
    g_return_if_fail(NM_IS_ACT_REQUEST(req));

    priv->wired_secrets_id =
        nm_act_request_get_secrets(req, TRUE, setting_name, flags, NULL, wired_secrets_cb, self);
    g_return_if_fail(priv->wired_secrets_id);
}

static gboolean
supplicant_lnk_timeout_cb(gpointer user_data)
{
    NMDeviceEthernet        *self   = NM_DEVICE_ETHERNET(user_data);
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMDevice                *device = NM_DEVICE(self);
    NMActRequest            *req;
    NMConnection            *applied_connection;
    const char              *setting_name;

    priv->supplicant.lnk_timeout_id = 0;

    req = nm_device_get_act_request(device);

    if (nm_device_get_state(device) == NM_DEVICE_STATE_ACTIVATED) {
        wired_auth_cond_fail(self, NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT);
        return G_SOURCE_REMOVE;
    }

    /* Disconnect event during initial authentication and credentials
     * ARE checked - we are likely to have wrong key.  Ask the user for
     * another one.
     */
    if (nm_device_get_state(device) != NM_DEVICE_STATE_CONFIG)
        goto time_out;

    nm_active_connection_clear_secrets(NM_ACTIVE_CONNECTION(req));

    applied_connection = nm_act_request_get_applied_connection(req);
    setting_name       = nm_connection_need_secrets(applied_connection, NULL);
    if (!setting_name)
        goto time_out;

    _LOGI(LOGD_DEVICE | LOGD_ETHER,
          "Activation: (ethernet) disconnected during authentication, asking for new key.");
    if (!wired_auth_is_optional(self))
        supplicant_interface_release(self);

    nm_device_state_changed(device,
                            NM_DEVICE_STATE_NEED_AUTH,
                            NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);
    wired_secrets_get_secrets(self, setting_name, NM_SECRET_AGENT_GET_SECRETS_FLAG_REQUEST_NEW);

    return G_SOURCE_REMOVE;

time_out:
    _LOGW(LOGD_DEVICE | LOGD_ETHER, "link timed out.");
    wired_auth_cond_fail(self, NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT);

    return G_SOURCE_REMOVE;
}

static NMSupplicantConfig *
build_supplicant_config(NMDeviceEthernet *self, GError **error)
{
    const char         *con_uuid;
    NMSupplicantConfig *config = NULL;
    NMSetting8021x     *security;
    NMConnection       *connection;
    guint32             mtu;

    connection = nm_device_get_applied_connection(NM_DEVICE(self));

    g_return_val_if_fail(connection, NULL);

    con_uuid = nm_connection_get_uuid(connection);
    mtu      = nm_platform_link_get_mtu(nm_device_get_platform(NM_DEVICE(self)),
                                   nm_device_get_ifindex(NM_DEVICE(self)));

    config = nm_supplicant_config_new(NM_SUPPL_CAP_MASK_NONE);

    security = nm_connection_get_setting_802_1x(connection);
    if (!nm_supplicant_config_add_setting_8021x(config, security, con_uuid, mtu, TRUE, error)) {
        g_prefix_error(error, "802-1x-setting: ");
        g_clear_object(&config);
    }

    return config;
}

static void
supplicant_iface_state_is_completed(NMDeviceEthernet *self, NMSupplicantInterfaceState state)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    if (state == NM_SUPPLICANT_INTERFACE_STATE_COMPLETED) {
        nm_clear_g_source(&priv->supplicant.lnk_timeout_id);
        nm_clear_g_source(&priv->supplicant.con_timeout_id);
        priv->supplicant.ready = TRUE;

        /* If this is the initial association during device activation,
         * schedule the activation stage again to proceed.
         */
        if (nm_device_get_state(NM_DEVICE(self)) == NM_DEVICE_STATE_CONFIG) {
            _LOGI(LOGD_DEVICE | LOGD_ETHER,
                  "Activation: (ethernet) Stage 2 of 5 (Device Configure) successful.");
            nm_device_activate_schedule_stage2_device_config(NM_DEVICE(self), FALSE);
        }
        return;
    }

    if (!priv->supplicant.lnk_timeout_id && !priv->supplicant.con_timeout_id)
        priv->supplicant.lnk_timeout_id =
            g_timeout_add_seconds(SUPPLICANT_LNK_TIMEOUT_SEC, supplicant_lnk_timeout_cb, self);
}

static void
supplicant_iface_assoc_cb(NMSupplicantInterface *iface, GError *error, gpointer user_data)
{
    NMDeviceEthernet        *self;
    NMDeviceEthernetPrivate *priv;

    if (nm_utils_error_is_cancelled_or_disposing(error))
        return;

    self = NM_DEVICE_ETHERNET(user_data);
    priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    if (error) {
        supplicant_interface_release(self);
        nm_device_queue_state(NM_DEVICE(self),
                              NM_DEVICE_STATE_FAILED,
                              NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED);
        return;
    }

    nm_assert(!priv->supplicant.lnk_timeout_id);
    nm_assert(!priv->supplicant.is_associated);

    priv->supplicant.is_associated = TRUE;
    supplicant_iface_state_is_completed(self,
                                        nm_supplicant_interface_get_state(priv->supplicant.iface));
}

static gboolean
supplicant_iface_start(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate            *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    gs_unref_object NMSupplicantConfig *config = NULL;
    gs_free_error GError               *error  = NULL;

    config = build_supplicant_config(self, &error);
    if (!config) {
        _LOGE(LOGD_DEVICE | LOGD_ETHER,
              "Activation: (ethernet) couldn't build security configuration: %s",
              error->message);
        supplicant_interface_release(self);
        nm_device_state_changed(NM_DEVICE(self),
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED);
        return FALSE;
    }

    nm_supplicant_interface_disconnect(priv->supplicant.iface);
    nm_supplicant_interface_assoc(priv->supplicant.iface, config, supplicant_iface_assoc_cb, self);
    return TRUE;
}

static void
supplicant_iface_state_cb(NMSupplicantInterface *iface,
                          int                    new_state_i,
                          int                    old_state_i,
                          int                    disconnect_reason,
                          gpointer               user_data)
{
    NMDeviceEthernet          *self      = NM_DEVICE_ETHERNET(user_data);
    NMDeviceEthernetPrivate   *priv      = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMSupplicantInterfaceState new_state = new_state_i;
    NMSupplicantInterfaceState old_state = old_state_i;

    _LOGI(LOGD_DEVICE | LOGD_ETHER,
          "supplicant interface state: %s -> %s",
          nm_supplicant_interface_state_to_string(old_state),
          nm_supplicant_interface_state_to_string(new_state));

    if (new_state == NM_SUPPLICANT_INTERFACE_STATE_DOWN) {
        supplicant_interface_release(self);
        nm_device_state_changed(NM_DEVICE(self),
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return;
    }

    if (old_state == NM_SUPPLICANT_INTERFACE_STATE_STARTING) {
        if (!supplicant_iface_start(self))
            return;
    }

    if (priv->supplicant.is_associated)
        supplicant_iface_state_is_completed(self, new_state);
}

static gboolean
handle_auth_or_fail(NMDeviceEthernet *self, NMActRequest *req, gboolean new_secrets)
{
    const char   *setting_name;
    NMConnection *applied_connection;

    if (!nm_device_auth_retries_try_next(NM_DEVICE(self)))
        return FALSE;

    nm_device_state_changed(NM_DEVICE(self),
                            NM_DEVICE_STATE_NEED_AUTH,
                            NM_DEVICE_STATE_REASON_NONE);

    nm_active_connection_clear_secrets(NM_ACTIVE_CONNECTION(req));

    applied_connection = nm_act_request_get_applied_connection(req);
    setting_name       = nm_connection_need_secrets(applied_connection, NULL);
    if (!setting_name) {
        _LOGI(LOGD_DEVICE, "Cleared secrets, but setting didn't need any secrets.");
        return FALSE;
    }

    _LOGI(LOGD_DEVICE | LOGD_ETHER, "Activation: (ethernet) asking for new secrets");

    /* Don't tear down supplicant if the authentication is optional
     * because in case of a failure in getting new secrets we want to
     * keep the supplicant alive.
     */
    if (!wired_auth_is_optional(self))
        supplicant_interface_release(self);

    wired_secrets_get_secrets(
        self,
        setting_name,
        NM_SECRET_AGENT_GET_SECRETS_FLAG_ALLOW_INTERACTION
            | (new_secrets ? NM_SECRET_AGENT_GET_SECRETS_FLAG_REQUEST_NEW : 0));
    return TRUE;
}

static gboolean
supplicant_connection_timeout_cb(gpointer user_data)
{
    NMDeviceEthernet        *self   = NM_DEVICE_ETHERNET(user_data);
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMDevice                *device = NM_DEVICE(self);
    NMActRequest            *req;
    NMSettingsConnection    *connection;
    guint64                  timestamp   = 0;
    gboolean                 new_secrets = TRUE;

    priv->supplicant.con_timeout_id = 0;

    /* Authentication failed; either driver problems, the encryption key is
     * wrong, the passwords or certificates were wrong or the Ethernet switch's
     * port is not configured for 802.1x. */
    _LOGW(LOGD_DEVICE | LOGD_ETHER, "Activation: (ethernet) association took too long.");

    req        = nm_device_get_act_request(device);
    connection = nm_act_request_get_settings_connection(req);

    /* Ask for new secrets only if we've never activated this connection
     * before.  If we've connected before, don't bother the user with dialogs,
     * just retry or fail, and if we never connect the user can fix the
     * password somewhere else. */
    if (nm_settings_connection_get_timestamp(connection, &timestamp))
        new_secrets = !timestamp;

    if (!handle_auth_or_fail(self, req, new_secrets)) {
        wired_auth_cond_fail(self, NM_DEVICE_STATE_REASON_NO_SECRETS);
        return G_SOURCE_REMOVE;
    }

    if (!priv->supplicant.lnk_timeout_id && priv->supplicant.iface) {
        NMSupplicantInterfaceState state;

        state = nm_supplicant_interface_get_state(priv->supplicant.iface);
        if (state != NM_SUPPLICANT_INTERFACE_STATE_COMPLETED
            && nm_supplicant_interface_state_is_operational(state))
            priv->supplicant.lnk_timeout_id =
                g_timeout_add_seconds(SUPPLICANT_LNK_TIMEOUT_SEC, supplicant_lnk_timeout_cb, self);
    }

    return G_SOURCE_REMOVE;
}

static void
supplicant_interface_create_cb(NMSupplicantManager         *supplicant_manager,
                               NMSupplMgrCreateIfaceHandle *handle,
                               NMSupplicantInterface       *iface,
                               GError                      *error,
                               gpointer                     user_data)
{
    NMDeviceEthernet        *self;
    NMDeviceEthernetPrivate *priv;
    guint                    timeout;

    if (nm_utils_error_is_cancelled(error))
        return;

    self = user_data;
    priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    nm_assert(priv->supplicant.create_handle == handle);
    priv->supplicant.create_handle = NULL;

    if (error) {
        _LOGE(LOGD_DEVICE | LOGD_ETHER,
              "Couldn't initialize supplicant interface: %s",
              error->message);
        supplicant_interface_release(self);
        nm_device_state_changed(NM_DEVICE(self),
                                NM_DEVICE_STATE_FAILED,
                                NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED);
        return;
    }

    priv->supplicant.iface         = g_object_ref(iface);
    priv->supplicant.is_associated = FALSE;

    priv->supplicant.iface_state_id = g_signal_connect(priv->supplicant.iface,
                                                       NM_SUPPLICANT_INTERFACE_STATE,
                                                       G_CALLBACK(supplicant_iface_state_cb),
                                                       self);

    timeout = nm_device_get_supplicant_timeout(NM_DEVICE(self));
    priv->supplicant.con_timeout_id =
        g_timeout_add_seconds(timeout, supplicant_connection_timeout_cb, self);

    if (nm_supplicant_interface_state_is_operational(nm_supplicant_interface_get_state(iface)))
        supplicant_iface_start(self);
}

static NMPlatformLinkDuplexType
link_duplex_to_platform(const char *duplex)
{
    if (!duplex)
        return NM_PLATFORM_LINK_DUPLEX_UNKNOWN;
    if (nm_streq(duplex, "full"))
        return NM_PLATFORM_LINK_DUPLEX_FULL;
    if (nm_streq(duplex, "half"))
        return NM_PLATFORM_LINK_DUPLEX_HALF;
    g_return_val_if_reached(NM_PLATFORM_LINK_DUPLEX_UNKNOWN);
}

static void
link_negotiation_set(NMDevice *device)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMSettingWired          *s_wired;
    gboolean                 autoneg = TRUE;
    gboolean                 link_autoneg;
    NMPlatformLinkDuplexType duplex      = NM_PLATFORM_LINK_DUPLEX_UNKNOWN;
    NMPlatformLinkDuplexType link_duplex = NM_PLATFORM_LINK_DUPLEX_UNKNOWN;
    guint32                  speed       = 0;
    guint32                  link_speed;

    s_wired = nm_device_get_applied_setting(device, NM_TYPE_SETTING_WIRED);
    if (s_wired) {
        autoneg = nm_setting_wired_get_auto_negotiate(s_wired);
        speed   = nm_setting_wired_get_speed(s_wired);
        duplex  = link_duplex_to_platform(nm_setting_wired_get_duplex(s_wired));
        if (!autoneg && !speed && !duplex) {
            _LOGD(LOGD_DEVICE, "set-link: ignore link negotiation");
            return;
        }
    }

    if (!nm_platform_ethtool_get_link_settings(nm_device_get_platform(device),
                                               nm_device_get_ifindex(device),
                                               &link_autoneg,
                                               &link_speed,
                                               &link_duplex)) {
        _LOGW(LOGD_DEVICE, "set-link: unable to retrieve link negotiation");
        return;
    }

    if (autoneg && !speed && !duplex)
        _LOGD(LOGD_DEVICE, "set-link: configure auto-negotiation");
    else {
        _LOGD(LOGD_DEVICE,
              "set-link: configure %snegotiation (%u Mbit, %s duplex)",
              autoneg ? "auto-" : "static ",
              speed,
              nm_platform_link_duplex_type_to_string(duplex));
    }

    if (!priv->ethtool_prev_set) {
        /* remember the values we had before setting it. */
        priv->ethtool_prev_autoneg = link_autoneg;
        if (link_autoneg) {
            /* with autoneg, we only support advertising one speed/duplex. Likewise
             * our nm_platform_ethtool_get_link_settings() can only return the current
             * speed/duplex, but not all the modes that we were advertising.
             *
             * Do the best we can do: remember to re-enable autoneg, but don't restrict
             * the mode. */
            priv->ethtool_prev_speed  = 0;
            priv->ethtool_prev_duplex = NM_PLATFORM_LINK_DUPLEX_UNKNOWN;
        } else {
            priv->ethtool_prev_speed  = link_speed;
            priv->ethtool_prev_duplex = link_duplex;
        }
        priv->ethtool_prev_set = TRUE;
    }

    if (!nm_platform_ethtool_set_link_settings(nm_device_get_platform(device),
                                               nm_device_get_ifindex(device),
                                               autoneg,
                                               speed,
                                               duplex)) {
        _LOGW(LOGD_DEVICE, "set-link: failure to set link negotiation");
        return;
    }
}

static gboolean
pppoe_reconnect_delay(gpointer user_data)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(user_data);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    nm_clear_g_source_inst(&priv->ppp_data.wait_source);
    priv->ppp_data.last_pppoe_time_msec = 0;
    _LOGI(LOGD_DEVICE, "PPPoE reconnect delay complete, resuming connection...");
    nm_device_activate_schedule_stage1_device_prepare(NM_DEVICE(self), FALSE);
    return G_SOURCE_CONTINUE;
}

static NMActStageReturn
act_stage1_prepare(NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    if (nm_device_managed_type_is_external_or_assume(device)) {
        if (!priv->ethtool_prev_set && !nm_device_managed_type_is_external(device)) {
            NMSettingWired *s_wired;

            /* During restart of NetworkManager service we forget the original auto
             * negotiation settings. When taking over a device, remember to reset
             * the "default" during deactivate. */
            s_wired = nm_device_get_applied_setting(device, NM_TYPE_SETTING_WIRED);
            if (s_wired
                && (nm_setting_wired_get_auto_negotiate(s_wired)
                    || nm_setting_wired_get_speed(s_wired)
                    || nm_setting_wired_get_duplex(s_wired))) {
                priv->ethtool_prev_set     = TRUE;
                priv->ethtool_prev_autoneg = TRUE;
                priv->ethtool_prev_speed   = 0;
                priv->ethtool_prev_duplex  = NM_PLATFORM_LINK_DUPLEX_UNKNOWN;
            }
        }
        return NM_ACT_STAGE_RETURN_SUCCESS;
    }

    link_negotiation_set(device);

    /* If we're re-activating a PPPoE connection a short while after
     * a previous PPPoE connection was torn down, wait a bit to allow the
     * remote side to handle the disconnection.  Otherwise, the peer may
     * get confused and fail to negotiate the new connection. (rh #1023503)
     *
     * FIXME(shutdown): when exiting, we also need to wait before quitting,
     * at least for additional NM_SHUTDOWN_TIMEOUT_MAX_MSEC seconds because
     * otherwise after restart the device won't work for the first seconds.
     */
    if (priv->ppp_data.last_pppoe_time_msec != 0) {
        gint64 delay =
            nm_utils_get_monotonic_timestamp_msec() - priv->ppp_data.last_pppoe_time_msec;

        if (delay < PPPOE_RECONNECT_DELAY_MSEC
            && nm_device_get_applied_setting(device, NM_TYPE_SETTING_PPPOE)) {
            if (!priv->ppp_data.wait_source) {
                _LOGI(LOGD_DEVICE,
                      "delaying PPPoE reconnect for %d.%03d seconds to ensure peer is ready...",
                      (int) (delay / 1000),
                      (int) (delay % 1000));
                priv->ppp_data.wait_source =
                    nm_g_timeout_add_source(delay, pppoe_reconnect_delay, self);
            }
            return NM_ACT_STAGE_RETURN_POSTPONE;
        }
        nm_clear_g_source_inst(&priv->ppp_data.wait_source);
        priv->ppp_data.last_pppoe_time_msec = 0;
    }

    return NM_ACT_STAGE_RETURN_SUCCESS;
}

static NMActStageReturn
supplicant_check_secrets_needed(NMDeviceEthernet *self, NMDeviceStateReason *out_failure_reason)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMConnection            *connection;
    NMSetting8021x          *security;
    const char              *setting_name;

    connection = nm_device_get_applied_connection(NM_DEVICE(self));
    g_return_val_if_fail(connection, NM_ACT_STAGE_RETURN_FAILURE);

    security = nm_connection_get_setting_802_1x(connection);
    if (!security) {
        _LOGE(LOGD_DEVICE, "Invalid or missing 802.1X security");
        NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_CONFIG_FAILED);
        return NM_ACT_STAGE_RETURN_FAILURE;
    }

    if (!priv->supplicant.mgr)
        priv->supplicant.mgr = g_object_ref(nm_supplicant_manager_get());

    /* If we need secrets, get them */
    setting_name = nm_connection_need_secrets(connection, NULL);
    if (setting_name) {
        NMActRequest *req = nm_device_get_act_request(NM_DEVICE(self));

        _LOGI(LOGD_DEVICE | LOGD_ETHER,
              "Activation: (ethernet) connection '%s' has security, but secrets are required.",
              nm_connection_get_id(connection));

        if (!handle_auth_or_fail(self, req, FALSE)) {
            NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_NO_SECRETS);
            return NM_ACT_STAGE_RETURN_FAILURE;
        }
        return NM_ACT_STAGE_RETURN_POSTPONE;
    }

    _LOGI(LOGD_DEVICE | LOGD_ETHER,
          "Activation: (ethernet) connection '%s' requires no security. No secrets needed.",
          nm_connection_get_id(connection));

    supplicant_interface_release(self);

    priv->supplicant.create_handle =
        nm_supplicant_manager_create_interface(priv->supplicant.mgr,
                                               nm_device_get_ifindex(NM_DEVICE(self)),
                                               NM_SUPPLICANT_DRIVER_WIRED,
                                               supplicant_interface_create_cb,
                                               self);
    return NM_ACT_STAGE_RETURN_POSTPONE;
}

static void
carrier_changed(NMSupplicantInterface *iface, GParamSpec *pspec, NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMDeviceStateReason      reason;
    NMActStageReturn         ret;

    if (!nm_device_has_carrier(NM_DEVICE(self)))
        return;

    _LOGD(LOGD_DEVICE | LOGD_ETHER, "got carrier, initializing supplicant");
    nm_clear_g_signal_handler(self, &priv->carrier_id);
    ret = supplicant_check_secrets_needed(self, &reason);
    if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
        nm_device_state_changed(NM_DEVICE(self), NM_DEVICE_STATE_FAILED, reason);
    }
}

/*****************************************************************************/

static void
_ppp_mgr_cleanup(NMDeviceEthernet *self)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    nm_clear_pointer(&priv->ppp_data.ppp_mgr, nm_ppp_mgr_destroy);
}

static void
_ppp_mgr_stage3_maybe_ready(NMDeviceEthernet *self)
{
    NMDevice                *device = NM_DEVICE(self);
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    int                      IS_IPv4;

    for (IS_IPv4 = 1; IS_IPv4 >= 0; IS_IPv4--) {
        const int             addr_family = IS_IPv4 ? AF_INET : AF_INET6;
        const NMPppMgrIPData *ip_data;

        ip_data = nm_ppp_mgr_get_ip_data(priv->ppp_data.ppp_mgr, addr_family);
        if (ip_data->ip_received)
            nm_device_devip_set_state(device, addr_family, NM_DEVICE_IP_STATE_READY, ip_data->l3cd);
    }

    if (nm_ppp_mgr_get_state(priv->ppp_data.ppp_mgr) >= NM_PPP_MGR_STATE_HAVE_IP_CONFIG)
        nm_device_devip_set_state(device, AF_UNSPEC, NM_DEVICE_IP_STATE_READY, NULL);
}

static void
_ppp_mgr_callback(NMPppMgr *ppp_mgr, const NMPppMgrCallbackData *callback_data, gpointer user_data)
{
    NMDeviceEthernet *self   = NM_DEVICE_ETHERNET(user_data);
    NMDevice         *device = NM_DEVICE(self);
    NMDeviceState     device_state;

    if (callback_data->callback_type != NM_PPP_MGR_CALLBACK_TYPE_STATE_CHANGED)
        return;

    device_state = nm_device_get_state(device);

    if (callback_data->data.state >= _NM_PPP_MGR_STATE_FAILED_START) {
        if (device_state <= NM_DEVICE_STATE_ACTIVATED)
            nm_device_state_changed(device, NM_DEVICE_STATE_FAILED, callback_data->data.reason);
        return;
    }

    if (device_state < NM_DEVICE_STATE_IP_CONFIG) {
        if (callback_data->data.state >= NM_PPP_MGR_STATE_HAVE_IFINDEX) {
            gs_free char *old_name = NULL;

            if (!nm_device_set_ip_ifindex(device, callback_data->data.ifindex)) {
                _LOGW(LOGD_DEVICE | LOGD_PPP,
                      "could not set ip-ifindex %d",
                      callback_data->data.ifindex);
                _ppp_mgr_cleanup(self);
                nm_device_state_changed(device,
                                        NM_DEVICE_STATE_FAILED,
                                        NM_DEVICE_STATE_REASON_CONFIG_FAILED);
                return;
            }

            if (old_name)
                nm_manager_remove_device(NM_MANAGER_GET, old_name, NM_DEVICE_TYPE_PPP);

            nm_device_activate_schedule_stage2_device_config(device, FALSE);
        }
        return;
    }

    _ppp_mgr_stage3_maybe_ready(self);
}

/*****************************************************************************/

static void dcb_state(NMDevice *device, gboolean timeout);

static gboolean
dcb_carrier_timeout(gpointer user_data)
{
    NMDeviceEthernet        *self   = NM_DEVICE_ETHERNET(user_data);
    NMDeviceEthernetPrivate *priv   = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMDevice                *device = NM_DEVICE(user_data);

    g_return_val_if_fail(nm_device_get_state(device) == NM_DEVICE_STATE_CONFIG, G_SOURCE_REMOVE);

    priv->dcb_timeout_id = 0;
    if (priv->dcb_wait != DCB_WAIT_CARRIER_POSTCONFIG_DOWN) {
        _LOGW(LOGD_DCB, "DCB: timed out waiting for carrier (step %d)", priv->dcb_wait);
    }
    dcb_state(device, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean
dcb_configure(NMDevice *device)
{
    NMDeviceEthernet        *self = (NMDeviceEthernet *) device;
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMSettingDcb            *s_dcb;
    GError                  *error = NULL;

    nm_clear_g_source(&priv->dcb_timeout_id);

    s_dcb = nm_device_get_applied_setting(device, NM_TYPE_SETTING_DCB);

    g_return_val_if_fail(s_dcb, FALSE);

    if (!nm_dcb_setup(nm_device_get_iface(device), s_dcb, &error)) {
        _LOGW(LOGD_DCB, "Activation: (ethernet) failed to enable DCB/FCoE: %s", error->message);
        g_clear_error(&error);
        return FALSE;
    }

    /* Pause again just in case the device takes the carrier down when
     * setting specific DCB attributes.
     */
    _LOGD(LOGD_DCB, "waiting for carrier (postconfig down)");
    priv->dcb_wait       = DCB_WAIT_CARRIER_POSTCONFIG_DOWN;
    priv->dcb_timeout_id = g_timeout_add_seconds(3, dcb_carrier_timeout, device);
    return TRUE;
}

static gboolean
dcb_enable(NMDevice *device)
{
    NMDeviceEthernet        *self  = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv  = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    GError                  *error = NULL;

    nm_clear_g_source(&priv->dcb_timeout_id);
    if (!nm_dcb_enable(nm_device_get_iface(device), TRUE, &error)) {
        _LOGW(LOGD_DCB, "Activation: (ethernet) failed to enable DCB/FCoE: %s", error->message);
        g_clear_error(&error);
        return FALSE;
    }

    /* Pause for 3 seconds after enabling DCB to let the card reconfigure
     * itself.  Drivers will often re-initialize internal settings which
     * takes the carrier down for 2 or more seconds.  During this time,
     * lldpad will refuse to do anything else with the card since the carrier
     * is down.  But NM might get the carrier-down signal long after calling
     * "dcbtool dcb on", so we have to first wait for the carrier to go down.
     */
    _LOGD(LOGD_DCB, "waiting for carrier (preconfig down)");
    priv->dcb_wait       = DCB_WAIT_CARRIER_PRECONFIG_DOWN;
    priv->dcb_timeout_id = g_timeout_add_seconds(3, dcb_carrier_timeout, device);
    return TRUE;
}

static void
dcb_state(NMDevice *device, gboolean timeout)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    gboolean                 carrier;

    g_return_if_fail(nm_device_get_state(device) == NM_DEVICE_STATE_CONFIG);

    carrier = nm_platform_link_is_connected(nm_device_get_platform(device),
                                            nm_device_get_ifindex(device));
    _LOGD(LOGD_DCB, "dcb_state() wait %d carrier %d timeout %d", priv->dcb_wait, carrier, timeout);

    switch (priv->dcb_wait) {
    case DCB_WAIT_CARRIER_PREENABLE_UP:
        if (timeout || carrier) {
            _LOGD(LOGD_DCB, "dcb_state() enabling DCB");
            nm_clear_g_source(&priv->dcb_timeout_id);
            if (!dcb_enable(device)) {
                priv->dcb_handle_carrier_changes = FALSE;
                nm_device_state_changed(device,
                                        NM_DEVICE_STATE_FAILED,
                                        NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED);
            }
        }
        break;
    case DCB_WAIT_CARRIER_PRECONFIG_DOWN:
        nm_clear_g_source(&priv->dcb_timeout_id);
        priv->dcb_wait = DCB_WAIT_CARRIER_PRECONFIG_UP;

        if (!carrier) {
            /* Wait for the carrier to come back up */
            _LOGD(LOGD_DCB, "waiting for carrier (preconfig up)");
            priv->dcb_timeout_id = g_timeout_add_seconds(5, dcb_carrier_timeout, device);
            break;
        }
        _LOGD(LOGD_DCB, "dcb_state() preconfig down falling through");
        /* fall-through */
    case DCB_WAIT_CARRIER_PRECONFIG_UP:
        if (timeout || carrier) {
            _LOGD(LOGD_DCB, "dcb_state() preconfig up configuring DCB");
            nm_clear_g_source(&priv->dcb_timeout_id);
            if (!dcb_configure(device)) {
                priv->dcb_handle_carrier_changes = FALSE;
                nm_device_state_changed(device,
                                        NM_DEVICE_STATE_FAILED,
                                        NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED);
            }
        }
        break;
    case DCB_WAIT_CARRIER_POSTCONFIG_DOWN:
        nm_clear_g_source(&priv->dcb_timeout_id);
        priv->dcb_wait = DCB_WAIT_CARRIER_POSTCONFIG_UP;

        if (!carrier) {
            /* Wait for the carrier to come back up */
            _LOGD(LOGD_DCB, "waiting for carrier (postconfig up)");
            priv->dcb_timeout_id = g_timeout_add_seconds(5, dcb_carrier_timeout, device);
            break;
        }
        _LOGD(LOGD_DCB, "dcb_state() postconfig down falling through");
        /* fall-through */
    case DCB_WAIT_CARRIER_POSTCONFIG_UP:
        if (timeout || carrier) {
            _LOGD(LOGD_DCB, "dcb_state() postconfig up starting IP");
            nm_clear_g_source(&priv->dcb_timeout_id);
            priv->dcb_handle_carrier_changes = FALSE;
            priv->dcb_wait                   = DCB_WAIT_UNKNOWN;
            nm_device_activate_schedule_stage2_device_config(device, FALSE);
        }
        break;
    default:
        g_assert_not_reached();
    }
}

/*****************************************************************************/

static gboolean
wake_on_lan_enable(NMDevice *device)
{
    NMSettingWiredWakeOnLan wol;
    NMSettingWired         *s_wired;
    const char             *password = NULL;

    s_wired = nm_device_get_applied_setting(device, NM_TYPE_SETTING_WIRED);

    if (NM_IS_DEVICE_VETH(device))
        return FALSE;

    if (s_wired) {
        wol      = nm_setting_wired_get_wake_on_lan(s_wired);
        password = nm_setting_wired_get_wake_on_lan_password(s_wired);

        /* NMSettingWired does not reject invalid flags. Filter them out here. */
        wol = (wol
               & (NM_SETTING_WIRED_WAKE_ON_LAN_ALL | NM_SETTING_WIRED_WAKE_ON_LAN_EXCLUSIVE_FLAGS));

        if (wol != NM_SETTING_WIRED_WAKE_ON_LAN_DEFAULT)
            goto found;
    }

    wol = nm_config_data_get_connection_default_int64(NM_CONFIG_GET_DATA,
                                                      NM_CON_DEFAULT("ethernet.wake-on-lan"),
                                                      device,
                                                      NM_SETTING_WIRED_WAKE_ON_LAN_NONE,
                                                      G_MAXINT32,
                                                      NM_SETTING_WIRED_WAKE_ON_LAN_DEFAULT);

    if (NM_FLAGS_ANY(wol, NM_SETTING_WIRED_WAKE_ON_LAN_EXCLUSIVE_FLAGS)
        && !nm_utils_is_power_of_two(wol)) {
        nm_log_dbg(LOGD_ETHER, "invalid default value %u for wake-on-lan", (guint) wol);
        wol = NM_SETTING_WIRED_WAKE_ON_LAN_DEFAULT;
    }

    wol = wol & (NM_SETTING_WIRED_WAKE_ON_LAN_ALL | NM_SETTING_WIRED_WAKE_ON_LAN_EXCLUSIVE_FLAGS);

    if (wol != NM_SETTING_WIRED_WAKE_ON_LAN_DEFAULT)
        goto found;

    wol = NM_SETTING_WIRED_WAKE_ON_LAN_IGNORE;

found:
    return nm_platform_ethtool_set_wake_on_lan(nm_device_get_platform(device),
                                               nm_device_get_ifindex(device),
                                               _NM_SETTING_WIRED_WAKE_ON_LAN_CAST(wol),
                                               password);
}

/*****************************************************************************/

static NMActStageReturn
act_stage2_config(NMDevice *device, NMDeviceStateReason *out_failure_reason)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMConnection            *connection;
    NMSettingConnection     *s_con;
    const char              *connection_type;
    NMSettingDcb            *s_dcb;
    NMActRequest            *req;

    connection = nm_device_get_applied_connection(device);
    g_return_val_if_fail(connection, NM_ACT_STAGE_RETURN_FAILURE);

    s_con = _nm_connection_get_setting(connection, NM_TYPE_SETTING_CONNECTION);
    g_return_val_if_fail(s_con, NM_ACT_STAGE_RETURN_FAILURE);

    nm_clear_g_source(&priv->dcb_timeout_id);
    priv->dcb_handle_carrier_changes = FALSE;

    connection_type = nm_setting_connection_get_connection_type(s_con);

    if (nm_streq(connection_type, NM_SETTING_PPPOE_SETTING_NAME)) {
        if (!priv->ppp_data.ppp_mgr) {
            gs_free_error GError *error = NULL;
            NMSettingPppoe       *s_pppoe;
            NMSettingPpp         *s_ppp;

            s_ppp = nm_device_get_applied_setting(device, NM_TYPE_SETTING_PPP);
            if (s_ppp) {
                guint32 mtu;
                guint32 mru;
                guint32 mxu;

                mtu = nm_setting_ppp_get_mtu(s_ppp);
                mru = nm_setting_ppp_get_mru(s_ppp);
                mxu = NM_MAX(mru, mtu);
                if (mxu) {
                    _LOGD(LOGD_PPP,
                          "set MTU to %u (PPP interface MRU %u, MTU %u)",
                          mxu + PPPOE_ENCAP_OVERHEAD,
                          mru,
                          mtu);
                    nm_platform_link_set_mtu(nm_device_get_platform(device),
                                             nm_device_get_ifindex(device),
                                             mxu + PPPOE_ENCAP_OVERHEAD);
                }
            }

            req = nm_device_get_act_request(device);
            g_return_val_if_fail(req, NM_ACT_STAGE_RETURN_FAILURE);

            s_pppoe = _nm_connection_get_setting(connection, NM_TYPE_SETTING_PPPOE);
            g_return_val_if_fail(s_pppoe, NM_ACT_STAGE_RETURN_FAILURE);

            priv->ppp_data.ppp_mgr =
                nm_ppp_mgr_start(&((const NMPppMgrConfig) {
                                     .netns         = nm_device_get_netns(device),
                                     .parent_iface  = nm_device_get_iface(device),
                                     .callback      = _ppp_mgr_callback,
                                     .user_data     = self,
                                     .act_req       = req,
                                     .ppp_username  = nm_setting_pppoe_get_username(s_pppoe),
                                     .timeout_secs  = 30,
                                     .baud_override = 0,
                                 }),
                                 &error);
            if (!priv->ppp_data.ppp_mgr) {
                _LOGW(LOGD_DEVICE | LOGD_PPP, "PPPoE failed to start: %s", error->message);
                *out_failure_reason = NM_DEVICE_STATE_REASON_PPP_START_FAILED;
                return NM_ACT_STAGE_RETURN_FAILURE;
            }

            return NM_ACT_STAGE_RETURN_POSTPONE;
        }

        if (nm_ppp_mgr_get_state(priv->ppp_data.ppp_mgr) < NM_PPP_MGR_STATE_HAVE_IFINDEX)
            return NM_ACT_STAGE_RETURN_POSTPONE;
    }

    /* 802.1x has to run before any IP configuration since the 802.1x auth
     * process opens the port up for normal traffic.
     */
    if (nm_streq(connection_type, NM_SETTING_WIRED_SETTING_NAME)) {
        NMSetting8021x *security;

        security = nm_device_get_applied_setting(device, NM_TYPE_SETTING_802_1X);

        if (security) {
            /* FIXME: we always return from this. stage2 must be re-entrant, and
             * process all the necessary steps. Just returning for 8021x is wrong. */

            if (priv->supplicant.ready)
                return NM_ACT_STAGE_RETURN_SUCCESS;

            if (!nm_device_has_carrier(NM_DEVICE(self))) {
                _LOGD(LOGD_DEVICE | LOGD_ETHER,
                      "delay supplicant initialization until carrier goes up");
                priv->carrier_id = g_signal_connect(self,
                                                    "notify::" NM_DEVICE_CARRIER,
                                                    G_CALLBACK(carrier_changed),
                                                    self);
                return NM_ACT_STAGE_RETURN_POSTPONE;
            }

            return supplicant_check_secrets_needed(self, out_failure_reason);
        }
    }

    wake_on_lan_enable(device);

    /* DCB and FCoE setup */
    s_dcb = nm_device_get_applied_setting(device, NM_TYPE_SETTING_DCB);
    if (!priv->stage2_ready_dcb && s_dcb) {
        /* lldpad really really wants the carrier to be up */
        if (nm_platform_link_is_connected(nm_device_get_platform(device),
                                          nm_device_get_ifindex(device))) {
            if (!dcb_enable(device)) {
                NM_SET_OUT(out_failure_reason, NM_DEVICE_STATE_REASON_DCB_FCOE_FAILED);
                return NM_ACT_STAGE_RETURN_FAILURE;
            }
        } else {
            _LOGD(LOGD_DCB, "waiting for carrier (preenable up)");
            priv->dcb_wait       = DCB_WAIT_CARRIER_PREENABLE_UP;
            priv->dcb_timeout_id = g_timeout_add_seconds(4, dcb_carrier_timeout, device);
        }

        priv->dcb_handle_carrier_changes = TRUE;
        return NM_ACT_STAGE_RETURN_POSTPONE;
    }

    return NM_ACT_STAGE_RETURN_SUCCESS;
}

static guint32
get_configured_mtu(NMDevice *device, NMDeviceMtuSource *out_source, gboolean *out_force)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(device);

    /* MTU only set for plain ethernet */
    if (priv->ppp_data.ppp_mgr)
        return 0;

    return nm_device_get_configured_mtu_for_wired(device, out_source, out_force);
}

static void
act_stage3_ip_config(NMDevice *device, int addr_family)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMPppMgrState            ppp_state;

    if (!priv->ppp_data.ppp_mgr)
        return;

    ppp_state = nm_ppp_mgr_get_state(priv->ppp_data.ppp_mgr);

    nm_assert(NM_IN_SET(ppp_state, NM_PPP_MGR_STATE_HAVE_IFINDEX, NM_PPP_MGR_STATE_HAVE_IP_CONFIG));

    if (ppp_state < NM_PPP_MGR_STATE_HAVE_IP_CONFIG) {
        nm_device_devip_set_state(device, AF_UNSPEC, NM_DEVICE_IP_STATE_PENDING, NULL);
        return;
    }

    _ppp_mgr_stage3_maybe_ready(self);
}

static void
deactivate(NMDevice *device)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    NMSettingDcb            *s_dcb;
    GError                  *error = NULL;
    int                      ifindex;

    nm_clear_g_source_inst(&priv->ppp_data.wait_source);
    nm_clear_g_signal_handler(self, &priv->carrier_id);

    _ppp_mgr_cleanup(self);

    supplicant_interface_release(self);

    priv->dcb_wait = DCB_WAIT_UNKNOWN;
    nm_clear_g_source(&priv->dcb_timeout_id);
    priv->dcb_handle_carrier_changes = FALSE;
    priv->stage2_ready_dcb           = FALSE;

    /* Tear down DCB/FCoE if it was enabled */
    s_dcb = nm_device_get_applied_setting(device, NM_TYPE_SETTING_DCB);
    if (s_dcb) {
        if (!nm_dcb_cleanup(nm_device_get_iface(device), &error)) {
            _LOGW(LOGD_DEVICE | LOGD_PLATFORM, "failed to disable DCB/FCoE: %s", error->message);
            g_clear_error(&error);
        }
    }

    /* Set last PPPoE connection time */
    if (nm_device_get_applied_setting(device, NM_TYPE_SETTING_PPPOE))
        priv->ppp_data.last_pppoe_time_msec = nm_utils_get_monotonic_timestamp_msec();

    ifindex = nm_device_get_ifindex(device);
    if (ifindex > 0 && priv->ethtool_prev_set) {
        priv->ethtool_prev_set = FALSE;

        _LOGD(LOGD_DEVICE,
              "set-link: reset %snegotiation (%u Mbit, %s duplex)",
              priv->ethtool_prev_autoneg ? "auto-" : "static ",
              priv->ethtool_prev_speed,
              nm_platform_link_duplex_type_to_string(priv->ethtool_prev_duplex));
        if (!nm_platform_ethtool_set_link_settings(nm_device_get_platform(device),
                                                   ifindex,
                                                   priv->ethtool_prev_autoneg,
                                                   priv->ethtool_prev_speed,
                                                   priv->ethtool_prev_duplex)) {
            _LOGW(LOGD_DEVICE, "set-link: failure to reset link negotiation");
            return;
        }
    }
}

static gboolean
complete_connection(NMDevice            *device,
                    NMConnection        *connection,
                    const char          *specific_object,
                    NMConnection *const *existing_connections,
                    GError             **error)
{
    NMSettingWired *s_wired;
    NMSettingPppoe *s_pppoe;

    if (nm_streq0(nm_connection_get_connection_type(connection), NM_SETTING_VETH_SETTING_NAME)) {
        NMSettingVeth *s_veth;
        const char    *peer_name     = NULL;
        const char    *con_peer_name = NULL;
        int            ifindex;

        nm_utils_complete_generic(nm_device_get_platform(device),
                                  connection,
                                  NM_SETTING_VETH_SETTING_NAME,
                                  existing_connections,
                                  NULL,
                                  _("Veth connection"),
                                  "veth",
                                  NULL);

        s_veth = _nm_connection_ensure_setting(connection, NM_TYPE_SETTING_VETH);

        ifindex = nm_device_get_ip_ifindex(device);
        if (ifindex > 0) {
            const NMPlatformLink *pllink;

            pllink = nm_platform_link_get(nm_device_get_platform(device), ifindex);
            if (pllink && pllink->type == NM_LINK_TYPE_VETH && pllink->parent > 0) {
                pllink = nm_platform_link_get(nm_device_get_platform(device), pllink->parent);

                if (pllink && pllink->type == NM_LINK_TYPE_VETH) {
                    peer_name = pllink->name;
                }
            }
        }

        if (!peer_name) {
            nm_utils_error_set(error, NM_UTILS_ERROR_UNKNOWN, "cannot find peer for veth device");
            return FALSE;
        }

        con_peer_name = nm_setting_veth_get_peer(s_veth);
        if (con_peer_name) {
            nm_utils_error_set(error,
                               NM_UTILS_ERROR_UNKNOWN,
                               "mismatching veth peer \"%s\"",
                               con_peer_name);
            return FALSE;
        } else
            g_object_set(s_veth, NM_SETTING_VETH_PEER, peer_name, NULL);

        return TRUE;
    }

    s_pppoe = nm_connection_get_setting_pppoe(connection);

    /* We can't telepathically figure out the service name or username, so if
     * those weren't given, we can't complete the connection.
     */
    if (s_pppoe && !nm_setting_verify(NM_SETTING(s_pppoe), NULL, error))
        return FALSE;

    s_wired = _nm_connection_ensure_setting(connection, NM_TYPE_SETTING_WIRED);

    /* Default to an ethernet-only connection, but if a PPPoE setting was given
     * then PPPoE should be our connection type.
     */
    nm_utils_complete_generic(
        nm_device_get_platform(device),
        connection,
        s_pppoe ? NM_SETTING_PPPOE_SETTING_NAME : NM_SETTING_WIRED_SETTING_NAME,
        existing_connections,
        NULL,
        s_pppoe ? _("PPPoE connection") : _("Wired connection"),
        NULL,
        nm_setting_wired_get_mac_address(s_wired) ? NULL : nm_device_get_iface(device));

    return TRUE;
}

static NMConnection *
new_default_connection(NMDevice *self)
{
    NMConnection                  *connection;
    NMSettingsConnection *const   *connections;
    NMSetting                     *setting;
    gs_unref_hashtable GHashTable *existing_ids = NULL;
    const char                    *perm_hw_addr;
    const char                    *iface;
    gs_free char                  *defname = NULL;
    gs_free char                  *uuid    = NULL;
    guint                          i, n_connections;

    perm_hw_addr = nm_device_get_permanent_hw_address(self);
    iface        = nm_device_get_iface(self);

    connection = nm_simple_connection_new();
    setting    = nm_setting_connection_new();
    nm_connection_add_setting(connection, setting);

    connections = nm_settings_get_connections(nm_device_get_settings(self), &n_connections);
    if (n_connections > 0) {
        existing_ids = g_hash_table_new(nm_str_hash, g_str_equal);
        for (i = 0; i < n_connections; i++)
            g_hash_table_add(existing_ids, (char *) nm_settings_connection_get_id(connections[i]));
    }
    defname = nm_device_ethernet_utils_get_default_wired_name(existing_ids);
    if (!defname)
        return NULL;

    /* Create a stable UUID. The UUID is also the Network_ID for stable-privacy addr-gen-mode,
     * thus when it changes we will also generate different IPv6 addresses. */
    uuid = nm_uuid_generate_from_strings_old("default-wired",
                                             nm_utils_machine_id_str(),
                                             defname,
                                             perm_hw_addr ?: iface);

    g_object_set(setting,
                 NM_SETTING_CONNECTION_ID,
                 defname,
                 NM_SETTING_CONNECTION_TYPE,
                 NM_SETTING_WIRED_SETTING_NAME,
                 NM_SETTING_CONNECTION_AUTOCONNECT,
                 TRUE,
                 NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY,
                 NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY_MIN,
                 NM_SETTING_CONNECTION_UUID,
                 uuid,
                 NM_SETTING_CONNECTION_TIMESTAMP,
                 (guint64) time(NULL),
                 NM_SETTING_CONNECTION_INTERFACE_NAME,
                 iface,
                 NULL);

    return connection;
}

static const char *
get_s390_subchannels(NMDevice *device)
{
    nm_assert(NM_IS_DEVICE_ETHERNET(device));

    return NM_DEVICE_ETHERNET_GET_PRIVATE(device)->subchannels;
}

static void
update_connection(NMDevice *device, NMConnection *connection)
{
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(device);
    NMSettingWired *s_wired = _nm_connection_ensure_setting(connection, NM_TYPE_SETTING_WIRED);
    gboolean        perm_hw_addr_is_fake;
    const char     *perm_hw_addr;
    const char     *mac      = nm_device_get_hw_address(device);
    const char     *mac_prop = NM_SETTING_WIRED_MAC_ADDRESS;
    GHashTableIter  iter;
    const char     *key;
    const char     *value;

    g_object_set(nm_connection_get_setting_connection(connection),
                 NM_SETTING_CONNECTION_TYPE,
                 nm_connection_get_setting_pppoe(connection) ? NM_SETTING_PPPOE_SETTING_NAME
                                                             : NM_SETTING_WIRED_SETTING_NAME,
                 NULL);

    /* If the device reports a permanent address, use that for the MAC address
     * and the current MAC, if different, is the cloned MAC.
     */
    perm_hw_addr = nm_device_get_permanent_hw_address_full(device, TRUE, &perm_hw_addr_is_fake);
    if (perm_hw_addr && !perm_hw_addr_is_fake) {
        g_object_set(s_wired, NM_SETTING_WIRED_MAC_ADDRESS, perm_hw_addr, NULL);

        mac_prop = NULL;
        if (mac && !nm_utils_hwaddr_matches(perm_hw_addr, -1, mac, -1))
            mac_prop = NM_SETTING_WIRED_CLONED_MAC_ADDRESS;
    }

    if (mac_prop && mac && nm_utils_hwaddr_valid(mac, ETH_ALEN))
        g_object_set(s_wired, mac_prop, mac, NULL);

    /* We don't set the MTU as we don't know whether it was set explicitly */

    /* s390 */
    if (priv->subchannels_dbus)
        g_object_set(s_wired, NM_SETTING_WIRED_S390_SUBCHANNELS, priv->subchannels_dbus, NULL);
    if (priv->s390_nettype)
        g_object_set(s_wired, NM_SETTING_WIRED_S390_NETTYPE, priv->s390_nettype, NULL);

    _nm_setting_wired_clear_s390_options(s_wired);
    g_hash_table_iter_init(&iter, priv->s390_options);
    while (g_hash_table_iter_next(&iter, (gpointer *) &key, (gpointer *) &value))
        nm_setting_wired_add_s390_option(s_wired, key, value);
}

static void
link_speed_update(NMDevice *device)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);
    guint32                  speed;

    if (!nm_platform_ethtool_get_link_settings(nm_device_get_platform(device),
                                               nm_device_get_ifindex(device),
                                               NULL,
                                               &speed,
                                               NULL))
        return;
    if (priv->speed == speed)
        return;

    priv->speed = speed;
    _LOGD(LOGD_PLATFORM | LOGD_ETHER, "speed is now %d Mb/s", speed);
    _notify(self, PROP_SPEED);
}

static void
carrier_changed_notify(NMDevice *device, gboolean carrier)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(device);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    if (priv->dcb_handle_carrier_changes) {
        nm_assert(nm_device_get_state(device) == NM_DEVICE_STATE_CONFIG);

        if (priv->dcb_timeout_id) {
            _LOGD(LOGD_DCB, "carrier_changed() calling dcb_state()");
            dcb_state(device, FALSE);
        }
    }

    if (carrier)
        link_speed_update(device);

    NM_DEVICE_CLASS(nm_device_ethernet_parent_class)->carrier_changed_notify(device, carrier);
}

static void
link_changed(NMDevice *device, const NMPlatformLink *pllink)
{
    NM_DEVICE_CLASS(nm_device_ethernet_parent_class)->link_changed(device, pllink);
    if (!NM_IS_DEVICE_VETH(device) && pllink->initialized)
        _update_s390_subchannels((NMDeviceEthernet *) device);
}

static gboolean
is_available(NMDevice *device, NMDeviceCheckDevAvailableFlags flags)
{
    if (!NM_DEVICE_CLASS(nm_device_ethernet_parent_class)->is_available(device, flags))
        return FALSE;

    return !!nm_device_get_initial_hw_address(device);
}

static const char *
get_ip_method_auto(NMDevice *device, int addr_family)
{
    NMSettingConnection *s_con;

    s_con = nm_device_get_applied_setting(device, NM_TYPE_SETTING_CONNECTION);
    g_return_val_if_fail(s_con,
                         NM_IS_IPv4(addr_family) ? NM_SETTING_IP4_CONFIG_METHOD_AUTO
                                                 : NM_SETTING_IP6_CONFIG_METHOD_AUTO);

    if (!nm_streq(nm_setting_connection_get_connection_type(s_con),
                  NM_SETTING_PPPOE_SETTING_NAME)) {
        return NM_DEVICE_CLASS(nm_device_ethernet_parent_class)
            ->get_ip_method_auto(device, addr_family);
    }

    if (NM_IS_IPv4(addr_family)) {
        /* We cannot do DHCPv4 on a PPP link, instead we get "auto" IP addresses
         * by pppd. Return "manual" here, which has the suitable effect to a
         * (zero) manual addresses in addition. */
        return NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
    }

    return NM_SETTING_IP6_CONFIG_METHOD_AUTO;
}

static gboolean
can_reapply_change(NMDevice   *device,
                   const char *setting_name,
                   NMSetting  *s_old,
                   NMSetting  *s_new,
                   GHashTable *diffs,
                   GError    **error)
{
    NMDeviceClass *device_class;

    /* Only handle wired setting here, delegate other settings to parent class */
    if (nm_streq(setting_name, NM_SETTING_WIRED_SETTING_NAME)) {
        return nm_device_hash_check_invalid_keys(
            diffs,
            NM_SETTING_WIRED_SETTING_NAME,
            error,
            NM_SETTING_WIRED_MTU, /* reapplied with IP config */
            NM_SETTING_WIRED_SPEED,
            NM_SETTING_WIRED_DUPLEX,
            NM_SETTING_WIRED_AUTO_NEGOTIATE,
            NM_SETTING_WIRED_WAKE_ON_LAN,
            NM_SETTING_WIRED_WAKE_ON_LAN_PASSWORD);
    }

    device_class = NM_DEVICE_CLASS(nm_device_ethernet_parent_class);
    return device_class->can_reapply_change(device, setting_name, s_old, s_new, diffs, error);
}

static void
reapply_connection(NMDevice *device, NMConnection *con_old, NMConnection *con_new)
{
    NMDeviceEthernet *self  = NM_DEVICE_ETHERNET(device);
    NMDeviceState     state = nm_device_get_state(device);

    NM_DEVICE_CLASS(nm_device_ethernet_parent_class)->reapply_connection(device, con_old, con_new);

    _LOGD(LOGD_DEVICE, "reapplying wired settings");

    if (state >= NM_DEVICE_STATE_PREPARE)
        link_negotiation_set(device);
    if (state >= NM_DEVICE_STATE_CONFIG)
        wake_on_lan_enable(device);
}

static void
dispose(GObject *object)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(object);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    wired_secrets_cancel(self);

    supplicant_interface_release(self);

    nm_clear_g_source_inst(&priv->ppp_data.wait_source);

    nm_clear_g_source(&priv->dcb_timeout_id);

    nm_clear_g_signal_handler(self, &priv->carrier_id);

    G_OBJECT_CLASS(nm_device_ethernet_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(object);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    g_clear_object(&priv->supplicant.mgr);
    g_free(priv->subchan1);
    g_free(priv->subchan2);
    g_free(priv->subchan3);
    g_free(priv->subchannels);
    g_strfreev(priv->subchannels_dbus);
    g_free(priv->s390_nettype);
    g_hash_table_destroy(priv->s390_options);

    G_OBJECT_CLASS(nm_device_ethernet_parent_class)->finalize(object);
}

static void
get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMDeviceEthernet        *self = NM_DEVICE_ETHERNET(object);
    NMDeviceEthernetPrivate *priv = NM_DEVICE_ETHERNET_GET_PRIVATE(self);

    switch (prop_id) {
    case PROP_SPEED:
        g_value_set_uint(value, priv->speed);
        break;
    case PROP_S390_SUBCHANNELS:
        g_value_set_boxed(value, priv->subchannels_dbus);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static const NMDBusInterfaceInfoExtended interface_info_device_wired = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE_DEVICE_WIRED,
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("HwAddress", "s", NM_DEVICE_HW_ADDRESS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE(
                "PermHwAddress",
                "s",
                NM_DEVICE_PERM_HW_ADDRESS,
                .annotations = NM_GDBUS_ANNOTATION_INFO_LIST_DEPRECATED(), ),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Speed", "u", NM_DEVICE_ETHERNET_SPEED),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("S390Subchannels",
                                                           "as",
                                                           NM_DEVICE_ETHERNET_S390_SUBCHANNELS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE(
                "Carrier",
                "b",
                NM_DEVICE_CARRIER,
                .annotations = NM_GDBUS_ANNOTATION_INFO_LIST_DEPRECATED(), ), ), ),
};

static void
nm_device_ethernet_class_init(NMDeviceEthernetClass *klass)
{
    GObjectClass      *object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);
    NMDeviceClass     *device_class      = NM_DEVICE_CLASS(klass);

    g_type_class_add_private(object_class, sizeof(NMDeviceEthernetPrivate));

    object_class->dispose      = dispose;
    object_class->finalize     = finalize;
    object_class->get_property = get_property;
    object_class->set_property = set_property;

    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_device_wired);

    device_class->connection_type_supported = NM_SETTING_WIRED_SETTING_NAME;
    device_class->link_types                = NM_DEVICE_DEFINE_LINK_TYPES(NM_LINK_TYPE_ETHERNET);

    device_class->get_generic_capabilities    = get_generic_capabilities;
    device_class->check_connection_compatible = check_connection_compatible;
    device_class->complete_connection         = complete_connection;
    device_class->new_default_connection      = new_default_connection;

    device_class->act_stage1_prepare_also_for_external_or_assume = TRUE;
    device_class->act_stage1_prepare                             = act_stage1_prepare;
    device_class->act_stage1_prepare_set_hwaddr_ethernet         = TRUE;
    device_class->act_stage2_config                              = act_stage2_config;
    device_class->act_stage3_ip_config                           = act_stage3_ip_config;
    device_class->get_configured_mtu                             = get_configured_mtu;
    device_class->get_ip_method_auto                             = get_ip_method_auto;
    device_class->deactivate                                     = deactivate;
    device_class->get_s390_subchannels                           = get_s390_subchannels;
    device_class->update_connection                              = update_connection;
    device_class->carrier_changed_notify                         = carrier_changed_notify;
    device_class->link_changed                                   = link_changed;
    device_class->is_available                                   = is_available;
    device_class->can_reapply_change                             = can_reapply_change;
    device_class->reapply_connection                             = reapply_connection;

    device_class->state_changed = device_state_changed;

    obj_properties[PROP_SPEED] = g_param_spec_uint(NM_DEVICE_ETHERNET_SPEED,
                                                   "",
                                                   "",
                                                   0,
                                                   G_MAXUINT32,
                                                   0,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_S390_SUBCHANNELS] =
        g_param_spec_boxed(NM_DEVICE_ETHERNET_S390_SUBCHANNELS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);
}

/*****************************************************************************/

#define NM_TYPE_ETHERNET_DEVICE_FACTORY (nm_ethernet_device_factory_get_type())
#define NM_ETHERNET_DEVICE_FACTORY(obj)                              \
    (_NM_G_TYPE_CHECK_INSTANCE_CAST((obj),                           \
                                    NM_TYPE_ETHERNET_DEVICE_FACTORY, \
                                    NMEthernetDeviceFactory))

static NMDevice *
create_device(NMDeviceFactory      *factory,
              const char           *iface,
              const NMPlatformLink *plink,
              NMConnection         *connection,
              gboolean             *out_ignore)
{
    return g_object_new(NM_TYPE_DEVICE_ETHERNET,
                        NM_DEVICE_IFACE,
                        iface,
                        NM_DEVICE_TYPE_DESC,
                        "Ethernet",
                        NM_DEVICE_DEVICE_TYPE,
                        NM_DEVICE_TYPE_ETHERNET,
                        NM_DEVICE_LINK_TYPE,
                        NM_LINK_TYPE_ETHERNET,
                        NULL);
}

static gboolean
match_connection(NMDeviceFactory *factory, NMConnection *connection)
{
    const char     *type = nm_connection_get_connection_type(connection);
    NMSettingPppoe *s_pppoe;

    if (nm_streq(type, NM_SETTING_WIRED_SETTING_NAME))
        return TRUE;

    nm_assert(nm_streq(type, NM_SETTING_PPPOE_SETTING_NAME));
    s_pppoe = nm_connection_get_setting_pppoe(connection);

    return !nm_setting_pppoe_get_parent(s_pppoe);
}

NM_DEVICE_FACTORY_DEFINE_INTERNAL(
    ETHERNET,
    Ethernet,
    ethernet,
    NM_DEVICE_FACTORY_DECLARE_LINK_TYPES(NM_LINK_TYPE_ETHERNET)
        NM_DEVICE_FACTORY_DECLARE_SETTING_TYPES(NM_SETTING_WIRED_SETTING_NAME,
                                                NM_SETTING_PPPOE_SETTING_NAME),
    factory_class->create_device    = create_device;
    factory_class->match_connection = match_connection;);
