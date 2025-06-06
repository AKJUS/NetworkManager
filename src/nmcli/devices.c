/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2010 - 2022 Red Hat, Inc.
 */

#include "libnm-client-aux-extern/nm-default-client.h"

#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#if HAVE_EDITLINE_READLINE
#include <editline/readline.h>
#else
#include <readline/readline.h>
#endif
#include <linux/if_ether.h>

#include "libnm-glib-aux/nm-secret-utils.h"
#include "libnm-glib-aux/nm-random-utils.h"
#include "common.h"
#include "connections.h"
#include "libnmc-base/nm-client-utils.h"
#include "libnmc-base/nm-secret-agent-simple.h"
#include "polkit-agent.h"
#include "utils.h"

/* define some prompts */
#define PROMPT_INTERFACE  _("Interface: ")
#define PROMPT_INTERFACES _("Interface(s): ")

/*****************************************************************************/

static char *
ap_wpa_rsn_flags_to_string(NM80211ApSecurityFlags flags, NMMetaAccessorGetType get_type)
{
    char *flags_str[16];
    int   i = 0;

    if (flags & NM_802_11_AP_SEC_PAIR_WEP40)
        flags_str[i++] = "pair_wpe40";
    if (flags & NM_802_11_AP_SEC_PAIR_WEP104)
        flags_str[i++] = "pair_wpe104";
    if (flags & NM_802_11_AP_SEC_PAIR_TKIP)
        flags_str[i++] = "pair_tkip";
    if (flags & NM_802_11_AP_SEC_PAIR_CCMP)
        flags_str[i++] = "pair_ccmp";
    if (flags & NM_802_11_AP_SEC_GROUP_WEP40)
        flags_str[i++] = "group_wpe40";
    if (flags & NM_802_11_AP_SEC_GROUP_WEP104)
        flags_str[i++] = "group_wpe104";
    if (flags & NM_802_11_AP_SEC_GROUP_TKIP)
        flags_str[i++] = "group_tkip";
    if (flags & NM_802_11_AP_SEC_GROUP_CCMP)
        flags_str[i++] = "group_ccmp";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_PSK)
        flags_str[i++] = "psk";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X)
        flags_str[i++] = "802.1X";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_SAE)
        flags_str[i++] = "sae";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)
        flags_str[i++] = "wpa-eap-suite-b-192";
    if (NM_FLAGS_ANY(flags, NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM))
        flags_str[i++] = "owe";

    /* Make sure you grow flags_str when adding items here. */
    nm_assert(i < G_N_ELEMENTS(flags_str));

    if (i == 0) {
        if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
            return g_strdup(_("(none)"));
        return g_strdup("(none)");
    }

    flags_str[i] = NULL;
    return g_strjoinv(" ", flags_str);
}

static NMMetaColor
wifi_signal_to_color(guint8 strength)
{
    if (strength > 80)
        return NM_META_COLOR_WIFI_SIGNAL_EXCELLENT;
    else if (strength > 55)
        return NM_META_COLOR_WIFI_SIGNAL_GOOD;
    else if (strength > 30)
        return NM_META_COLOR_WIFI_SIGNAL_FAIR;
    else if (strength > 5)
        return NM_META_COLOR_WIFI_SIGNAL_POOR;
    else
        return NM_META_COLOR_WIFI_SIGNAL_UNKNOWN;
}

/*****************************************************************************/

static gconstpointer
_metagen_device_status_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice           *d = target;
    NMActiveConnection *ac;

    NMC_HANDLE_COLOR(nmc_device_state_to_color(d));

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_DEVICE:
        return nm_device_get_iface(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_TYPE:
        return nm_device_get_type_description(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_STATE:
        return nmc_meta_generic_get_str_i18n(nmc_device_state_to_string_with_external(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_IP4_CONNECTIVITY:
        return nmc_meta_generic_get_str_i18n(
            nm_connectivity_to_string(nm_device_get_connectivity(d, AF_INET)),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_IP6_CONNECTIVITY:
        return nmc_meta_generic_get_str_i18n(
            nm_connectivity_to_string(nm_device_get_connectivity(d, AF_INET6)),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_DBUS_PATH:
        return nm_object_get_path(NM_OBJECT(d));
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CONNECTION:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_active_connection_get_id(ac) : NULL;
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CON_UUID:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_active_connection_get_uuid(ac) : NULL;
    case NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CON_PATH:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_object_get_path(NM_OBJECT(ac)) : NULL;
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo
    *const metagen_device_status[_NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_NUM + 1] = {
#define _METAGEN_DEVICE_STATUS(type, name) \
    [type] = NMC_META_GENERIC(name, .info_type = type, .get_fcn = _metagen_device_status_get_fcn)
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_DEVICE, "DEVICE"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_TYPE, "TYPE"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_STATE, "STATE"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_IP4_CONNECTIVITY,
                               "IP4-CONNECTIVITY"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_IP6_CONNECTIVITY,
                               "IP6-CONNECTIVITY"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_DBUS_PATH, "DBUS-PATH"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CONNECTION, "CONNECTION"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CON_UUID, "CON-UUID"),
        _METAGEN_DEVICE_STATUS(NMC_GENERIC_INFO_TYPE_DEVICE_STATUS_CON_PATH, "CON-PATH"),
};

/*****************************************************************************/

static gconstpointer
_metagen_device_detail_general_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice           *d = target;
    NMActiveConnection *ac;
    NMDeviceStateReason state_reason;
    NMConnectivityState connectivity;
    const char         *s;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DEVICE:
        return nm_device_get_iface(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_TYPE:
        return nm_device_get_type_description(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_TYPE:
        return G_OBJECT_TYPE_NAME(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DBUS_PATH:
        return nm_object_get_path(NM_OBJECT(d));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_VENDOR:
        return nm_device_get_vendor(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PRODUCT:
        return nm_device_get_product(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DRIVER:
        s = nm_device_get_driver(d);
        return s ?: nmc_meta_generic_get_unknown(get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DRIVER_VERSION:
        return nm_device_get_driver_version(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_FIRMWARE_VERSION:
        return nm_device_get_firmware_version(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_HWADDR:
        s = nm_device_get_hw_address(d);
        return s ?: nmc_meta_generic_get_unknown(get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_MTU:
        return (*out_to_free = g_strdup_printf("%u", (guint) nm_device_get_mtu(d)));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_STATE:
        return (*out_to_free = nmc_meta_generic_get_enum_with_detail(
                    NMC_META_GENERIC_GET_ENUM_TYPE_PARENTHESES,
                    nm_device_get_state(d),
                    nmc_device_state_to_string_with_external(d),
                    get_type));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_REASON:
        state_reason = nm_device_get_state_reason(d);
        return (*out_to_free = nmc_meta_generic_get_enum_with_detail(
                    NMC_META_GENERIC_GET_ENUM_TYPE_PARENTHESES,
                    state_reason,
                    nmc_device_reason_to_string(state_reason),
                    get_type));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP4_CONNECTIVITY:
        connectivity = nm_device_get_connectivity(d, AF_INET);
        return (*out_to_free = nmc_meta_generic_get_enum_with_detail(
                    NMC_META_GENERIC_GET_ENUM_TYPE_PARENTHESES,
                    connectivity,
                    nm_connectivity_to_string(connectivity),
                    get_type));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP6_CONNECTIVITY:
        connectivity = nm_device_get_connectivity(d, AF_INET6);
        return (*out_to_free = nmc_meta_generic_get_enum_with_detail(
                    NMC_META_GENERIC_GET_ENUM_TYPE_PARENTHESES,
                    connectivity,
                    nm_connectivity_to_string(connectivity),
                    get_type));
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_UDI:
        return nm_device_get_udi(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PATH:
        return nm_device_get_path(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP_IFACE:
        return nm_device_get_ip_iface(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IS_SOFTWARE:
        return nmc_meta_generic_get_bool(nm_device_is_software(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_MANAGED:
        return nmc_meta_generic_get_bool(nm_device_get_managed(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_AUTOCONNECT:
        return nmc_meta_generic_get_bool(nm_device_get_autoconnect(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_FIRMWARE_MISSING:
        return nmc_meta_generic_get_bool(nm_device_get_firmware_missing(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_PLUGIN_MISSING:
        return nmc_meta_generic_get_bool(nm_device_get_nm_plugin_missing(d), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PHYS_PORT_ID:
        return nm_device_get_physical_port_id(d);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CONNECTION:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_active_connection_get_id(ac) : NULL;
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CON_UUID:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_active_connection_get_uuid(ac) : NULL;
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CON_PATH:
        ac = nm_device_get_active_connection(d);
        return ac ? nm_object_get_path(NM_OBJECT(ac)) : NULL;
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_METERED:
        return nmc_meta_generic_get_str_i18n(nmc_device_metered_to_string(nm_device_get_metered(d)),
                                             get_type);
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo
    *const metagen_device_detail_general[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NUM + 1] = {
#define _METAGEN_DEVICE_DETAIL_GENERAL(type, name) \
    [type] = NMC_META_GENERIC(name,                \
                              .info_type = type,   \
                              .get_fcn   = _metagen_device_detail_general_get_fcn)
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DEVICE,
                                       "DEVICE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_TYPE, "TYPE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_TYPE,
                                       "NM-TYPE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DBUS_PATH,
                                       "DBUS-PATH"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_VENDOR,
                                       "VENDOR"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PRODUCT,
                                       "PRODUCT"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DRIVER,
                                       "DRIVER"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_DRIVER_VERSION,
                                       "DRIVER-VERSION"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_FIRMWARE_VERSION,
                                       "FIRMWARE-VERSION"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_HWADDR,
                                       "HWADDR"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_MTU, "MTU"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_STATE, "STATE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_REASON,
                                       "REASON"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP4_CONNECTIVITY,
                                       "IP4-CONNECTIVITY"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP6_CONNECTIVITY,
                                       "IP6-CONNECTIVITY"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_UDI, "UDI"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PATH, "PATH"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IP_IFACE,
                                       "IP-IFACE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_IS_SOFTWARE,
                                       "IS-SOFTWARE"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_MANAGED,
                                       "NM-MANAGED"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_AUTOCONNECT,
                                       "AUTOCONNECT"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_FIRMWARE_MISSING,
                                       "FIRMWARE-MISSING"),
        _METAGEN_DEVICE_DETAIL_GENERAL(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_NM_PLUGIN_MISSING,
            "NM-PLUGIN-MISSING"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_PHYS_PORT_ID,
                                       "PHYS-PORT-ID"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CONNECTION,
                                       "CONNECTION"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CON_UUID,
                                       "CON-UUID"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_CON_PATH,
                                       "CON-PATH"),
        _METAGEN_DEVICE_DETAIL_GENERAL(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_GENERAL_METERED,
                                       "METERED"),
};

/*****************************************************************************/

static NMRemoteConnection **
_device_get_available_connections(NMDevice *d, guint *out_len)
{
    NMRemoteConnection **avail_cons;
    const GPtrArray     *avail_cons_arr;

    avail_cons_arr = nm_device_get_available_connections(d);
    if (!avail_cons_arr || avail_cons_arr->len == 0) {
        *out_len = 0;
        return NULL;
    }

    avail_cons = (NMRemoteConnection **) nmc_objects_sort_by_path(
        (const NMObject *const *) avail_cons_arr->pdata,
        avail_cons_arr->len);
    nm_assert(avail_cons_arr->len == NM_PTRARRAY_LEN(avail_cons));
    *out_len = avail_cons_arr->len;
    return avail_cons;
}

static gconstpointer
_metagen_device_detail_connections_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice                    *d          = target;
    gs_free NMRemoteConnection **avail_cons = NULL;
    guint                        avail_cons_len;
    guint                        i;
    guint                        j;
    char                       **arr = NULL;
    GString                     *str;
    gboolean                     had_prefix;
    gboolean                     has_prefix;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CONNECTIONS_AVAILABLE_CONNECTIONS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;

        avail_cons = _device_get_available_connections(d, &avail_cons_len);
        if (avail_cons_len == 0)
            goto arr_out;

        arr = g_new(char *, avail_cons_len + 1);
        j   = 0;
        for (i = 0; i < avail_cons_len; i++) {
            NMRemoteConnection *ac      = avail_cons[i];
            const char         *ac_id   = nm_connection_get_id(NM_CONNECTION(ac));
            const char         *ac_uuid = nm_connection_get_uuid(NM_CONNECTION(ac));

            if (!ac_id || !ac_uuid) {
                const char *ac_path = nm_connection_get_path(NM_CONNECTION(ac));

                if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY) {
                    arr[j++] = ac_path ? g_strdup_printf(_("<invisible> | %s"), ac_path)
                                       : g_strdup(_("<invisible>"));
                } else {
                    arr[j++] = ac_path ? g_strdup_printf("<invisible> | %s", ac_path)
                                       : g_strdup("<invisible>");
                }
            } else
                arr[j++] = g_strdup_printf("%s | %s", ac_uuid, ac_id);
        }
        arr[j] = NULL;
        goto arr_out;

    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CONNECTIONS_AVAILABLE_CONNECTION_PATHS:

        avail_cons = _device_get_available_connections(d, &avail_cons_len);
        if (avail_cons_len == 0)
            return NULL;

        str = g_string_new(NULL);

        had_prefix = FALSE;
        for (i = 0; i < avail_cons_len; i++) {
            NMRemoteConnection *ac = avail_cons[i];
            const char         *p  = nm_connection_get_path(NM_CONNECTION(ac));

            if (!p)
                continue;

            has_prefix = g_str_has_prefix(p, NM_DBUS_PATH_SETTINGS_CONNECTION "/")
                         && p[NM_STRLEN(NM_DBUS_PATH_SETTINGS_CONNECTION "/")];

            if (str->len > 0) {
                if (had_prefix && !has_prefix)
                    g_string_append_c(str, '}');
                g_string_append_c(str, ',');
            }

            if (!has_prefix)
                g_string_append(str, p);
            else {
                if (!had_prefix)
                    g_string_printf(str, "%s/{", NM_DBUS_PATH_SETTINGS_CONNECTION);
                g_string_append(str, &p[NM_STRLEN(NM_DBUS_PATH_SETTINGS_CONNECTION "/")]);
            }
            had_prefix = has_prefix;
        }
        if (had_prefix)
            g_string_append_c(str, '}');

        return (*out_to_free = g_string_free(str, FALSE));

    default:
        break;
    }

    g_return_val_if_reached(NULL);

arr_out:
    NM_SET_OUT(out_is_default, !arr || !arr[0]);
    *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
    *out_to_free = arr;
    return arr;
}

const NmcMetaGenericInfo *const
    metagen_device_detail_connections[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CONNECTIONS_NUM + 1] = {
#define _METAGEN_DEVICE_DETAIL_CONNECTIONS(type, name) \
    [type] = NMC_META_GENERIC(name,                    \
                              .info_type = type,       \
                              .get_fcn   = _metagen_device_detail_connections_get_fcn)
        _METAGEN_DEVICE_DETAIL_CONNECTIONS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CONNECTIONS_AVAILABLE_CONNECTION_PATHS,
            "AVAILABLE-CONNECTION-PATHS"),
        _METAGEN_DEVICE_DETAIL_CONNECTIONS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CONNECTIONS_AVAILABLE_CONNECTIONS,
            "AVAILABLE-CONNECTIONS"),
};

/*****************************************************************************/

static gconstpointer
_metagen_device_detail_capabilities_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice            *d = target;
    NMDeviceCapabilities caps;
    guint32              speed;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    caps = nm_device_get_capabilities(d);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_CARRIER_DETECT:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(caps, NM_DEVICE_CAP_CARRIER_DETECT),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_SPEED:
        speed = 0;
        if (NM_IS_DEVICE_ETHERNET(d)) {
            /* Speed in Mb/s */
            speed = nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(d));
        } else if (NM_IS_DEVICE_WIFI(d)) {
            /* Speed in b/s */
            speed = nm_device_wifi_get_bitrate(NM_DEVICE_WIFI(d));
            speed /= 1000;
        }

        if (speed) {
            if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
                return (*out_to_free = g_strdup_printf(_("%u Mb/s"), (guint) speed));
            return (*out_to_free = g_strdup_printf("%u Mb/s", (guint) speed));
        }
        return nmc_meta_generic_get_str_i18n(N_("unknown"), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_IS_SOFTWARE:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(caps, NM_DEVICE_CAP_IS_SOFTWARE), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_SRIOV:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(caps, NM_DEVICE_CAP_SRIOV), get_type);
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo
    *const metagen_device_detail_capabilities[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_NUM
                                              + 1] = {
#define _METAGEN_DEVICE_DETAIL_CAPABILITIES(type, name) \
    [type] = NMC_META_GENERIC(name,                     \
                              .info_type = type,        \
                              .get_fcn   = _metagen_device_detail_capabilities_get_fcn)
        _METAGEN_DEVICE_DETAIL_CAPABILITIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_CARRIER_DETECT,
            "CARRIER-DETECT"),
        _METAGEN_DEVICE_DETAIL_CAPABILITIES(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_SPEED,
                                            "SPEED"),
        _METAGEN_DEVICE_DETAIL_CAPABILITIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_IS_SOFTWARE,
            "IS-SOFTWARE"),
        _METAGEN_DEVICE_DETAIL_CAPABILITIES(NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_CAPABILITIES_SRIOV,
                                            "SRIOV"),
};

/*****************************************************************************/

static gconstpointer
_metagen_device_detail_wired_properties_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice *d = target;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIRED_PROPERTIES_CARRIER:
        return nmc_meta_generic_get_bool_onoff(
            nm_device_ethernet_get_carrier(NM_DEVICE_ETHERNET(d)),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIRED_PROPERTIES_S390_SUBCHANNELS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
        return nm_device_ethernet_get_s390_subchannels(NM_DEVICE_ETHERNET(d));
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo *const
    metagen_device_detail_wired_properties[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIRED_PROPERTIES_NUM
                                           + 1] = {
#define _METAGEN_DEVICE_DETAIL_WIRED_PROPERTIES(type, name) \
    [type] = NMC_META_GENERIC(name,                         \
                              .info_type = type,            \
                              .get_fcn   = _metagen_device_detail_wired_properties_get_fcn)
        _METAGEN_DEVICE_DETAIL_WIRED_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIRED_PROPERTIES_CARRIER,
            "CARRIER"),
        _METAGEN_DEVICE_DETAIL_WIRED_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIRED_PROPERTIES_S390_SUBCHANNELS,
            "S390-SUBCHANNELS"),
};

/*****************************************************************************/

static gconstpointer
_metagen_device_detail_wifi_properties_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice                *d = target;
    NMDeviceWifiCapabilities wcaps;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    wcaps = nm_device_wifi_get_capabilities(NM_DEVICE_WIFI(d));

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WEP:
        return nmc_meta_generic_get_bool(
            NM_FLAGS_ANY(wcaps, NM_WIFI_DEVICE_CAP_CIPHER_WEP40 | NM_WIFI_DEVICE_CAP_CIPHER_WEP104),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WPA:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_WPA), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WPA2:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_RSN), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_TKIP:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_CIPHER_TKIP),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_CCMP:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_CIPHER_CCMP),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_AP:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_AP), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_ADHOC:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_ADHOC), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_2GHZ:
        return nmc_meta_generic_get_str_i18n(
            NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_VALID)
                ? (NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_2GHZ) ? N_("yes") : N_("no"))
                : N_("unknown"),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_5GHZ:
        return nmc_meta_generic_get_str_i18n(
            NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_VALID)
                ? (NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_5GHZ) ? N_("yes") : N_("no"))
                : N_("unknown"),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_6GHZ:
        return nmc_meta_generic_get_str_i18n(
            NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_VALID)
                ? (NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_FREQ_6GHZ) ? N_("yes") : N_("no"))
                : N_("unknown"),
            get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_MESH:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_MESH), get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_IBSS_RSN:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(wcaps, NM_WIFI_DEVICE_CAP_IBSS_RSN),
                                         get_type);
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo *const
    metagen_device_detail_wifi_properties[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_NUM
                                          + 1] = {
#define _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(type, name) \
    [type] = NMC_META_GENERIC(name,                        \
                              .info_type = type,           \
                              .get_fcn   = _metagen_device_detail_wifi_properties_get_fcn)
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WEP,
            "WEP"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WPA,
            "WPA"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_WPA2,
            "WPA2"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_TKIP,
            "TKIP"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_CCMP,
            "CCMP"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_AP,
            "AP"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_ADHOC,
            "ADHOC"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_2GHZ,
            "2GHZ"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_5GHZ,
            "5GHZ"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_6GHZ,
            "6GHZ"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_MESH,
            "MESH"),
        _METAGEN_DEVICE_DETAIL_WIFI_PROPERTIES(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_WIFI_PROPERTIES_IBSS_RSN,
            "IBSS-RSN"),
};

/*****************************************************************************/

static gconstpointer
_metagen_device_detail_interface_flags_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDevice              *d = target;
    NMDeviceInterfaceFlags flags;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    flags = nm_device_get_interface_flags(d);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_UP:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(flags, NM_DEVICE_INTERFACE_FLAG_UP),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_LOWER_UP:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(flags, NM_DEVICE_INTERFACE_FLAG_LOWER_UP),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_CARRIER:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(flags, NM_DEVICE_INTERFACE_FLAG_CARRIER),
                                         get_type);
    case NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_PROMISC:
        return nmc_meta_generic_get_bool(NM_FLAGS_HAS(flags, NM_DEVICE_INTERFACE_FLAG_PROMISC),
                                         get_type);
    default:
        break;
    }

    g_return_val_if_reached(NULL);
}

const NmcMetaGenericInfo *const
    metagen_device_detail_interface_flags[_NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_NUM
                                          + 1] = {
#define _METAGEN_DEVICE_DETAIL_INTERFACE_FLAGS(type, name) \
    [type] = NMC_META_GENERIC(name,                        \
                              .info_type = type,           \
                              .get_fcn   = _metagen_device_detail_interface_flags_get_fcn)
        _METAGEN_DEVICE_DETAIL_INTERFACE_FLAGS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_UP,
            "UP"),
        _METAGEN_DEVICE_DETAIL_INTERFACE_FLAGS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_LOWER_UP,
            "LOWER-UP"),
        _METAGEN_DEVICE_DETAIL_INTERFACE_FLAGS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_CARRIER,
            "CARRIER"),
        _METAGEN_DEVICE_DETAIL_INTERFACE_FLAGS(
            NMC_GENERIC_INFO_TYPE_DEVICE_DETAIL_INTERFACE_FLAGS_PROMISC,
            "PROMISC"),
};

/*****************************************************************************/

const NmcMetaGenericInfo *const metagen_device_detail_wimax_properties[] = {
    NMC_META_GENERIC("CTR-FREQ"),
    NMC_META_GENERIC("RSSI"),
    NMC_META_GENERIC("CINR"),
    NMC_META_GENERIC("TX-POW"),
    NMC_META_GENERIC("BSID"),
};

/*****************************************************************************/

const NmcMetaGenericInfo *const nmc_fields_dev_wifi_list[] = {
    NMC_META_GENERIC("NAME"),      /* 0 */
    NMC_META_GENERIC("SSID"),      /* 1 */
    NMC_META_GENERIC("SSID-HEX"),  /* 2 */
    NMC_META_GENERIC("BSSID"),     /* 3 */
    NMC_META_GENERIC("MODE"),      /* 4 */
    NMC_META_GENERIC("CHAN"),      /* 5 */
    NMC_META_GENERIC("FREQ"),      /* 6 */
    NMC_META_GENERIC("RATE"),      /* 7 */
    NMC_META_GENERIC("BANDWIDTH"), /* 8 */
    NMC_META_GENERIC("SIGNAL"),    /* 9 */
    NMC_META_GENERIC("BARS"),      /* 10 */
    NMC_META_GENERIC("SECURITY"),  /* 11 */
    NMC_META_GENERIC("WPA-FLAGS"), /* 12 */
    NMC_META_GENERIC("RSN-FLAGS"), /* 13 */
    NMC_META_GENERIC("DEVICE"),    /* 14 */
    NMC_META_GENERIC("ACTIVE"),    /* 15 */
    NMC_META_GENERIC("IN-USE"),    /* 16 */
    NMC_META_GENERIC("DBUS-PATH"), /* 17 */
    NULL,
};
#define NMC_FIELDS_DEV_WIFI_LIST_COMMON       "IN-USE,BSSID,SSID,MODE,CHAN,RATE,SIGNAL,BARS,SECURITY"
#define NMC_FIELDS_DEV_WIFI_LIST_FOR_DEV_LIST "NAME," NMC_FIELDS_DEV_WIFI_LIST_COMMON

const NmcMetaGenericInfo *const nmc_fields_dev_wimax_list[] = {
    NMC_META_GENERIC("NAME"),      /* 0 */
    NMC_META_GENERIC("NSP"),       /* 1 */
    NMC_META_GENERIC("SIGNAL"),    /* 2 */
    NMC_META_GENERIC("TYPE"),      /* 3 */
    NMC_META_GENERIC("DEVICE"),    /* 4 */
    NMC_META_GENERIC("ACTIVE"),    /* 5 */
    NMC_META_GENERIC("DBUS-PATH"), /* 6 */
    NULL,
};
#define NMC_FIELDS_DEV_WIMAX_LIST_COMMON       "NSP,SIGNAL,TYPE,DEVICE,ACTIVE"
#define NMC_FIELDS_DEV_WIMAX_LIST_FOR_DEV_LIST "NAME," NMC_FIELDS_DEV_WIMAX_LIST_COMMON

const NmcMetaGenericInfo *const nmc_fields_dev_show_controller_prop[] = {
    NMC_META_GENERIC("NAME"),   /* 0 */
    NMC_META_GENERIC("SLAVES"), /* 1 */
    NULL,
};
#define NMC_FIELDS_DEV_SHOW_CONTROLLER_PROP_COMMON "NAME,SLAVES"

const NmcMetaGenericInfo *const nmc_fields_dev_show_team_prop[] = {
    NMC_META_GENERIC("NAME"),   /* 0 */
    NMC_META_GENERIC("SLAVES"), /* 1 */
    NMC_META_GENERIC("CONFIG"), /* 2 */
    NULL,
};
#define NMC_FIELDS_DEV_SHOW_TEAM_PROP_COMMON "NAME,SLAVES,CONFIG"

const NmcMetaGenericInfo *const nmc_fields_dev_show_vlan_prop[] = {
    NMC_META_GENERIC("NAME"),   /* 0 */
    NMC_META_GENERIC("PARENT"), /* 1 */
    NMC_META_GENERIC("ID"),     /* 2 */
    NULL,
};
#define NMC_FIELDS_DEV_SHOW_VLAN_PROP_COMMON "NAME,PARENT,ID"

const NmcMetaGenericInfo *const nmc_fields_dev_show_bluetooth[] = {
    NMC_META_GENERIC("NAME"),         /* 0 */
    NMC_META_GENERIC("CAPABILITIES"), /* 1 */
    NULL,
};
#define NMC_FIELDS_DEV_SHOW_BLUETOOTH_COMMON "NAME,CAPABILITIES"

/* Available sections for 'device show' */
const NmcMetaGenericInfo *const nmc_fields_dev_show_sections[] = {
    NMC_META_GENERIC_WITH_NESTED("GENERAL", metagen_device_detail_general),                 /* 0 */
    NMC_META_GENERIC_WITH_NESTED("CAPABILITIES", metagen_device_detail_capabilities),       /* 1 */
    NMC_META_GENERIC_WITH_NESTED("INTERFACE-FLAGS", metagen_device_detail_interface_flags), /* 2 */
    NMC_META_GENERIC_WITH_NESTED("WIFI-PROPERTIES", metagen_device_detail_wifi_properties), /* 3 */
    NMC_META_GENERIC_WITH_NESTED("AP", nmc_fields_dev_wifi_list + 1),                       /* 4 */
    NMC_META_GENERIC_WITH_NESTED("WIRED-PROPERTIES",
                                 metagen_device_detail_wired_properties), /* 5 */
    NMC_META_GENERIC_WITH_NESTED("WIMAX-PROPERTIES",
                                 metagen_device_detail_wimax_properties),            /* 6 */
    NMC_META_GENERIC_WITH_NESTED("NSP", nmc_fields_dev_wimax_list + 1),              /* 7 */
    NMC_META_GENERIC_WITH_NESTED("IP4", metagen_ip4_config),                         /* 8 */
    NMC_META_GENERIC_WITH_NESTED("DHCP4", metagen_dhcp_config),                      /* 9 */
    NMC_META_GENERIC_WITH_NESTED("IP6", metagen_ip6_config),                         /* 10 */
    NMC_META_GENERIC_WITH_NESTED("DHCP6", metagen_dhcp_config),                      /* 11 */
    NMC_META_GENERIC_WITH_NESTED("BOND", nmc_fields_dev_show_controller_prop + 1),   /* 12 */
    NMC_META_GENERIC_WITH_NESTED("TEAM", nmc_fields_dev_show_team_prop + 1),         /* 13 */
    NMC_META_GENERIC_WITH_NESTED("BRIDGE", nmc_fields_dev_show_controller_prop + 1), /* 14 */
    NMC_META_GENERIC_WITH_NESTED("VLAN", nmc_fields_dev_show_vlan_prop + 1),         /* 15 */
    NMC_META_GENERIC_WITH_NESTED("BLUETOOTH", nmc_fields_dev_show_bluetooth + 1),    /* 16 */
    NMC_META_GENERIC_WITH_NESTED("CONNECTIONS", metagen_device_detail_connections),  /* 17 */
    NULL,
};
#define NMC_FIELDS_DEV_SHOW_SECTIONS_COMMON                                 \
    "GENERAL.DEVICE,GENERAL.TYPE,GENERAL.HWADDR,GENERAL.MTU,GENERAL.STATE," \
    "GENERAL.CONNECTION,GENERAL.CON-PATH,WIRED-PROPERTIES,IP4,IP6"

const NmcMetaGenericInfo *const nmc_fields_dev_lldp_list[] = {
    NMC_META_GENERIC("NAME"),                   /* 0 */
    NMC_META_GENERIC("DEVICE"),                 /* 1 */
    NMC_META_GENERIC("CHASSIS-ID"),             /* 2 */
    NMC_META_GENERIC("PORT-ID"),                /* 3 */
    NMC_META_GENERIC("PORT-DESCRIPTION"),       /* 4 */
    NMC_META_GENERIC("SYSTEM-NAME"),            /* 5 */
    NMC_META_GENERIC("SYSTEM-DESCRIPTION"),     /* 6 */
    NMC_META_GENERIC("SYSTEM-CAPABILITIES"),    /* 7 */
    NMC_META_GENERIC("IEEE-802-1-PVID"),        /* 8 */
    NMC_META_GENERIC("IEEE-802-1-PPVID"),       /* 9 */
    NMC_META_GENERIC("IEEE-802-1-PPVID-FLAGS"), /* 10 */
    NMC_META_GENERIC("IEEE-802-1-VID"),         /* 11 */
    NMC_META_GENERIC("IEEE-802-1-VLAN-NAME"),   /* 12 */
    NMC_META_GENERIC("DESTINATION"),            /* 13 */
    NMC_META_GENERIC("CHASSIS-ID-TYPE"),        /* 14 */
    NMC_META_GENERIC("PORT-ID-TYPE"),           /* 15 */
    NULL,
};
#define NMC_FIELDS_DEV_LLDP_LIST_COMMON                                          \
    "DEVICE,CHASSIS-ID,PORT-ID,PORT-DESCRIPTION,SYSTEM-NAME,SYSTEM-DESCRIPTION," \
    "SYSTEM-CAPABILITIES"

static guint progress_id = 0; /* ID of event source for displaying progress */

static void
usage(void)
{
    nmc_printerr(
        _("Usage: nmcli device { COMMAND | help }\n\n"
          "COMMAND := { status | show | set | connect | reapply | modify | disconnect | "
          "delete | monitor | wifi | lldp }\n\n"
          "  status\n\n"
          "  show [<ifname>]\n\n"
          "  set [ifname] <ifname> [autoconnect yes|no] [managed yes|no]\n\n"
          "  connect <ifname>\n\n"
          "  reapply <ifname>\n\n"
          "  modify <ifname> ([+|-]<setting>.<property> <value>)+\n\n"
          "  disconnect <ifname> ...\n\n"
          "  delete <ifname> ...\n\n"
          "  monitor <ifname> ...\n\n"
          "  wifi [list [ifname <ifname>] [bssid <BSSID>] [--rescan yes|no|auto]]\n\n"
          "  wifi connect <(B)SSID> [password <password>] [wep-key-type key|phrase] [ifname "
          "<ifname>]\n"
          "                         [bssid <BSSID>] [name <name>] [private yes|no] [hidden "
          "yes|no]\n\n"
          "  wifi hotspot [ifname <ifname>] [con-name <name>] [ssid <SSID>] [band a|bg] "
          "[channel <channel>] [password <password>]\n\n"
          "  wifi rescan [ifname <ifname>] [[ssid <SSID to scan>] ...]\n\n"
          "  wifi show-password [ifname <ifname>]\n\n"
          "  lldp [list [ifname <ifname>]]\n\n"));
}

static void
usage_device_status(void)
{
    nmc_printerr(
        _("Usage: nmcli device status { help }\n"
          "\n"
          "Show status for all devices.\n"
          "By default, the following columns are shown:\n"
          " DEVICE     - interface name\n"
          " TYPE       - device type\n"
          " STATE      - device state\n"
          " CONNECTION - connection activated on device (if any)\n"
          "Displayed columns can be changed using '--fields' global option. 'status' is\n"
          "the default command, which means 'nmcli device' calls 'nmcli device status'.\n\n"));
}

static void
usage_device_show(void)
{
    nmc_printerr(_("Usage: nmcli device show { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := [<ifname>]\n"
                   "\n"
                   "Show details of device(s).\n"
                   "The command lists details for all devices, or for a given device.\n\n"));
}

static void
usage_device_connect(void)
{
    nmc_printerr(_("Usage: nmcli device connect { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := <ifname>\n"
                   "\n"
                   "Connect the device.\n"
                   "NetworkManager will try to find a suitable connection that will be activated.\n"
                   "It will also consider connections that are not set to auto-connect.\n\n"));
}

static void
usage_device_reapply(void)
{
    nmc_printerr(_("Usage: nmcli device reapply { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := <ifname>\n"
                   "\n"
                   "Attempts to update device with changes to the currently active connection\n"
                   "made since it was last applied.\n\n"));
}

static void
usage_device_modify(void)
{
    nmc_printerr(_(
        "Usage: nmcli device modify { ARGUMENTS | help }\n"
        "\n"
        "ARGUMENTS := <ifname> ([+|-]<setting>.<property> <value>)+\n"
        "\n"
        "Modify one or more properties that are currently active on the device without modifying\n"
        "the connection profile. The changes have immediate effect. For multi-valued\n"
        "properties you can use optional '+' or '-' prefix to the property name.\n"
        "The '+' sign allows appending items instead of overwriting the whole value.\n"
        "The '-' sign allows removing selected items instead of the whole value.\n"
        "\n"
        "Examples:\n"
        "nmcli dev mod em1 ipv4.method manual ipv4.addr \"192.168.1.2/24, 10.10.1.5/8\"\n"
        "nmcli dev mod em1 +ipv4.dns 8.8.4.4\n"
        "nmcli dev mod em1 -ipv4.dns 1\n"
        "nmcli dev mod em1 -ipv6.addr \"abbe::cafe/56\"\n"));
}

static void
usage_device_disconnect(void)
{
    nmc_printerr(_("Usage: nmcli device disconnect { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := <ifname> ...\n"
                   "\n"
                   "Disconnect devices.\n"
                   "The command disconnects the device and prevents it from auto-activating\n"
                   "further connections without user/manual intervention.\n\n"));
}

static void
usage_device_delete(void)
{
    nmc_printerr(_("Usage: nmcli device delete { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := <ifname> ...\n"
                   "\n"
                   "Delete the software devices.\n"
                   "The command removes the interfaces. It only works for software devices\n"
                   "(like bonds, bridges, etc.). Hardware devices cannot be deleted by the\n"
                   "command.\n\n"));
}

static void
usage_device_set(void)
{
    nmc_printerr(_("Usage: nmcli device set { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := DEVICE { PROPERTY [ PROPERTY ... ] }\n"
                   "DEVICE    := [ifname] <ifname> \n"
                   "PROPERTY  := { autoconnect { yes | no } |\n"
                   "             { managed { yes | no }\n"
                   "\n"
                   "Modify device properties.\n\n"));
}

static void
usage_device_monitor(void)
{
    nmc_printerr(_("Usage: nmcli device monitor { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := [<ifname>] ...\n"
                   "\n"
                   "Monitor device activity.\n"
                   "This command prints a line whenever the specified devices change state.\n"
                   "Monitors all devices in case no interface is specified.\n\n"));
}

static void
usage_device_wifi(void)
{
    nmc_printerr(
        _("Usage: nmcli device wifi { ARGUMENTS | help }\n"
          "\n"
          "Perform operation on Wi-Fi devices.\n"
          "\n"
          "ARGUMENTS := [list [ifname <ifname>] [bssid <BSSID>] [--rescan yes|no|auto]]\n"
          "\n"
          "List available Wi-Fi access points. The 'ifname' and 'bssid' options can be\n"
          "used to list APs for a particular interface, or with a specific BSSID. The\n"
          "--rescan flag tells whether a new Wi-Fi scan should be triggered.\n"
          "\n"
          "ARGUMENTS := connect <(B)SSID> [password <password>] [wep-key-type key|phrase] [ifname "
          "<ifname>]\n"
          "                     [bssid <BSSID>] [name <name>] [private yes|no] [hidden yes|no]\n"
          "\n"
          "Connect to a Wi-Fi network specified by SSID or BSSID. The command finds a\n"
          "matching connection or creates one and then activates it on a device. This\n"
          "is a command-line counterpart of clicking an SSID in a GUI client. If a\n"
          "connection for the network already exists, it is possible to bring up the\n"
          "existing profile as follows: nmcli con up id <name>. Note that only open,\n"
          "WEP and WPA-PSK networks are supported if no previous connection exists.\n"
          "It is also assumed that IP configuration is obtained via DHCP.\n"
          "\n"
          "ARGUMENTS := hotspot [ifname <ifname>] [con-name <name>] [ssid <SSID>]\n"
          "                     [band a|bg] [channel <channel>] [password <password>]\n"
          "\n"
          "Create a Wi-Fi hotspot. Use 'connection down' or 'device disconnect'\n"
          "to stop the hotspot.\n"
          "Parameters of the hotspot can be influenced by the optional parameters:\n"
          "ifname - Wi-Fi device to use\n"
          "con-name - name of the created hotspot connection profile\n"
          "ssid - SSID of the hotspot\n"
          "band - Wi-Fi band to use\n"
          "channel - Wi-Fi channel to use\n"
          "password - password to use for the hotspot\n"
          "\n"
          "ARGUMENTS := rescan [ifname <ifname>] [[ssid <SSID to scan>] ...]\n"
          "\n"
          "Request that NetworkManager immediately re-scan for available access points.\n"
          "NetworkManager scans Wi-Fi networks periodically, but in some cases it might\n"
          "be useful to start scanning manually. 'ssid' allows scanning for a specific\n"
          "SSID, which is useful for APs with hidden SSIDs. More 'ssid' parameters can be\n"
          "given. Note that this command does not show the APs,\n"
          "use 'nmcli device wifi list' for that.\n\n"));
}

static void
usage_device_lldp(void)
{
    nmc_printerr(_("Usage: nmcli device lldp { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := [list [ifname <ifname>]]\n"
                   "\n"
                   "List neighboring devices discovered through LLDP. The 'ifname' option can be\n"
                   "used to list neighbors for a particular interface.\n\n"));
}

static void
usage_device_checkpoint(void)
{
    nmc_printerr(_("Usage: nmcli device checkpoint { ARGUMENTS | help }\n"
                   "\n"
                   "ARGUMENTS := [--timeout <seconds>] -- COMMAND...\n"
                   "\n"
                   "Runs the command with a configuration checkpoint taken and asks for a\n"
                   "confirmation when finished. When the confirmation is not given, the\n"
                   "checkpoint is automatically restored after timeout.\n\n"));
}

static void
quit(void)
{
    if (nm_clear_g_source(&progress_id))
        nmc_terminal_erase_line();
    g_main_loop_quit(loop);
}

static int
compare_devices(const void *a, const void *b)
{
    NMDevice           *da    = *(NMDevice **) a;
    NMDevice           *db    = *(NMDevice **) b;
    NMActiveConnection *da_ac = nm_device_get_active_connection(da);
    NMActiveConnection *db_ac = nm_device_get_active_connection(db);

    NM_CMP_RETURN(nmc_active_connection_cmp(db_ac, da_ac));
    NM_CMP_DIRECT(nm_device_get_state(db), nm_device_get_state(da));
    NM_CMP_DIRECT_STRCMP0(nm_device_get_type_description(da), nm_device_get_type_description(db));
    NM_CMP_DIRECT_STRCMP0(nm_device_get_iface(da), nm_device_get_iface(db));
    NM_CMP_DIRECT_STRCMP0(nm_object_get_path(NM_OBJECT(da)), nm_object_get_path(NM_OBJECT(db)));

    g_return_val_if_reached(0);
}

NMDevice **
nmc_get_devices_sorted(NMClient *client)
{
    const GPtrArray *devs;
    NMDevice       **sorted;

    devs = nm_client_get_devices(client);

    sorted = g_new(NMDevice *, devs->len + 1);
    if (devs->len > 0)
        memcpy(sorted, devs->pdata, devs->len * sizeof(NMDevice *));
    sorted[devs->len] = NULL;

    qsort(sorted, devs->len, sizeof(NMDevice *), compare_devices);
    return sorted;
}

static void
complete_device(NMDevice **devices, const char *prefix, gboolean wifi_only)
{
    int i;

    for (i = 0; devices[i]; i++) {
        const char *iface = nm_device_get_iface(devices[i]);

        if (wifi_only && !NM_IS_DEVICE_WIFI(devices[i]))
            continue;

        if (g_str_has_prefix(iface, prefix))
            nmc_print("%s\n", iface);
    }
}

void
nmc_complete_device(NMClient *client, const char *prefix, gboolean wifi_only)
{
    gs_free NMDevice **devices = NULL;

    devices = nmc_get_devices_sorted(client);
    complete_device(devices, prefix, wifi_only);
}

static void destroy_queue_element(gpointer data);

static GPtrArray *
get_device_list(NmCli *nmc, int *argc, const char *const **argv)
{
    int                arg_num;
    const char *const *arg_ptr;

    gs_strfreev char **arg_arr = NULL;
    NMDevice         **devices;
    GPtrArray         *queue = NULL;
    NMDevice          *device;
    int                i;

    if (*argc == 0 && nmc->ask) {
        gs_free char *line = NULL;

        line = nmc_readline(&nmc->nmc_config, PROMPT_INTERFACES);
        nmc_string_to_arg_array(line, NULL, FALSE, &arg_arr, &arg_num);
        arg_ptr = (const char *const *) arg_arr;

        argc = &arg_num;
        argv = &arg_ptr;
    }

    if (*argc == 0) {
        g_string_printf(nmc->return_text, _("Error: No interface specified."));
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return NULL;
    }

    devices = nmc_get_devices_sorted(nmc->client);
    while (*argc > 0) {
        if (strcmp(**argv, "--") == 0) {
            (*argc)--;
            (*argv)++;
            break;
        }

        if (*argc == 1 && nmc->complete)
            complete_device(devices, **argv, FALSE);

        device = NULL;
        for (i = 0; devices[i]; i++) {
            if (!g_strcmp0(nm_device_get_iface(devices[i]), **argv)) {
                device = devices[i];
                break;
            }
        }

        if (device) {
            if (!queue)
                queue = g_ptr_array_new_with_free_func(destroy_queue_element);
            if (!g_ptr_array_find(queue, device, NULL))
                g_ptr_array_add(queue, g_object_ref(device));
            else
                nmc_printerr(_("Warning: argument '%s' is duplicated.\n"), **argv);
        } else {
            if (!nmc->complete)
                nmc_printerr(_("Error: Device '%s' not found.\n"), **argv);
            g_string_printf(nmc->return_text, _("Error: not all devices found."));
            nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        }

        /* Take next argument */
        next_arg(nmc->ask ? NULL : nmc, argc, argv, NULL);
    }
    g_free(devices);

    return queue;
}

static NMDevice *
get_device(NmCli *nmc, int *argc, const char *const **argv, GError **error)
{
    gs_free NMDevice **devices    = NULL;
    gs_free char      *ifname_ask = NULL;
    const char        *ifname     = NULL;
    int                i;

    if (*argc == 0) {
        if (nmc->ask) {
            ifname = ifname_ask = nmc_readline(&nmc->nmc_config, PROMPT_INTERFACE);
        }

        if (!ifname_ask) {
            g_set_error_literal(error,
                                NMCLI_ERROR,
                                NMC_RESULT_ERROR_USER_INPUT,
                                _("No interface specified"));
            return NULL;
        }
    } else {
        ifname = **argv;
        next_arg(nmc, argc, argv, NULL);
    }

    devices = nmc_get_devices_sorted(nmc->client);
    for (i = 0; devices[i]; i++) {
        if (!g_strcmp0(nm_device_get_iface(devices[i]), ifname))
            break;
    }

    if (nmc->complete && !*argc)
        complete_device(devices, ifname, FALSE);

    if (devices[i] == NULL) {
        g_set_error(error,
                    NMCLI_ERROR,
                    NMC_RESULT_ERROR_NOT_FOUND,
                    _("Device '%s' not found"),
                    ifname);
    }

    return devices[i];
}

static bool
_ap_is_wep(NMAccessPoint *ap)
{
    NM80211ApFlags         flags     = nm_access_point_get_flags(ap);
    NM80211ApSecurityFlags wpa_flags = nm_access_point_get_wpa_flags(ap);
    NM80211ApSecurityFlags rsn_flags = nm_access_point_get_rsn_flags(ap);

    if ((flags & NM_802_11_AP_FLAGS_PRIVACY) && (wpa_flags == NM_802_11_AP_SEC_NONE)
        && (rsn_flags == NM_802_11_AP_SEC_NONE)) {
        return TRUE;
    }

    return FALSE;
}

static int
compare_aps(gconstpointer a, gconstpointer b, gpointer user_data)
{
    NMAccessPoint *apa = *(NMAccessPoint **) a;
    NMAccessPoint *apb = *(NMAccessPoint **) b;

    /* Sort the deprecated WEP connections last. */
    NM_CMP_DIRECT(_ap_is_wep(apa), _ap_is_wep(apb));

    NM_CMP_DIRECT(nm_access_point_get_strength(apb), nm_access_point_get_strength(apa));
    NM_CMP_DIRECT(nm_access_point_get_frequency(apa), nm_access_point_get_frequency(apb));
    NM_CMP_DIRECT(nm_access_point_get_max_bitrate(apb), nm_access_point_get_max_bitrate(apa));

    /* as fallback, just give it some stable order and use the D-Bus path (literally). */
    NM_CMP_DIRECT_STRCMP0(nm_object_get_path(NM_OBJECT(apa)), nm_object_get_path(NM_OBJECT(apb)));

    return 0;
}

static GPtrArray *
sort_access_points(const GPtrArray *aps)
{
    GPtrArray *sorted;
    guint      i;

    g_return_val_if_fail(aps, NULL);

    sorted = g_ptr_array_sized_new(aps->len);
    g_ptr_array_set_free_func(sorted, nm_g_object_unref);
    for (i = 0; i < aps->len; i++)
        g_ptr_array_add(sorted, g_object_ref(aps->pdata[i]));
    g_ptr_array_sort_with_data(sorted, compare_aps, NULL);
    return sorted;
}

typedef struct {
    NmCli         *nmc;
    NMAccessPoint *active_ap;
    const char    *device;
    GPtrArray     *output_data;
    int           *p_index;
    guint32        output_flags;
} APInfo;

static void
fill_output_access_point(NMAccessPoint *ap, const APInfo *info)
{
    NmcOutputField        *arr;
    gboolean               active;
    NM80211ApSecurityFlags wpa_flags, rsn_flags;
    guint32                freq;
    guint32                bitrate;
    guint32                bandwidth;
    guint8                 strength;
    GBytes                *ssid;
    const char            *bssid;
    NM80211Mode            mode;
    char                  *channel_str;
    char                  *freq_str;
    char                  *ssid_str     = NULL;
    char                  *ssid_hex_str = NULL;
    char                  *bitrate_str;
    char                  *bandwidth_str;
    char                  *strength_str;
    char                  *wpa_flags_str;
    char                  *rsn_flags_str;
    GString               *security_str;
    char                  *ap_name;
    const char            *sig_bars;
    NMMetaColor            color;

    active = (info->active_ap == ap);

    /* Get AP properties */
    wpa_flags = nm_access_point_get_wpa_flags(ap);
    rsn_flags = nm_access_point_get_rsn_flags(ap);
    ssid      = nm_access_point_get_ssid(ap);
    bssid     = nm_access_point_get_bssid(ap);
    freq      = nm_access_point_get_frequency(ap);
    mode      = nm_access_point_get_mode(ap);
    bitrate   = nm_access_point_get_max_bitrate(ap);
    bandwidth = nm_access_point_get_bandwidth(ap);
    strength  = NM_MIN(nm_access_point_get_strength(ap), 100u);

    /* Convert to strings */
    if (ssid) {
        const guint8 *ssid_data;
        gsize         ssid_len;

        ssid_data    = g_bytes_get_data(ssid, &ssid_len);
        ssid_str     = nm_utils_ssid_to_utf8(ssid_data, ssid_len);
        ssid_hex_str = ssid_to_hex((const char *) ssid_data, ssid_len);
    }
    channel_str   = g_strdup_printf("%u", nm_utils_wifi_freq_to_channel(freq));
    freq_str      = g_strdup_printf(_("%u MHz"), freq);
    bitrate_str   = g_strdup_printf(_("%u Mbit/s"), bitrate / 1000);
    bandwidth_str = g_strdup_printf(_("%u MHz"), bandwidth);
    strength_str  = nm_strdup_int(strength);
    wpa_flags_str = ap_wpa_rsn_flags_to_string(wpa_flags, NM_META_ACCESSOR_GET_TYPE_PRETTY);
    rsn_flags_str = ap_wpa_rsn_flags_to_string(rsn_flags, NM_META_ACCESSOR_GET_TYPE_PRETTY);
    sig_bars      = nmc_wifi_strength_bars(strength);

    security_str = g_string_new(NULL);

    if (_ap_is_wep(ap)) {
        g_string_append(security_str, "WEP ");
    } else {
        if (wpa_flags != NM_802_11_AP_SEC_NONE) {
            g_string_append(security_str, "WPA1 ");
        }
        if ((rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK)
            || (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X)) {
            g_string_append(security_str, "WPA2 ");
        }
        if (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_SAE) {
            g_string_append(security_str, "WPA3 ");
        }
        if (NM_FLAGS_ANY(rsn_flags, NM_802_11_AP_SEC_KEY_MGMT_OWE)) {
            g_string_append(security_str, "OWE ");
        } else if (NM_FLAGS_ANY(rsn_flags, NM_802_11_AP_SEC_KEY_MGMT_OWE_TM)) {
            g_string_append(security_str, "OWE-TM ");
        }
        if ((wpa_flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X)
            || (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X)) {
            g_string_append(security_str, "802.1X ");
        }
        if ((wpa_flags & NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)
            || (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)) {
            g_string_append(security_str, "WPA-EAP-SUITE-B-192 ");
        }
    }

    if (security_str->len > 0)
        g_string_truncate(security_str, security_str->len - 1); /* Chop off last space */

    arr = nmc_dup_fields_array((const NMMetaAbstractInfo *const *) nmc_fields_dev_wifi_list,
                               info->output_flags);

    ap_name = g_strdup_printf("AP[%d]", ++(*info->p_index)); /* AP */
    set_val_str(arr, 0, ap_name);
    set_val_str(arr, 1, ssid_str);
    set_val_str(arr, 2, ssid_hex_str);
    set_val_strc(arr, 3, bssid);
    set_val_strc(arr,
                 4,
                 mode == NM_802_11_MODE_ADHOC   ? _("Ad-Hoc")
                 : mode == NM_802_11_MODE_INFRA ? _("Infra")
                 : mode == NM_802_11_MODE_MESH  ? _("Mesh")
                                                : _("N/A"));
    set_val_str(arr, 5, channel_str);
    set_val_str(arr, 6, freq_str);
    set_val_str(arr, 7, bitrate_str);
    set_val_str(arr, 8, bandwidth_str);
    set_val_str(arr, 9, strength_str);
    set_val_strc(arr, 10, sig_bars);
    set_val_str(arr, 11, g_string_free(security_str, FALSE));
    set_val_str(arr, 12, wpa_flags_str);
    set_val_str(arr, 13, rsn_flags_str);
    set_val_strc(arr, 14, info->device);
    set_val_strc(arr, 15, active ? _("yes") : _("no"));
    set_val_strc(arr, 16, active ? "*" : " ");
    set_val_strc(arr, 17, nm_object_get_path(NM_OBJECT(ap)));

    /* Set colors */
    color = wifi_signal_to_color(strength);
    if (_ap_is_wep(ap))
        color = NM_META_COLOR_WIFI_DEPRECATED;
    set_val_color_all(arr, color);
    if (active)
        arr[15].color = NM_META_COLOR_CONNECTION_ACTIVATED;

    g_ptr_array_add(info->output_data, arr);
}

static void
fill_output_access_point_void(gpointer data, gpointer user_data)
{
    fill_output_access_point(data, user_data);
}

static char *
bluetooth_caps_to_string(NMBluetoothCapabilities caps)
{
    char *caps_str[8]; /* Enough space for caps and terminating NULL */
    char *ret_str;
    int   i = 0;

    if (caps & NM_BT_CAPABILITY_DUN)
        caps_str[i++] = g_strdup("DUN");
    if (caps & NM_BT_CAPABILITY_NAP)
        caps_str[i++] = g_strdup("NAP");

    if (i == 0)
        caps_str[i++] = g_strdup(_("(none)"));

    caps_str[i] = NULL;

    ret_str = g_strjoinv(" ", caps_str);

    i = 0;
    while (caps_str[i])
        g_free(caps_str[i++]);

    return ret_str;
}

static char *
construct_header_name(const char *base, const char *spec)
{
    if (spec == NULL)
        return g_strdup(base);

    return g_strdup_printf("%s (%s)", base, spec);
}

static gboolean
print_bond_bridge_info(NMDevice   *device,
                       NmCli      *nmc,
                       const char *group_prefix,
                       const char *one_field)
{
    const GPtrArray                 *ports = NULL;
    GString                         *ports_str;
    int                              idx;
    const NMMetaAbstractInfo *const *tmpl;
    NmcOutputField                  *arr;
    NMC_OUTPUT_DATA_DEFINE_SCOPED(out);

    if (NM_IS_DEVICE_BOND(device) || NM_IS_DEVICE_BRIDGE(device))
        ports = nm_device_get_ports(device);
    else
        g_return_val_if_reached(FALSE);

    ports_str = g_string_new(NULL);
    for (idx = 0; ports && idx < ports->len; idx++) {
        NMDevice   *port  = g_ptr_array_index(ports, idx);
        const char *iface = nm_device_get_iface(port);

        if (iface) {
            g_string_append(ports_str, iface);
            g_string_append_c(ports_str, ' ');
        }
    }
    if (ports_str->len > 0)
        g_string_truncate(ports_str, ports_str->len - 1); /* Chop off last space */

    tmpl        = (const NMMetaAbstractInfo *const *) nmc_fields_dev_show_controller_prop;
    out_indices = parse_output_fields(one_field, tmpl, FALSE, NULL, NULL);
    arr         = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_FIELD_NAMES);
    g_ptr_array_add(out.output_data, arr);

    arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_SECTION_PREFIX);
    set_val_strc(arr, 0, group_prefix); /* i.e. BOND, TEAM, BRIDGE */
    set_val_str(arr, 1, g_string_free(ports_str, FALSE));
    g_ptr_array_add(out.output_data, arr);

    print_data_prepare_width(out.output_data);
    print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, NULL, 0, &out);

    return TRUE;
}

static char *
sanitize_team_config(const char *config)
{
    char *ret;
    int   i;

    if (!config)
        return NULL;

    ret = g_strdup(config);

    for (i = 0; i < strlen(ret); i++) {
        if (ret[i] == '\n')
            ret[i] = ' ';
    }

    return ret;
}

static gboolean
print_team_info(NMDevice *device, NmCli *nmc, const char *group_prefix, const char *one_field)
{
    const GPtrArray                 *ports = NULL;
    GString                         *ports_str;
    int                              idx;
    const NMMetaAbstractInfo *const *tmpl;
    NmcOutputField                  *arr;
    NMC_OUTPUT_DATA_DEFINE_SCOPED(out);

    if (NM_IS_DEVICE_TEAM(device))
        ports = nm_device_get_ports(device);
    else
        g_return_val_if_reached(FALSE);

    ports_str = g_string_new(NULL);
    for (idx = 0; ports && idx < ports->len; idx++) {
        NMDevice   *port  = g_ptr_array_index(ports, idx);
        const char *iface = nm_device_get_iface(port);

        if (iface) {
            g_string_append(ports_str, iface);
            g_string_append_c(ports_str, ' ');
        }
    }
    if (ports_str->len > 0)
        g_string_truncate(ports_str, ports_str->len - 1); /* Chop off last space */

    tmpl        = (const NMMetaAbstractInfo *const *) nmc_fields_dev_show_team_prop;
    out_indices = parse_output_fields(one_field, tmpl, FALSE, NULL, NULL);
    arr         = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_FIELD_NAMES);
    g_ptr_array_add(out.output_data, arr);

    arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_SECTION_PREFIX);
    set_val_strc(arr, 0, group_prefix); /* TEAM */
    set_val_str(arr, 1, g_string_free(ports_str, FALSE));
    set_val_str(arr, 2, sanitize_team_config(nm_device_team_get_config(NM_DEVICE_TEAM(device))));
    g_ptr_array_add(out.output_data, arr);

    print_data_prepare_width(out.output_data);
    print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, NULL, 0, &out);

    return TRUE;
}

static gboolean
show_device_info(NMDevice *device, NmCli *nmc)
{
    GError                          *error = NULL;
    NMDeviceState                    state = NM_DEVICE_STATE_UNKNOWN;
    GArray                          *sections_array;
    int                              k;
    const char                      *fields_str = NULL;
    const NMMetaAbstractInfo *const *tmpl;
    NmcOutputField                  *arr;
    gboolean                         was_output = FALSE;
    NMIPConfig                      *cfg4, *cfg6;
    NMDhcpConfig                    *dhcp4, *dhcp6;
    const char                      *base_hdr          = _("Device details");
    GPtrArray                       *fields_in_section = NULL;

    if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "common") == 0)
        fields_str = NMC_FIELDS_DEV_SHOW_SECTIONS_COMMON;
    else if (g_ascii_strcasecmp(nmc->required_fields, "all") == 0) {
        /* pass */
    } else
        fields_str = nmc->required_fields;

    sections_array =
        parse_output_fields(fields_str,
                            (const NMMetaAbstractInfo *const *) nmc_fields_dev_show_sections,
                            TRUE,
                            &fields_in_section,
                            &error);
    if (error) {
        g_string_printf(nmc->return_text, _("Error: 'device show': %s"), error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return FALSE;
    }

    {
        gs_unref_array GArray  *out_indices = NULL;
        gs_free char           *header_name = NULL;
        gs_free NmcOutputField *row         = NULL;
        int                     i;

        /* Main header (pretty only) */
        header_name = construct_header_name(base_hdr, nm_device_get_iface(device));

        /* Lazy way to retrieve sorted array from 0 to the number of dev fields */
        out_indices =
            parse_output_fields(NULL,
                                (const NMMetaAbstractInfo *const *) metagen_device_detail_general,
                                FALSE,
                                NULL,
                                NULL);

        row = g_new0(NmcOutputField, G_N_ELEMENTS(metagen_device_detail_general));
        for (i = 0; i < G_N_ELEMENTS(metagen_device_detail_general); i++)
            row[i].info = (const NMMetaAbstractInfo *) &metagen_device_detail_general[i];

        print_required_fields(&nmc->nmc_config,
                              &nmc->pager_data,
                              NMC_OF_FLAG_MAIN_HEADER_ONLY,
                              out_indices,
                              header_name,
                              0,
                              row);
    }

    /* Loop through the required sections and print them. */
    for (k = 0; k < sections_array->len; k++) {
        int   section_idx = nm_g_array_index(sections_array, int, k);
        char *section_fld = (char *) g_ptr_array_index(fields_in_section, k);

        if (NM_IN_SET(nmc->nmc_config.print_output, NMC_PRINT_NORMAL, NMC_PRINT_PRETTY)
            && !nmc->nmc_config.multiline_output && was_output)
            nmc_print("\n"); /* Print empty line between groups in tabular mode */

        was_output = FALSE;

        state = nm_device_get_state(device);

        if (nmc_fields_dev_show_sections[section_idx]->nested == metagen_device_detail_general) {
            gs_free char *f = section_fld ? g_strdup_printf("GENERAL.%s", section_fld) : NULL;

            nmc_print_table(
                &nmc->nmc_config,
                (gpointer[]) {device, NULL},
                NULL,
                NULL,
                NMC_META_GENERIC_GROUP("GENERAL", metagen_device_detail_general, N_("NAME")),
                f,
                NULL);
            was_output = TRUE;
            continue;
        }

        if (nmc_fields_dev_show_sections[section_idx]->nested
            == metagen_device_detail_capabilities) {
            gs_free char *f = section_fld ? g_strdup_printf("CAPABILITIES.%s", section_fld) : NULL;

            nmc_print_table(&nmc->nmc_config,
                            (gpointer[]) {device, NULL},
                            NULL,
                            NULL,
                            NMC_META_GENERIC_GROUP("CAPABILITIES",
                                                   metagen_device_detail_capabilities,
                                                   N_("NAME")),
                            f,
                            NULL);
            was_output = TRUE;
            continue;
        }

        if (nmc_fields_dev_show_sections[section_idx]->nested
            == metagen_device_detail_interface_flags) {
            gs_free char *f =
                section_fld ? g_strdup_printf("INTERFACE-FLAGS.%s", section_fld) : NULL;

            nmc_print_table(&nmc->nmc_config,
                            (gpointer[]) {device, NULL},
                            NULL,
                            NULL,
                            NMC_META_GENERIC_GROUP("INTERFACE-FLAGS",
                                                   metagen_device_detail_interface_flags,
                                                   N_("NAME")),
                            f,
                            NULL);
            was_output = TRUE;
            continue;
        }

        if (nmc_fields_dev_show_sections[section_idx]->nested
            == metagen_device_detail_wifi_properties) {
            if (NM_IS_DEVICE_WIFI(device)) {
                gs_free char *f =
                    section_fld ? g_strdup_printf("WIFI-PROPERTIES.%s", section_fld) : NULL;

                nmc_print_table(&nmc->nmc_config,
                                (gpointer[]) {device, NULL},
                                NULL,
                                NULL,
                                NMC_META_GENERIC_GROUP("WIFI-PROPERTIES",
                                                       metagen_device_detail_wifi_properties,
                                                       N_("NAME")),
                                f,
                                NULL);
                was_output = TRUE;
            }
            continue;
        }

        /* Wireless specific information */
        if ((NM_IS_DEVICE_WIFI(device))) {
            /* section AP */
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[4]->name)) {
                NMAccessPoint *active_ap = NULL;
                NMC_OUTPUT_DATA_DEFINE_SCOPED(out);

                if (state == NM_DEVICE_STATE_ACTIVATED)
                    active_ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device));

                tmpl = (const NMMetaAbstractInfo *const *) nmc_fields_dev_wifi_list;
                out_indices =
                    parse_output_fields(section_fld ?: NMC_FIELDS_DEV_WIFI_LIST_FOR_DEV_LIST,
                                        tmpl,
                                        FALSE,
                                        NULL,
                                        NULL);
                arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_FIELD_NAMES);
                g_ptr_array_add(out.output_data, arr);

                {
                    gs_unref_ptrarray GPtrArray *aps        = NULL;
                    int                          info_index = 0;
                    const APInfo                 info       = {
                                              .nmc          = nmc,
                                              .p_index      = &info_index,
                                              .output_flags = NMC_OF_FLAG_SECTION_PREFIX,
                                              .active_ap    = active_ap,
                                              .device       = nm_device_get_iface(device),
                                              .output_data  = out.output_data,
                    };

                    aps = sort_access_points(
                        nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device)));
                    g_ptr_array_foreach(aps, fill_output_access_point_void, (gpointer) &info);
                }

                print_data_prepare_width(out.output_data);
                print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, NULL, 0, &out);
                was_output = TRUE;
            }
        }

        if (nmc_fields_dev_show_sections[section_idx]->nested
            == metagen_device_detail_wired_properties) {
            if ((NM_IS_DEVICE_ETHERNET(device))) {
                gs_free char *f =
                    section_fld ? g_strdup_printf("WIRED-PROPERTIES.%s", section_fld) : NULL;

                nmc_print_table(&nmc->nmc_config,
                                (gpointer[]) {device, NULL},
                                NULL,
                                NULL,
                                NMC_META_GENERIC_GROUP("WIRED-PROPERTIES",
                                                       metagen_device_detail_wired_properties,
                                                       N_("NAME")),
                                f,
                                NULL);
                was_output = TRUE;
            }
            continue;
        }

        /* IP configuration info */
        cfg4  = nm_device_get_ip4_config(device);
        cfg6  = nm_device_get_ip6_config(device);
        dhcp4 = nm_device_get_dhcp4_config(device);
        dhcp6 = nm_device_get_dhcp6_config(device);

        /* IP4 */
        if (cfg4
            && !g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                   nmc_fields_dev_show_sections[8]->name))
            was_output = print_ip_config(cfg4, AF_INET, &nmc->nmc_config, section_fld);

        /* DHCP4 */
        if (dhcp4
            && !g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                   nmc_fields_dev_show_sections[9]->name))
            was_output = print_dhcp_config(dhcp4, AF_INET, &nmc->nmc_config, section_fld);

        /* IP6 */
        if (cfg6
            && !g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                   nmc_fields_dev_show_sections[10]->name))
            was_output = print_ip_config(cfg6, AF_INET6, &nmc->nmc_config, section_fld);

        /* DHCP6 */
        if (dhcp6
            && !g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                   nmc_fields_dev_show_sections[11]->name))
            was_output = print_dhcp_config(dhcp6, AF_INET6, &nmc->nmc_config, section_fld);

        /* Bond specific information */
        if (NM_IS_DEVICE_BOND(device)) {
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[12]->name))
                was_output = print_bond_bridge_info(device,
                                                    nmc,
                                                    nmc_fields_dev_show_sections[12]->name,
                                                    section_fld);
        }

        /* Team specific information */
        if (NM_IS_DEVICE_TEAM(device)) {
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[13]->name))
                was_output = print_team_info(device,
                                             nmc,
                                             nmc_fields_dev_show_sections[13]->name,
                                             section_fld);
        }

        /* Bridge specific information */
        if (NM_IS_DEVICE_BRIDGE(device)) {
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[14]->name))
                was_output = print_bond_bridge_info(device,
                                                    nmc,
                                                    nmc_fields_dev_show_sections[14]->name,
                                                    section_fld);
        }

        /* VLAN-specific information */
        if ((NM_IS_DEVICE_VLAN(device))) {
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[15]->name)) {
                char *vlan_id_str =
                    g_strdup_printf("%u", nm_device_vlan_get_vlan_id(NM_DEVICE_VLAN(device)));
                NMDevice *parent = nm_device_vlan_get_parent(NM_DEVICE_VLAN(device));
                NMC_OUTPUT_DATA_DEFINE_SCOPED(out);

                tmpl        = (const NMMetaAbstractInfo *const *) nmc_fields_dev_show_vlan_prop;
                out_indices = parse_output_fields(section_fld, tmpl, FALSE, NULL, NULL);
                arr         = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_FIELD_NAMES);
                g_ptr_array_add(out.output_data, arr);

                arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_SECTION_PREFIX);
                set_val_strc(arr, 0, nmc_fields_dev_show_sections[15]->name); /* "VLAN" */
                set_val_strc(arr, 1, parent ? nm_device_get_iface(parent) : NULL);
                set_val_str(arr, 2, vlan_id_str);
                g_ptr_array_add(out.output_data, arr);

                print_data_prepare_width(out.output_data);
                print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, NULL, 0, &out);

                was_output = TRUE;
            }
        }

        if (NM_IS_DEVICE_BT(device)) {
            if (!g_ascii_strcasecmp(nmc_fields_dev_show_sections[section_idx]->name,
                                    nmc_fields_dev_show_sections[16]->name)) {
                NMC_OUTPUT_DATA_DEFINE_SCOPED(out);

                tmpl        = (const NMMetaAbstractInfo *const *) nmc_fields_dev_show_bluetooth;
                out_indices = parse_output_fields(section_fld, tmpl, FALSE, NULL, NULL);
                arr         = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_FIELD_NAMES);
                g_ptr_array_add(out.output_data, arr);

                arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_SECTION_PREFIX);
                set_val_strc(arr, 0, nmc_fields_dev_show_sections[16]->name); /* "BLUETOOTH" */
                set_val_str(
                    arr,
                    1,
                    bluetooth_caps_to_string(nm_device_bt_get_capabilities(NM_DEVICE_BT(device))));
                g_ptr_array_add(out.output_data, arr);

                print_data_prepare_width(out.output_data);
                print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, NULL, 0, &out);
                was_output = TRUE;
            }
        }

        if (nmc_fields_dev_show_sections[section_idx]->nested
            == metagen_device_detail_connections) {
            gs_free char *f = section_fld ? g_strdup_printf("CONNECTIONS.%s", section_fld) : NULL;

            nmc_print_table(&nmc->nmc_config,
                            (gpointer[]) {device, NULL},
                            NULL,
                            NULL,
                            NMC_META_GENERIC_GROUP("CONNECTIONS",
                                                   metagen_device_detail_connections,
                                                   N_("NAME")),
                            f,
                            NULL);
            was_output = TRUE;
            continue;
        }
    }

    if (sections_array)
        g_array_free(sections_array, TRUE);
    if (fields_in_section)
        g_ptr_array_free(fields_in_section, TRUE);

    return TRUE;
}

NMMetaColor
nmc_device_state_to_color(NMDevice *device)
{
    NMDeviceState       state;
    NMActiveConnection *ac;

    if (!device)
        return NM_META_COLOR_DEVICE_UNKNOWN;

    ac = nm_device_get_active_connection(device);
    if (ac
        && NM_FLAGS_HAS(nm_active_connection_get_state_flags(ac),
                        NM_ACTIVATION_STATE_FLAG_EXTERNAL))
        return NM_META_COLOR_CONNECTION_EXTERNAL;

    state = nm_device_get_state(device);
    if (state <= NM_DEVICE_STATE_UNAVAILABLE)
        return NM_META_COLOR_DEVICE_UNAVAILABLE;
    else if (state == NM_DEVICE_STATE_DISCONNECTED)
        return NM_META_COLOR_DEVICE_DISCONNECTED;
    else if (state >= NM_DEVICE_STATE_PREPARE && state <= NM_DEVICE_STATE_SECONDARIES)
        return NM_META_COLOR_DEVICE_ACTIVATING;
    else if (state == NM_DEVICE_STATE_ACTIVATED)
        return NM_META_COLOR_DEVICE_ACTIVATED;

    return NM_META_COLOR_DEVICE_UNKNOWN;
}

static void
do_devices_status(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    GError            *error      = NULL;
    gs_free NMDevice **devices    = NULL;
    const char        *fields_str = NULL;

    next_arg(nmc, &argc, &argv, NULL);

    if (nmc->complete)
        return;

    if (argc) {
        g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "common") == 0)
        fields_str = "DEVICE,TYPE,STATE,CONNECTION";
    else if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "all") == 0) {
        /* pass */
    } else
        fields_str = nmc->required_fields;

    devices = nmc_get_devices_sorted(nmc->client);

    if (!nmc_print_table(&nmc->nmc_config,
                         (gpointer *) devices,
                         NULL,
                         N_("Status of devices"),
                         (const NMMetaAbstractInfo *const *) metagen_device_status,
                         fields_str,
                         &error)) {
        g_string_printf(nmc->return_text, _("Error: 'device status': %s"), error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }
}

static void
do_device_show(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    gs_free_error GError *error = NULL;

    next_arg(nmc, &argc, &argv, NULL);
    if (!nmc->mode_specified)
        nmc->nmc_config_mutable.multiline_output =
            TRUE; /* multiline mode is default for 'device show' */

    if (argc) {
        NMDevice *device;

        device = get_device(nmc, &argc, &argv, &error);
        if (!device) {
            g_string_printf(nmc->return_text, _("Error: %s."), error->message);
            nmc->return_value = error->code;
            return;
        }

        if (argc) {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        if (nmc->complete)
            return;

        show_device_info(device, nmc);
    } else {
        NMDevice **devices = nmc_get_devices_sorted(nmc->client);
        int        i;

        /* nmc_do_cmd() should not call this with argc=0. */
        nm_assert(!nmc->complete);

        /* Show details for all devices */
        for (i = 0; devices[i]; i++) {
            if (!show_device_info(devices[i], nmc))
                break;
            if (devices[i + 1])
                nmc_print("\n"); /* Empty line */
        }

        g_free(devices);
    }
}

static gboolean
timeout_cb(gpointer user_data)
{
    /* Time expired -> exit nmcli */

    NmCli *nmc = (NmCli *) user_data;

    g_string_printf(nmc->return_text, _("Error: Timeout %d sec expired."), nmc->timeout);
    nmc->return_value = NMC_RESULT_ERROR_TIMEOUT_EXPIRED;
    quit();
    return FALSE;
}

static gboolean
progress_cb(gpointer user_data)
{
    NMDevice *device = (NMDevice *) user_data;

    nmc_terminal_show_progress(device ? gettext(nmc_device_state_to_string_with_external(device))
                                      : "");

    return TRUE;
}

typedef struct {
    NmCli              *nmc;
    NMDevice           *device;
    NMActiveConnection *active;
    char               *specific_object;
    bool                hotspot : 1;
    bool                create : 1;
} AddAndActivateInfo;

static AddAndActivateInfo *
add_and_activate_info_new(NmCli      *nmc,
                          NMDevice   *device,
                          gboolean    hotspot,
                          gboolean    create,
                          const char *specific_object)
{
    AddAndActivateInfo *info;

    info  = g_slice_new(AddAndActivateInfo);
    *info = (AddAndActivateInfo) {
        .nmc             = nmc,
        .device          = g_object_ref(device),
        .hotspot         = hotspot,
        .create          = create,
        .specific_object = g_strdup(specific_object),
    };
    return info;
}

static void
add_and_activate_info_free(AddAndActivateInfo *info)
{
    g_object_unref(info->device);
    g_clear_object(&info->active);
    g_free(info->specific_object);
    nm_g_slice_free(info);
}

NM_AUTO_DEFINE_FCN0(AddAndActivateInfo *,
                    _nm_auto_free_add_and_activate_info,
                    add_and_activate_info_free);
#define nm_auto_free_add_and_activate_info nm_auto(_nm_auto_free_add_and_activate_info)

static void add_and_activate_notify_state_cb(GObject *src, GParamSpec *pspec, gpointer user_data);

static void
add_and_activate_check_state(AddAndActivateInfo *info)
{
    NMDeviceState           state;
    NMDeviceStateReason     reason;
    NMActiveConnectionState ac_state;

    state    = nm_device_get_state(info->device);
    ac_state = nm_active_connection_get_state(info->active);

    if (ac_state == NM_ACTIVE_CONNECTION_STATE_ACTIVATING)
        return;

    if (state == NM_DEVICE_STATE_ACTIVATED) {
        nmc_terminal_erase_line();
        nmc_print(_("Device '%s' successfully activated with '%s'.\n"),
                  nm_device_get_iface(info->device),
                  nm_active_connection_get_uuid(info->active));

        if (info->hotspot)
            nmc_print(
                _("Hint: \"nmcli dev wifi show-password\" shows the Wi-Fi name and password.\n"));
    } else if (state <= NM_DEVICE_STATE_DISCONNECTED || state >= NM_DEVICE_STATE_DEACTIVATING) {
        reason = nm_device_get_state_reason(info->device);
        g_string_printf(info->nmc->return_text,
                        _("Error: Connection activation failed: %s."),
                        gettext(nmc_device_reason_to_string(reason)));
        info->nmc->return_value = NMC_RESULT_ERROR_CON_ACTIVATION;
    } else {
        return;
    }

    g_signal_handlers_disconnect_by_func(info->active,
                                         G_CALLBACK(add_and_activate_notify_state_cb),
                                         info);
    g_signal_handlers_disconnect_by_func(info->device,
                                         G_CALLBACK(add_and_activate_notify_state_cb),
                                         info);
    add_and_activate_info_free(info);

    quit();
}

static void
add_and_activate_notify_state_cb(GObject *src, GParamSpec *pspec, gpointer user_data)

{
    add_and_activate_check_state(user_data);
}

static void
add_and_activate_cb(GObject *client, GAsyncResult *result, gpointer user_data)
{
    nm_auto_free_add_and_activate_info AddAndActivateInfo *info   = user_data;
    NmCli                                                 *nmc    = info->nmc;
    gs_unref_object NMActiveConnection                    *active = NULL;
    gs_free_error GError                                  *error  = NULL;
    const char                                            *deprecated;

    if (info->create)
        active = nm_client_add_and_activate_connection_finish(NM_CLIENT(client), result, &error);
    else
        active = nm_client_activate_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        if (info->hotspot) {
            g_string_printf(nmc->return_text,
                            _("Error: Failed to setup a Wi-Fi hotspot: %s"),
                            error->message);
        } else if (info->create) {
            g_string_printf(nmc->return_text,
                            _("Error: Failed to add/activate new connection: %s"),
                            error->message);
        } else {
            g_string_printf(nmc->return_text,
                            _("Error: Failed to activate connection: %s"),
                            error->message);
        }
        nmc->return_value = NMC_RESULT_ERROR_CON_ACTIVATION;
        quit();
        return;
    }

    deprecated =
        nmc_connection_check_deprecated(NM_CONNECTION(nm_active_connection_get_connection(active)));
    if (deprecated)
        nmc_printerr(_("Warning: %s.\n"), deprecated);

    if (nmc->nowait_flag) {
        quit();
        return;
    }

    if (nmc->secret_agent) {
        NMRemoteConnection *connection = nm_active_connection_get_connection(active);

        nm_secret_agent_simple_enable(nmc->secret_agent,
                                      nm_connection_get_path(NM_CONNECTION(connection)));
    }

    if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
        progress_id = g_timeout_add(120, progress_cb, info->device);

    info->active = g_steal_pointer(&active);
    g_signal_connect(info->device,
                     "notify::" NM_DEVICE_STATE,
                     G_CALLBACK(add_and_activate_notify_state_cb),
                     info);
    g_signal_connect(info->active,
                     "notify::" NM_ACTIVE_CONNECTION_STATE,
                     G_CALLBACK(add_and_activate_notify_state_cb),
                     info);
    add_and_activate_check_state(g_steal_pointer(&info));

    g_timeout_add_seconds(nmc->timeout, timeout_cb, nmc); /* Exit if timeout expires */
}

static void
create_connect_connection_for_device(AddAndActivateInfo *info)
{
    NMConnection        *connection;
    NMSettingConnection *s_con;

    /* Create new connection and tie it to the device */
    connection = nm_simple_connection_new();
    s_con      = (NMSettingConnection *) nm_setting_connection_new();
    nm_connection_add_setting(connection, NM_SETTING(s_con));
    g_object_set(s_con, NM_SETTING_CONNECTION_ID, nm_device_get_iface(info->device), NULL);

    nm_client_add_and_activate_connection_async(info->nmc->client,
                                                connection,
                                                info->device,
                                                NULL,
                                                NULL,
                                                add_and_activate_cb,
                                                info);
}

static void
connect_device_cb(GObject *client, GAsyncResult *result, gpointer user_data)
{
    nm_auto_free_add_and_activate_info AddAndActivateInfo *info   = user_data;
    NmCli                                                 *nmc    = info->nmc;
    gs_unref_object NMActiveConnection                    *active = NULL;
    GError                                                *error  = NULL;

    active = nm_client_activate_connection_finish(NM_CLIENT(client), result, &error);

    if (error) {
        /* If no connection existed for the device, create one and activate it */
        if (g_error_matches(error, NM_MANAGER_ERROR, NM_MANAGER_ERROR_UNKNOWN_CONNECTION)) {
            info->create = TRUE;
            create_connect_connection_for_device(g_steal_pointer(&info));
            return;
        }

        g_string_printf(nmc->return_text, _("Error: Device activation failed: %s"), error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_CON_ACTIVATION;
        quit();
        return;
    }

    nm_assert(NM_IS_ACTIVE_CONNECTION(active));

    if (nmc->nowait_flag) {
        quit();
        return;
    }

    if (nmc->secret_agent) {
        NMRemoteConnection *connection = nm_active_connection_get_connection(active);

        nm_secret_agent_simple_enable(nmc->secret_agent,
                                      nm_connection_get_path(NM_CONNECTION(connection)));
    }

    info->active = g_steal_pointer(&active);
    g_signal_connect(info->device,
                     "notify::" NM_DEVICE_STATE,
                     G_CALLBACK(add_and_activate_notify_state_cb),
                     info);
    g_signal_connect(info->active,
                     "notify::" NM_ACTIVE_CONNECTION_STATE,
                     G_CALLBACK(add_and_activate_notify_state_cb),
                     info);
    add_and_activate_check_state(g_steal_pointer(&info));

    /* Start timer not to loop forever if "notify::state" signal is not issued */
    g_timeout_add_seconds(nmc->timeout, timeout_cb, nmc);
}

static void
do_device_connect(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice             *device = NULL;
    AddAndActivateInfo   *info;
    gs_free_error GError *error = NULL;

    /* Set default timeout for connect operation. */
    if (nmc->timeout == -1)
        nmc->timeout = 90;

    next_arg(nmc, &argc, &argv, NULL);
    device = get_device(nmc, &argc, &argv, &error);
    if (!device) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = error->code;
        return;
    }

    if (*argv) {
        g_string_printf(nmc->return_text, _("Error: extra argument not allowed: '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (nmc->complete)
        return;

    /*
     * Use nowait_flag instead of should_wait, because exiting has to be postponed
     * till connect_device_cb() is called, giving NM time to check our permissions.
     */
    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    /* Create secret agent */
    nmc->secret_agent = nm_secret_agent_simple_new("nmcli-connect");
    if (nmc->secret_agent) {
        g_signal_connect(nmc->secret_agent,
                         NM_SECRET_AGENT_SIMPLE_REQUEST_SECRETS,
                         G_CALLBACK(nmc_secrets_requested),
                         nmc);
    }

    info = add_and_activate_info_new(nmc, device, FALSE, FALSE, NULL);

    nm_client_activate_connection_async(nmc->client,
                                        NULL, /* let NM find a connection automatically */
                                        device,
                                        NULL,
                                        NULL,
                                        connect_device_cb,
                                        info);

    /* Start progress indication */
    if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
        progress_id = g_timeout_add(120, progress_cb, device);
}

typedef struct {
    NmCli        *nmc;
    GPtrArray    *queue;
    guint         timeout_id;
    gboolean      cmd_disconnect;
    GCancellable *cancellable;
} DeviceCbInfo;

static void device_cb_info_finish(DeviceCbInfo *info, NMDevice *device);

static gboolean
device_op_timeout_cb(gpointer user_data)
{
    DeviceCbInfo *info = user_data;

    timeout_cb(info->nmc);
    device_cb_info_finish(info, NULL);
    return G_SOURCE_REMOVE;
}

static void
device_removed_cb(NMClient *client, NMDevice *device, DeviceCbInfo *info)
{
    /* Success: device has been removed.
     * It can also happen when disconnecting a software device.
     */
    if (!g_ptr_array_find(info->queue, device, NULL))
        return;

    if (info->cmd_disconnect)
        nmc_print(_("Device '%s' successfully disconnected.\n"), nm_device_get_iface(device));
    else
        nmc_print(_("Device '%s' successfully removed.\n"), nm_device_get_iface(device));
    device_cb_info_finish(info, device);
}

static void
disconnect_state_cb(NMDevice *device, GParamSpec *pspec, DeviceCbInfo *info)
{
    if (!g_ptr_array_find(info->queue, device, NULL))
        return;

    if (nm_device_get_state(device) <= NM_DEVICE_STATE_DISCONNECTED) {
        nmc_print(_("Device '%s' successfully disconnected.\n"), nm_device_get_iface(device));
        device_cb_info_finish(info, device);
    }
}

static void
destroy_queue_element(gpointer data)
{
    g_signal_handlers_disconnect_matched(data,
                                         G_SIGNAL_MATCH_FUNC,
                                         0,
                                         0,
                                         0,
                                         disconnect_state_cb,
                                         NULL);
    g_object_unref(data);
}

static void
device_cb_info_finish(DeviceCbInfo *info, NMDevice *device)
{
    if (device) {
        if (!g_ptr_array_remove(info->queue, device))
            return;
        if (info->queue->len)
            return;
    }

    if (info->timeout_id)
        g_source_remove(info->timeout_id);

    g_ptr_array_free(info->queue, TRUE);
    g_signal_handlers_disconnect_by_func(info->nmc->client, device_removed_cb, info);
    nm_clear_g_cancellable(&info->cancellable);

    g_slice_free(DeviceCbInfo, info);
    quit();
}

static void
reapply_device_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NMDevice     *device = NM_DEVICE(object);
    DeviceCbInfo *info   = (DeviceCbInfo *) user_data;
    NmCli        *nmc    = info->nmc;
    GError       *error  = NULL;

    if (!nm_device_reapply_finish(device, result, &error)) {
        g_string_printf(nmc->return_text,
                        _("Error: Reapplying connection to device '%s' (%s) failed: %s"),
                        nm_device_get_iface(device),
                        nm_object_get_path(NM_OBJECT(device)),
                        error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_DEV_DISCONNECT;
        device_cb_info_finish(info, device);
    } else {
        if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
            nmc_terminal_erase_line();
        nmc_print(_("Connection successfully reapplied to device '%s'.\n"),
                  nm_device_get_iface(device));
        device_cb_info_finish(info, device);
    }
}

static void
do_device_reapply(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice             *device;
    DeviceCbInfo         *info  = NULL;
    gs_free_error GError *error = NULL;

    /* Set default timeout for reapply operation. */
    if (nmc->timeout == -1)
        nmc->timeout = 10;

    next_arg(nmc, &argc, &argv, NULL);
    device = get_device(nmc, &argc, &argv, &error);
    if (!device) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = error->code;
        return;
    }

    if (argc) {
        g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (nmc->complete)
        return;

    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    info      = g_slice_new0(DeviceCbInfo);
    info->nmc = nmc;

    info->queue = g_ptr_array_new_with_free_func(destroy_queue_element);
    g_ptr_array_add(info->queue, g_object_ref(device));

    /* Now reapply the connection to the device */
    nm_device_reapply_async(device, NULL, 0, 0, NULL, reapply_device_cb, info);
}

typedef struct {
    NmCli *nmc;
    int    argc;
    char **argv;
} ModifyInfo;

static void
modify_info_free(ModifyInfo *info)
{
    g_strfreev(info->argv);
    nm_g_slice_free(info);
}

NM_AUTO_DEFINE_FCN_VOID0(ModifyInfo *, _auto_free_modify_info, modify_info_free);

static void
modify_reapply_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NMDevice                                   *device = NM_DEVICE(object);
    nm_auto(_auto_free_modify_info) ModifyInfo *info   = user_data;
    NmCli                                      *nmc    = info->nmc;
    GError                                     *error  = NULL;

    if (!nm_device_reapply_finish(device, result, &error)) {
        g_string_printf(nmc->return_text,
                        _("Error: Reapplying connection to device '%s' (%s) failed: %s"),
                        nm_device_get_iface(device),
                        nm_object_get_path(NM_OBJECT(device)),
                        error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_DEV_DISCONNECT;
    } else {
        if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
            nmc_terminal_erase_line();
        nmc_print(_("Connection successfully reapplied to device '%s'.\n"),
                  nm_device_get_iface(device));
    }

    quit();
}

static void
modify_get_applied_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NMDevice                                   *device = NM_DEVICE(object);
    nm_auto(_auto_free_modify_info) ModifyInfo *info   = user_data;
    NmCli                                      *nmc    = info->nmc;
    gs_free_error GError                       *error  = NULL;
    NMConnection                               *connection;
    guint64                                     version_id;
    int                                         argc;
    const char *const                          *argv;

    connection = nm_device_get_applied_connection_finish(device, result, &version_id, &error);
    if (!connection) {
        g_string_printf(nmc->return_text,
                        _("Error: Reading applied connection from device '%s' (%s) failed: %s"),
                        nm_device_get_iface(device),
                        nm_object_get_path(NM_OBJECT(device)),
                        error->message);
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        quit();
        return;
    }

    argc = info->argc;
    argv = (const char *const *) info->argv;

    if (!nmc_process_connection_properties(info->nmc, connection, argc, argv, TRUE, &error)) {
        g_string_assign(nmc->return_text, error->message);
        nmc->return_value = error->code;
        quit();
        return;
    }

    if (nmc->complete) {
        quit();
        return;
    }

    nm_device_reapply_async(device,
                            connection,
                            version_id,
                            0,
                            NULL,
                            modify_reapply_cb,
                            g_steal_pointer(&info));
}

static void
do_device_modify(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice             *device = NULL;
    ModifyInfo           *info;
    gs_free_error GError *error = NULL;

    next_arg(nmc, &argc, &argv, NULL);
    device = get_device(nmc, &argc, &argv, &error);
    if (!device) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = error->code;
        return;
    }

    if (nmc->timeout == -1)
        nmc->timeout = 10;

    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    info  = g_slice_new(ModifyInfo);
    *info = (ModifyInfo) {
        .nmc  = nmc,
        .argc = argc,
        .argv = nm_strv_dup(argv, argc, TRUE),
    };

    nm_device_get_applied_connection_async(device, 0, NULL, modify_get_applied_cb, info);
}

static void
disconnect_device_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NMDevice     *device = NM_DEVICE(object);
    DeviceCbInfo *info   = (DeviceCbInfo *) user_data;
    NmCli        *nmc;
    NMDeviceState state;
    GError       *error = NULL;

    if (!nm_device_disconnect_finish(device, result, &error)) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;
        nmc = info->nmc;
        g_string_printf(nmc->return_text, _("Error: not all devices disconnected."));
        nmc_printerr(_("Error: Device '%s' (%s) disconnecting failed: %s\n"),
                     nm_device_get_iface(device),
                     nm_object_get_path(NM_OBJECT(device)),
                     error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_DEV_DISCONNECT;
        device_cb_info_finish(info, device);
    } else {
        nmc   = info->nmc;
        state = nm_device_get_state(device);
        if (nmc->nowait_flag || state <= NM_DEVICE_STATE_DISCONNECTED) {
            /* Don't want to wait or device already disconnected */
            if (state <= NM_DEVICE_STATE_DISCONNECTED) {
                if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
                    nmc_terminal_erase_line();
                nmc_print(_("Device '%s' successfully disconnected.\n"),
                          nm_device_get_iface(device));
            }
            device_cb_info_finish(info, device);
        }
    }
}

static void
do_devices_disconnect(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice                    *device;
    DeviceCbInfo                *info  = NULL;
    gs_unref_ptrarray GPtrArray *queue = NULL;
    guint                        i;

    /* Set default timeout for disconnect operation. */
    if (nmc->timeout == -1)
        nmc->timeout = 10;

    next_arg(nmc, &argc, &argv, NULL);
    queue = get_device_list(nmc, &argc, &argv);
    if (argc) {
        g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }
    if (!queue)
        return;
    if (nmc->complete)
        return;

    info                 = g_slice_new0(DeviceCbInfo);
    info->queue          = g_steal_pointer(&queue);
    info->nmc            = nmc;
    info->cmd_disconnect = TRUE;
    info->cancellable    = g_cancellable_new();
    if (nmc->timeout > 0)
        info->timeout_id = g_timeout_add_seconds(nmc->timeout, device_op_timeout_cb, info);

    g_signal_connect(nmc->client, NM_CLIENT_DEVICE_REMOVED, G_CALLBACK(device_removed_cb), info);

    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    for (i = 0; i < info->queue->len; i++) {
        device = info->queue->pdata[i];

        g_signal_connect(device, "notify::" NM_DEVICE_STATE, G_CALLBACK(disconnect_state_cb), info);
        nm_device_disconnect_async(device, info->cancellable, disconnect_device_cb, info);
    }
}

static void
delete_device_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NMDevice     *device = NM_DEVICE(object);
    DeviceCbInfo *info   = (DeviceCbInfo *) user_data;
    NmCli        *nmc    = info->nmc;
    GError       *error  = NULL;

    if (!nm_device_delete_finish(device, result, &error)) {
        g_string_printf(nmc->return_text, _("Error: not all devices deleted."));
        nmc_printerr(_("Error: Device '%s' (%s) deletion failed: %s\n"),
                     nm_device_get_iface(device),
                     nm_object_get_path(NM_OBJECT(device)),
                     error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        device_cb_info_finish(info, device);
    } else {
        nmc_print(_("Device '%s' successfully removed.\n"), nm_device_get_iface(device));
        device_cb_info_finish(info, device);
    }
}

static void
do_devices_delete(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    DeviceCbInfo                *info  = NULL;
    gs_unref_ptrarray GPtrArray *queue = NULL;
    guint                        i;

    /* Set default timeout for delete operation. */
    if (nmc->timeout == -1)
        nmc->timeout = 10;

    next_arg(nmc, &argc, &argv, NULL);
    queue = get_device_list(nmc, &argc, &argv);
    if (argc) {
        g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }
    if (!queue)
        return;
    if (nmc->complete)
        return;

    info        = g_slice_new0(DeviceCbInfo);
    info->queue = g_steal_pointer(&queue);
    info->nmc   = nmc;
    if (nmc->timeout > 0)
        info->timeout_id = g_timeout_add_seconds(nmc->timeout, device_op_timeout_cb, info);

    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    for (i = 0; i < info->queue->len; i++) {
        nm_device_delete_async(info->queue->pdata[i], NULL, delete_device_cb, info);
    }
}

static void
do_device_set(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
#define DEV_SET_AUTOCONNECT 0
#define DEV_SET_MANAGED     1
    NMDevice *device = NULL;
    int       i;
    struct {
        int      idx;
        gboolean value;
    } values[2] = {
        [DEV_SET_AUTOCONNECT] = {-1},
        [DEV_SET_MANAGED]     = {-1},
    };
    gs_free_error GError *error = NULL;

    next_arg(nmc, &argc, &argv, NULL);
    if (argc >= 1 && g_strcmp0(*argv, "ifname") == 0)
        next_arg(nmc, &argc, &argv, NULL);

    device = get_device(nmc, &argc, &argv, &error);
    if (!device) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = error->code;
        return;
    }

    if (!argc) {
        g_string_printf(nmc->return_text, _("Error: No property specified."));
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    i = 0;
    do {
        gboolean flag;

        if (argc == 1 && nmc->complete)
            nmc_complete_strings(*argv, "managed", "autoconnect");

        if (matches(*argv, "managed")) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text,
                                _("Error: '%s' argument is missing."),
                                *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            if (argc == 1 && nmc->complete)
                nmc_complete_bool(*argv);
            if (!nmc_string_to_bool(*argv, &flag, &error)) {
                g_string_printf(nmc->return_text, _("Error: 'managed': %s."), error->message);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            values[DEV_SET_MANAGED].idx   = ++i;
            values[DEV_SET_MANAGED].value = flag;
        } else if (matches(*argv, "autoconnect")) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text,
                                _("Error: '%s' argument is missing."),
                                *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            if (argc == 1 && nmc->complete)
                nmc_complete_bool(*argv);
            if (!nmc_string_to_bool(*argv, &flag, &error)) {
                g_string_printf(nmc->return_text, _("Error: 'autoconnect': %s."), error->message);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            values[DEV_SET_AUTOCONNECT].idx   = ++i;
            values[DEV_SET_AUTOCONNECT].value = flag;
        } else {
            g_string_printf(nmc->return_text, _("Error: property '%s' is not known."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }
    } while (next_arg(nmc, &argc, &argv, NULL) == 0);

    if (nmc->complete)
        return;

    /* when multiple properties are specified, set them in the order as they
     * are specified on the command line. */
    if (values[DEV_SET_AUTOCONNECT].idx >= 0 && values[DEV_SET_MANAGED].idx >= 0
        && values[DEV_SET_MANAGED].idx < values[DEV_SET_AUTOCONNECT].idx) {
        nm_device_set_managed(device, values[DEV_SET_MANAGED].value);
        values[DEV_SET_MANAGED].idx = -1;
    }
    if (values[DEV_SET_AUTOCONNECT].idx >= 0)
        nm_device_set_autoconnect(device, values[DEV_SET_AUTOCONNECT].value);
    if (values[DEV_SET_MANAGED].idx >= 0)
        nm_device_set_managed(device, values[DEV_SET_MANAGED].value);
}

static void
device_state(NMDevice *device, GParamSpec *pspec, NmCli *nmc)
{
    gs_free char *str = NULL;
    NMMetaColor   color;

    color = nmc_device_state_to_color(device);
    str   = nmc_colorize(&nmc->nmc_config,
                       color,
                       "%s: %s\n",
                       nm_device_get_iface(device),
                       gettext(nmc_device_state_to_string_with_external(device)));

    nmc_print("%s", str);
}

static void
device_ac(NMDevice *device, GParamSpec *pspec, NmCli *nmc)
{
    NMActiveConnection *ac = nm_device_get_active_connection(device);
    const char         *id = ac ? nm_active_connection_get_id(ac) : NULL;

    if (!id)
        return;

    nmc_print(_("%s: using connection '%s'\n"), nm_device_get_iface(device), id);
}

static void
device_watch(NmCli *nmc, NMDevice *device)
{
    nmc->should_wait++;
    g_signal_connect(device, "notify::" NM_DEVICE_STATE, G_CALLBACK(device_state), nmc);
    g_signal_connect(device, "notify::" NM_DEVICE_ACTIVE_CONNECTION, G_CALLBACK(device_ac), nmc);
}

static void
device_unwatch(NmCli *nmc, NMDevice *device)
{
    g_signal_handlers_disconnect_by_func(device, device_state, nmc);
    if (g_signal_handlers_disconnect_by_func(device, device_ac, nmc))
        nmc->should_wait--;

    /* Terminate if all the watched devices disappeared. */
    if (!nmc->should_wait)
        quit();
}

static void
device_added(NMClient *client, NMDevice *device, NmCli *nmc)
{
    nmc_print(_("%s: device created\n"), nm_device_get_iface(device));
    device_watch(nmc, NM_DEVICE(device));
}

static void
device_removed(NMClient *client, NMDevice *device, NmCli *nmc)
{
    nmc_print(_("%s: device removed\n"), nm_device_get_iface(device));
    device_unwatch(nmc, device);
}

static void
do_devices_monitor(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    const GPtrArray             *devices;
    gs_unref_ptrarray GPtrArray *devices_free = NULL;
    guint                        i;

    if (nmc->complete)
        return;

    next_arg(nmc, &argc, &argv, NULL);
    if (argc > 0) {
        devices = devices_free = get_device_list(nmc, &argc, &argv);
        if (argc) {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }
        if (!devices)
            return;
    } else {
        /* No devices specified. Monitor all. */
        devices = nm_client_get_devices(nmc->client);

        /* We'll watch the device additions too, never exit. */
        nmc->should_wait++;
        g_signal_connect(nmc->client, NM_CLIENT_DEVICE_ADDED, G_CALLBACK(device_added), nmc);
    }

    for (i = 0; i < devices->len; i++)
        device_watch(nmc, g_ptr_array_index(devices, i));

    g_signal_connect(nmc->client, NM_CLIENT_DEVICE_REMOVED, G_CALLBACK(device_removed), nmc);
}

/*
 * Find a Wi-Fi device with 'iface' in 'devices' array. If 'iface' is NULL,
 * the first Wi-Fi device is returned. 'idx' parameter is updated to the point
 * where the function finished so that the function can be called repeatedly
 * to get next matching device.
 * Returns: found device or NULL
 */
static NMDevice *
find_wifi_device_by_iface(NMDevice **devices, const char *iface, int *idx)
{
    int i;

    for (i = idx ? *idx : 0; devices[i]; i++) {
        const char *dev_iface = nm_device_get_iface(devices[i]);

        if (!NM_IS_DEVICE_WIFI(devices[i]))
            continue;

        if (iface) {
            /* If a iface was specified then use it. */
            if (g_strcmp0(dev_iface, iface) == 0)
                break;
        } else {
            /* Else return the first Wi-Fi device. */
            break;
        }
    }

    if (idx)
        *idx = i + 1;
    return devices[i];
}

/*
 * Find AP on 'device' according to 'bssid' and 'ssid' parameters.
 * Returns: found AP or NULL
 */
static NMAccessPoint *
find_ap_on_device(NMDevice *device, const char *bssid, const char *ssid, gboolean complete)
{
    const GPtrArray *aps;
    NMAccessPoint   *ap = NULL;
    int              i;

    g_return_val_if_fail(NM_IS_DEVICE_WIFI(device), NULL);

    aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device));
    for (i = 0; i < aps->len; i++) {
        NMAccessPoint *candidate_ap = g_ptr_array_index(aps, i);

        if (bssid) {
            const char *candidate_bssid = nm_access_point_get_bssid(candidate_ap);

            if (!candidate_bssid)
                continue;

            /* Compare BSSIDs */
            if (complete) {
                if (g_str_has_prefix(candidate_bssid, bssid))
                    nmc_print("%s\n", candidate_bssid);
            } else if (strcmp(bssid, candidate_bssid) != 0)
                continue;
        }

        if (ssid) {
            /* Parameter is SSID */
            GBytes *candidate_ssid;
            char   *ssid_tmp;

            candidate_ssid = nm_access_point_get_ssid(candidate_ap);
            if (!candidate_ssid)
                continue;

            ssid_tmp = nm_utils_ssid_to_utf8(g_bytes_get_data(candidate_ssid, NULL),
                                             g_bytes_get_size(candidate_ssid));

            /* Compare SSIDs */
            if (complete) {
                if (g_str_has_prefix(ssid_tmp, ssid))
                    nmc_print("%s\n", ssid_tmp);
            } else if (strcmp(ssid, ssid_tmp) != 0) {
                g_free(ssid_tmp);
                continue;
            }
            g_free(ssid_tmp);
        }

        if (complete)
            continue;

        ap = candidate_ap;
        break;
    }

    return ap;
}

static void
show_access_point_info(NMDeviceWifi *wifi, NmCli *nmc, NmcOutputData *out)
{
    NMAccessPoint  *active_ap = NULL;
    NmcOutputField *arr;

    if (nm_device_get_state(NM_DEVICE(wifi)) == NM_DEVICE_STATE_ACTIVATED)
        active_ap = nm_device_wifi_get_active_access_point(wifi);

    arr = nmc_dup_fields_array((const NMMetaAbstractInfo *const *) nmc_fields_dev_wifi_list,
                               NMC_OF_FLAG_MAIN_HEADER_ADD | NMC_OF_FLAG_FIELD_NAMES);
    g_ptr_array_add(out->output_data, arr);

    {
        gs_unref_ptrarray GPtrArray *aps        = NULL;
        int                          info_index = 0;
        const APInfo                 info       = {
                                  .nmc          = nmc,
                                  .p_index      = &info_index,
                                  .output_flags = 0,
                                  .active_ap    = active_ap,
                                  .device       = nm_device_get_iface(NM_DEVICE(wifi)),
                                  .output_data  = out->output_data,
        };

        aps = sort_access_points(nm_device_wifi_get_access_points(wifi));
        g_ptr_array_foreach(aps, fill_output_access_point_void, (gpointer) &info);
    }

    print_data_prepare_width(out->output_data);
}

static void
wifi_print_aps(NMDeviceWifi                    *wifi,
               NmCli                           *nmc,
               GArray                          *_out_indices,
               const NMMetaAbstractInfo *const *tmpl,
               const char                      *bssid_user,
               gboolean                        *bssid_found)
{
    NMAccessPoint   *ap = NULL;
    const GPtrArray *aps;
    guint            i;
    NmcOutputField  *arr;
    const char      *base_hdr = _("Wi-Fi scan list");
    NMC_OUTPUT_DATA_DEFINE_SCOPED(out);
    gs_free char   *header_name = NULL;
    static gboolean empty_line  = FALSE;

    if (empty_line)
        nmc_print("\n"); /* Empty line between devices' APs */

    /* Main header name */
    header_name = construct_header_name(base_hdr, nm_device_get_iface(NM_DEVICE(wifi)));

    out_indices = g_array_ref(_out_indices);

    if (bssid_user) {
        /* Specific AP requested - list only that */
        aps = nm_device_wifi_get_access_points(wifi);
        for (i = 0; i < aps->len; i++) {
            NMAccessPoint *candidate_ap = g_ptr_array_index(aps, i);

            if (nm_utils_hwaddr_matches(bssid_user,
                                        -1,
                                        nm_access_point_get_bssid(candidate_ap),
                                        -1))
                ap = candidate_ap;
        }
        if (ap) {
            int          info_index = 0;
            const APInfo info       = {
                      .nmc          = nmc,
                      .p_index      = &info_index,
                      .output_flags = 0,
                      .device       = nm_device_get_iface(NM_DEVICE(wifi)),
                      .output_data  = out.output_data,
            };

            /* Add headers (field names) */
            arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_MAIN_HEADER_ADD | NMC_OF_FLAG_FIELD_NAMES);
            g_ptr_array_add(out.output_data, arr);

            fill_output_access_point(ap, &info);

            print_data_prepare_width(out.output_data);
            print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, header_name, 0, &out);

            *bssid_found = TRUE;
            empty_line   = TRUE;
        }
    } else {
        show_access_point_info(wifi, nmc, &out);
        print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, header_name, 0, &out);
        empty_line = TRUE;
    }
}

static gint64
_device_wifi_get_last_scan(NMDeviceWifi *wifi)
{
    gint64 timestamp;

    timestamp = nm_device_wifi_get_last_scan(wifi);
    if (timestamp == -1)
        return G_MININT64;
    return timestamp;
}

typedef struct {
    NmCli                           *nmc;
    NMDevice                       **devices;
    const NMMetaAbstractInfo *const *tmpl;
    char                            *bssid_user;
    GArray                          *out_indices;
    gint64                           rescan_cutoff_msec;
    guint                            pending;
} ScanInfo;

typedef struct {
    ScanInfo     *scan_info;
    NMDeviceWifi *wifi;
    gulong        last_scan_id;
    guint         timeout_id;
    GCancellable *scan_cancellable;
} WifiListData;

static void
wifi_list_finish(WifiListData *wifi_list_data, gboolean force_finished)
{
    ScanInfo *scan_info   = wifi_list_data->scan_info;
    NmCli    *nmc         = scan_info->nmc;
    gboolean  bssid_found = FALSE;
    guint     i;

    if (!force_finished
        && scan_info->rescan_cutoff_msec > _device_wifi_get_last_scan(wifi_list_data->wifi)) {
        /* wait longer... */
        return;
    }

    nm_clear_g_signal_handler(wifi_list_data->wifi, &wifi_list_data->last_scan_id);
    nm_clear_g_source(&wifi_list_data->timeout_id);
    nm_clear_g_cancellable(&wifi_list_data->scan_cancellable);
    nm_g_slice_free(wifi_list_data);

    if (--scan_info->pending > 0)
        return;

    for (i = 0; scan_info->devices[i]; i++) {
        wifi_print_aps(NM_DEVICE_WIFI(scan_info->devices[i]),
                       nmc,
                       scan_info->out_indices,
                       scan_info->tmpl,
                       scan_info->bssid_user,
                       &bssid_found);
    }

    if (scan_info->bssid_user && !bssid_found) {
        nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        g_string_printf(nmc->return_text,
                        _("Error: Access point with bssid '%s' not found."),
                        scan_info->bssid_user);
    }

    for (i = 0; scan_info->devices[i]; i++)
        g_object_unref(scan_info->devices[i]);
    g_free(scan_info->devices);
    g_array_unref(scan_info->out_indices);
    g_free(scan_info->bssid_user);
    nm_g_slice_free(scan_info);

    nmc->should_wait--;
    g_main_loop_quit(loop);
}

static void
wifi_last_scan_updated(GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
    wifi_list_finish(user_data, FALSE);
}

static void wifi_list_rescan_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);

static void
wifi_list_rescan_retry_cb(gpointer user_data, GCancellable *cancellable)
{
    WifiListData *wifi_list_data;

    if (g_cancellable_is_cancelled(cancellable))
        return;

    wifi_list_data = user_data;
    nm_device_wifi_request_scan_async(wifi_list_data->wifi,
                                      wifi_list_data->scan_cancellable,
                                      wifi_list_rescan_cb,
                                      wifi_list_data);
}

static void
wifi_list_rescan_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    NMDeviceWifi         *wifi  = NM_DEVICE_WIFI(source_object);
    gs_free_error GError *error = NULL;
    WifiListData         *wifi_list_data;
    gboolean              force_finished;
    gboolean              done;

    nm_device_wifi_request_scan_finish(wifi, res, &error);
    if (nm_utils_error_is_cancelled(error))
        return;

    wifi_list_data = user_data;

    if (g_error_matches(error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_NOT_ALLOWED)) {
        if (nm_device_get_state(NM_DEVICE(wifi)) < NM_DEVICE_STATE_DISCONNECTED) {
            /* the device is either unmanaged or unavailable.
             *
             * If it's unmanaged, we don't expect any scan result and are done.
             * If it's unavailable, that usually means that we wait for wpa_supplicant
             * to start. In that case, also quit (without scan results). */
            force_finished = TRUE;
            done           = TRUE;
        } else {
            /* This likely means that scanning is already in progress. There's
             * a good chance we'll get updated results soon; wait for them.
             *
             * But also, NetworkManager ratelimits (and rejects requests). That
             * means, possibly we were just ratelimited, so waiting will not lead
             * to a new scan result. Instead, repeatedly ask new scans... */
            nm_utils_invoke_on_timeout(1000,
                                       wifi_list_data->scan_cancellable,
                                       wifi_list_rescan_retry_cb,
                                       wifi_list_data);
            force_finished = FALSE;
            done           = FALSE;
        }
    } else if (error) {
        force_finished = TRUE;
        done           = TRUE;
    } else {
        force_finished = FALSE;
        done           = TRUE;
    }

    if (done)
        g_clear_object(&wifi_list_data->scan_cancellable);
    wifi_list_finish(wifi_list_data, force_finished);
}

static gboolean
wifi_list_scan_timeout(gpointer user_data)
{
    WifiListData *wifi_list_data = user_data;

    wifi_list_data->timeout_id = 0;
    wifi_list_finish(user_data, TRUE);
    return G_SOURCE_REMOVE;
}

static void
complete_aps(NMDevice  **devices,
             const char *ifname,
             const char *bssid_prefix,
             const char *ssid_prefix)
{
    int       devices_idx = 0;
    NMDevice *device;

    while ((device = find_wifi_device_by_iface(devices, ifname, &devices_idx)))
        find_ap_on_device(device, bssid_prefix, ssid_prefix, TRUE);
}

void
nmc_complete_bssid(NMClient *client, const char *ifname, const char *bssid_prefix)
{
    gs_free NMDevice **devices = NULL;

    devices = nmc_get_devices_sorted(client);
    complete_aps(devices, ifname, bssid_prefix, NULL);
}

static void
do_device_wifi_list(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    GError                          *error      = NULL;
    NMDevice                        *device     = NULL;
    const char                      *ifname     = NULL;
    const char                      *bssid_user = NULL;
    const char                      *rescan     = NULL;
    gs_free NMDevice               **devices    = NULL;
    const char                      *fields_str = NULL;
    const NMMetaAbstractInfo *const *tmpl;
    gs_unref_array GArray           *out_indices = NULL;
    int                              option;
    gint64                           rescan_cutoff_msec;
    ScanInfo                        *scan_info = NULL;
    gboolean                         ifname_handled;
    NMDevice                        *ifname_handled_candidate;
    guint                            i, j;

    devices = nmc_get_devices_sorted(nmc->client);

    while ((option = next_arg(nmc, &argc, &argv, "ifname", "hwaddr", "bssid", "--rescan", NULL))
           > 0) {
        switch (option) {
        case 1: /* ifname */
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            ifname = *argv;
            if (argc == 1 && nmc->complete)
                complete_device(devices, ifname, TRUE);
            break;
        case 2: /* hwaddr is deprecated and will be removed later */
        case 3: /* bssid */
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            bssid_user = *argv;
            if (argc == 1 && nmc->complete)
                complete_aps(devices, NULL, bssid_user, NULL);
            break;
        case 4: /* --rescan */
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            rescan = *argv;
            if (argc == 1 && nmc->complete)
                nmc_complete_strings(rescan, "auto", "no", "yes");
            break;
        default:
            nm_assert_not_reached();
            break;
        }
    }

    if (nmc->complete)
        return;

    if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "common") == 0)
        fields_str = NMC_FIELDS_DEV_WIFI_LIST_COMMON;
    else if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "all") == 0) {
        /* pass */
    } else
        fields_str = nmc->required_fields;

    tmpl        = (const NMMetaAbstractInfo *const *) nmc_fields_dev_wifi_list;
    out_indices = parse_output_fields(fields_str, tmpl, FALSE, NULL, &error);

    if (error) {
        g_string_printf(nmc->return_text, _("Error: 'device wifi': %s"), error->message);
        g_error_free(error);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (argc) {
        g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (NM_IN_STRSET(rescan, NULL, "auto"))
        rescan_cutoff_msec = nm_utils_get_timestamp_msec() - (30 * NM_UTILS_MSEC_PER_SEC);
    else if (nm_streq(rescan, "no"))
        rescan_cutoff_msec = G_MININT64;
    else if (nm_streq(rescan, "yes"))
        rescan_cutoff_msec = nm_utils_get_timestamp_msec();
    else {
        g_string_printf(nmc->return_text,
                        _("Error: invalid rescan argument: '%s' not among [auto, no, yes]"),
                        rescan);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    ifname_handled           = (ifname == NULL);
    ifname_handled_candidate = NULL;

    j = 0;
    for (i = 0; devices[i]; i++) {
        const char *dev_iface;

        device    = devices[i];
        dev_iface = nm_device_get_iface(device);

        if (ifname) {
            if (!nm_streq0(ifname, dev_iface))
                continue;
            if (!NM_IS_DEVICE_WIFI(device)) {
                if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_GENERIC
                    && nm_streq0(nm_device_get_type_description(device), "wifi"))
                    ifname_handled_candidate = device;
                else if (!ifname_handled_candidate)
                    ifname_handled_candidate = device;
                continue;
            }
            ifname_handled = TRUE;
        } else {
            if (!NM_IS_DEVICE_WIFI(device))
                continue;
        }

        devices[j++] = device;
    }
    devices[j] = NULL;

    if (!ifname_handled) {
        if (!ifname_handled_candidate) {
            g_string_printf(nmc->return_text, _("Error: Device '%s' not found."), ifname);
        } else if (nm_device_get_device_type(ifname_handled_candidate) == NM_DEVICE_TYPE_GENERIC
                   && nm_streq0(nm_device_get_type_description(ifname_handled_candidate), "wifi")) {
            g_string_printf(nmc->return_text,
                            _("Error: Device '%s' was not recognized as a Wi-Fi device, check "
                              "NetworkManager Wi-Fi plugin."),
                            ifname);
        } else {
            g_string_printf(nmc->return_text,
                            _("Error: Device '%s' is not a Wi-Fi device."),
                            ifname);
        }
        nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        return;
    }

    if (!devices[0]) {
        if (bssid_user) {
            nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
            g_string_printf(nmc->return_text,
                            _("Error: Access point with bssid '%s' not found."),
                            bssid_user);
            nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        }
        return;
    }

    scan_info  = g_slice_new(ScanInfo);
    *scan_info = (ScanInfo) {
        .out_indices        = g_array_ref(out_indices),
        .tmpl               = tmpl,
        .bssid_user         = g_strdup(bssid_user),
        .nmc                = nmc,
        .rescan_cutoff_msec = rescan_cutoff_msec,
    };

    for (i = 0; devices[i]; i++)
        g_object_ref(devices[i]);

    for (i = 0; devices[i]; i++) {
        NMDeviceWifi *wifi = NM_DEVICE_WIFI(devices[i]);
        WifiListData *wifi_list_data;
        int           timeout_msec;

        if (rescan_cutoff_msec <= _device_wifi_get_last_scan(wifi))
            timeout_msec = 0;
        else
            timeout_msec = 15000;

        wifi_list_data  = g_slice_new(WifiListData);
        *wifi_list_data = (WifiListData) {
            .wifi       = wifi,
            .scan_info  = scan_info,
            .timeout_id = g_timeout_add(timeout_msec, wifi_list_scan_timeout, wifi_list_data),
        };

        scan_info->pending++;

        if (timeout_msec > 0) {
            wifi_list_data->last_scan_id     = g_signal_connect(wifi,
                                                            "notify::" NM_DEVICE_WIFI_LAST_SCAN,
                                                            G_CALLBACK(wifi_last_scan_updated),
                                                            wifi_list_data),
            wifi_list_data->scan_cancellable = g_cancellable_new(),
            nm_device_wifi_request_scan_async(wifi,
                                              wifi_list_data->scan_cancellable,
                                              wifi_list_rescan_cb,
                                              wifi_list_data);
        }
    }

    scan_info->devices = g_steal_pointer(&devices);

    nmc->should_wait++;
}

static void
activate_update2_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    NMRemoteConnection        *remote_con = NM_REMOTE_CONNECTION(source_object);
    AddAndActivateInfo        *info       = user_data;
    NmCli                     *nmc        = info->nmc;
    gs_unref_variant GVariant *ret        = NULL;
    GError                    *error      = NULL;

    ret = nm_remote_connection_update2_finish(remote_con, res, &error);

    if (!ret) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        g_error_free(error);
        quit();
        add_and_activate_info_free(info);
        return;
    }

    nm_client_activate_connection_async(nmc->client,
                                        NM_CONNECTION(remote_con),
                                        info->device,
                                        info->specific_object,
                                        NULL,
                                        add_and_activate_cb,
                                        info);
}

static void
save_and_activate_connection(NmCli        *nmc,
                             NMDevice     *device,
                             NMConnection *connection,
                             gboolean      hotspot,
                             const char   *specific_object)
{
    AddAndActivateInfo *info;

    info = add_and_activate_info_new(nmc,
                                     device,
                                     hotspot,
                                     !NM_IS_REMOTE_CONNECTION(connection),
                                     specific_object);

    if (NM_IS_REMOTE_CONNECTION(connection)) {
        nm_remote_connection_update2(NM_REMOTE_CONNECTION(connection),
                                     nm_connection_to_dbus(connection, NM_CONNECTION_SERIALIZE_ALL),
                                     NM_SETTINGS_UPDATE2_FLAG_BLOCK_AUTOCONNECT,
                                     NULL,
                                     NULL,
                                     activate_update2_cb,
                                     info);
    } else {
        nm_client_add_and_activate_connection_async(nmc->client,
                                                    connection,
                                                    info->device,
                                                    info->specific_object,
                                                    NULL,
                                                    add_and_activate_cb,
                                                    info);
    }
}

static void
do_device_wifi_connect(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice                     *device = NULL;
    NMAccessPoint                *ap     = NULL;
    NM80211ApFlags                ap_flags;
    NM80211ApSecurityFlags        ap_wpa_flags;
    NM80211ApSecurityFlags        ap_rsn_flags;
    gs_unref_object NMConnection *connection = NULL;
    NMSettingConnection          *s_con;
    NMSettingWireless            *s_wifi;
    const char                   *param_user = NULL;
    const char                   *ifname     = NULL;
    const char                   *bssid      = NULL;
    const char                   *password   = NULL;
    const char                   *con_name   = NULL;
    gboolean private                         = FALSE;
    gboolean           hidden                = FALSE;
    gboolean           wep_passphrase        = FALSE;
    GByteArray        *bssid1_arr            = NULL;
    GByteArray        *bssid2_arr            = NULL;
    gs_free NMDevice **devices               = NULL;
    int                devices_idx;
    char              *ssid_ask = NULL;
    const GPtrArray   *avail_cons;
    gboolean           name_match = FALSE;
    int                i;

    /* Set default timeout waiting for operation completion. */
    if (nmc->timeout == -1)
        nmc->timeout = 90;

    devices = nmc_get_devices_sorted(nmc->client);

    next_arg(nmc, &argc, &argv, NULL);
    /* Get the first compulsory argument (SSID or BSSID) */
    if (argc > 0) {
        param_user = *argv;
        bssid1_arr = nm_utils_hwaddr_atoba(param_user, ETH_ALEN);

        if (argc == 1 && nmc->complete)
            complete_aps(devices, NULL, param_user, param_user);

        next_arg(nmc, &argc, &argv, NULL);
    } else {
        /* nmc_do_cmd() should not call this with argc=0. */
        nm_assert(!nmc->complete);

        if (nmc->ask) {
            ssid_ask   = nmc_readline(&nmc->nmc_config, _("SSID or BSSID: "));
            param_user = ssid_ask ?: "";
            bssid1_arr = nm_utils_hwaddr_atoba(param_user, ETH_ALEN);
        }
        if (!ssid_ask) {
            g_string_printf(nmc->return_text, _("Error: SSID or BSSID are missing."));
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            goto finish;
        }
    }

    /* Get the rest of the parameters */
    while (argc > 0) {
        if (argc == 1 && nmc->complete) {
            nmc_complete_strings(*argv,
                                 "ifname",
                                 "bssid",
                                 "password",
                                 "wep-key-type",
                                 "name",
                                 "private",
                                 "hidden");
        }

        if (strcmp(*argv, "ifname") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            ifname = *argv;
            if (argc == 1 && nmc->complete)
                complete_device(devices, ifname, TRUE);
        } else if (strcmp(*argv, "bssid") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            bssid = *argv;
            if (argc == 1 && nmc->complete)
                complete_aps(devices, NULL, bssid, NULL);
            bssid2_arr = nm_utils_hwaddr_atoba(bssid, ETH_ALEN);
            if (!bssid2_arr) {
                g_string_printf(nmc->return_text,
                                _("Error: bssid argument value '%s' is not a valid BSSID."),
                                bssid);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
        } else if (strcmp(*argv, "password") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            password = *argv;
        } else if (strcmp(*argv, "wep-key-type") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            if (argc == 1 && nmc->complete)
                nmc_complete_strings(*argv, "key", "phrase");
            if (strcmp(*argv, "key") == 0)
                wep_passphrase = FALSE;
            else if (strcmp(*argv, "phrase") == 0)
                wep_passphrase = TRUE;
            else {
                g_string_printf(
                    nmc->return_text,
                    _("Error: wep-key-type argument value '%s' is invalid, use 'key' or 'phrase'."),
                    *argv);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
        } else if (strcmp(*argv, "name") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            con_name = *argv;
        } else if (strcmp(*argv, "private") == 0) {
            GError *err_tmp = NULL;

            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            if (argc == 1 && nmc->complete)
                nmc_complete_bool(*argv);
            if (!nmc_string_to_bool(*argv, &private, &err_tmp)) {
                g_string_printf(nmc->return_text,
                                _("Error: %s: %s."),
                                *(argv - 1),
                                err_tmp->message);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                g_clear_error(&err_tmp);
                goto finish;
            }
        } else if (strcmp(*argv, "hidden") == 0) {
            GError *err_tmp = NULL;

            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                goto finish;
            }
            if (argc == 1 && nmc->complete)
                nmc_complete_bool(*argv);
            if (!nmc_string_to_bool(*argv, &hidden, &err_tmp)) {
                g_string_printf(nmc->return_text,
                                _("Error: %s: %s."),
                                *(argv - 1),
                                err_tmp->message);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                g_clear_error(&err_tmp);
                goto finish;
            }
        } else if (!nmc->complete) {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            goto finish;
        }

        next_arg(nmc, &argc, &argv, NULL);
    }

    if (nmc->complete)
        goto finish;

    /* Verify SSID/BSSID parameters */
    if (bssid1_arr && bssid2_arr && memcmp(bssid1_arr->data, bssid2_arr->data, ETH_ALEN)) {
        g_string_printf(nmc->return_text,
                        _("Error: BSSID to connect to (%s) differs from bssid argument (%s)."),
                        param_user,
                        bssid);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        goto finish;
    }
    if (!bssid1_arr && strlen(param_user) > 32) {
        g_string_printf(nmc->return_text,
                        _("Error: Parameter '%s' is neither SSID nor BSSID."),
                        param_user);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        goto finish;
    }

    /* Find a device to activate the connection on */
    devices_idx = 0;
    device      = find_wifi_device_by_iface(devices, ifname, &devices_idx);

    if (!device) {
        if (ifname)
            g_string_printf(nmc->return_text,
                            _("Error: Device '%s' is not a Wi-Fi device."),
                            ifname);
        else
            g_string_printf(nmc->return_text, _("Error: No Wi-Fi device found."));
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        goto finish;
    }

    /* For hidden SSID first scan it so that NM learns about the AP */
    if (hidden) {
        GVariantBuilder builder, array_builder;
        GVariant       *options;
        GError         *scan_err = NULL;

        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_init(&array_builder, G_VARIANT_TYPE("aay"));
        g_variant_builder_add(&array_builder,
                              "@ay",
                              nm_g_variant_new_ay((const guint8 *) param_user, strlen(param_user)));
        g_variant_builder_add(&builder, "{sv}", "ssids", g_variant_builder_end(&array_builder));
        options = g_variant_builder_end(&builder);

        nm_device_wifi_request_scan_options(NM_DEVICE_WIFI(device), options, NULL, &scan_err);
        if (scan_err) {
            g_string_printf(nmc->return_text,
                            _("Error: Failed to scan hidden SSID: %s."),
                            scan_err->message);
            g_clear_error(&scan_err);
            nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
            goto finish;
        }
    }

    /* Find an AP to connect to */
    ap = find_ap_on_device(device,
                           bssid1_arr ? param_user : bssid,
                           bssid1_arr ? NULL : param_user,
                           FALSE);
    if (!ap && !ifname) {
        NMDevice *dev;

        /* AP not found, ifname was not specified, so try finding the AP on another device. */
        while ((dev = find_wifi_device_by_iface(devices, NULL, &devices_idx)) != NULL) {
            ap = find_ap_on_device(dev,
                                   bssid1_arr ? param_user : bssid,
                                   bssid1_arr ? NULL : param_user,
                                   FALSE);
            if (ap) {
                device = dev;
                break;
            }
        }
    }

    if (!ap) {
        if (!bssid1_arr)
            g_string_printf(nmc->return_text,
                            _("Error: No network with SSID '%s' found."),
                            param_user);
        else
            g_string_printf(nmc->return_text,
                            _("Error: No access point with BSSID '%s' found."),
                            param_user);
        nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        goto finish;
    }

    avail_cons = nm_device_get_available_connections(device);
    for (i = 0; i < avail_cons->len; i++) {
        NMConnection *avail_con = g_ptr_array_index(avail_cons, i);
        const char   *id        = nm_connection_get_id(NM_CONNECTION(avail_con));

        if (con_name) {
            if (!id || strcmp(id, con_name))
                continue;

            name_match = TRUE;
        }

        if (nm_access_point_connection_valid(ap, NM_CONNECTION(avail_con))) {
            /* ap has been checked against bssid1, bssid2 and the ssid
             * and now avail_con has been checked against ap.
             */
            connection = g_object_ref(avail_con);
            break;
        }
    }

    if (name_match && !connection) {
        g_string_printf(nmc->return_text,
                        _("Error: Connection '%s' exists but properties don't match."),
                        con_name);
        nmc->return_value = NMC_RESULT_ERROR_NOT_FOUND;
        goto finish;
    }

    if (!connection) {
        /* If there are some connection data from user, create a connection and
         * fill them into proper settings. */
        if (con_name || private || bssid2_arr || hidden)
            connection = nm_simple_connection_new();

        if (con_name || private) {
            s_con = (NMSettingConnection *) nm_setting_connection_new();
            nm_connection_add_setting(connection, NM_SETTING(s_con));

            /* Set user provided connection name */
            if (con_name)
                g_object_set(s_con, NM_SETTING_CONNECTION_ID, con_name, NULL);

            /* Connection will only be visible to this user when 'private' is specified */
            if (private)
                nm_setting_connection_add_permission(s_con,
                                                     NM_SETTINGS_CONNECTION_PERMISSION_USER,
                                                     g_get_user_name() ?: "",
                                                     NULL);
        }
        if (bssid2_arr || hidden) {
            s_wifi = (NMSettingWireless *) nm_setting_wireless_new();
            nm_connection_add_setting(connection, NM_SETTING(s_wifi));

            /* 'bssid' parameter is used to restrict the connection only to the BSSID */
            if (bssid2_arr)
                g_object_set(s_wifi, NM_SETTING_WIRELESS_BSSID, bssid2_arr, NULL);

            /* 'hidden' parameter is used to indicate that SSID is not broadcasted */
            if (hidden) {
                GBytes *ssid = g_bytes_new(param_user, strlen(param_user));

                g_object_set(s_wifi,
                             NM_SETTING_WIRELESS_SSID,
                             ssid,
                             NM_SETTING_WIRELESS_HIDDEN,
                             hidden,
                             NULL);
                g_bytes_unref(ssid);

                /* Warn when the provided AP identifier looks like BSSID instead of SSID */
                if (bssid1_arr)
                    nmc_printerr(
                        _("Warning: '%s' should be SSID for hidden APs; but it looks like a "
                          "BSSID.\n"),
                        param_user);
            }
        }
    }

    /* handle password */
    ap_flags     = nm_access_point_get_flags(ap);
    ap_wpa_flags = nm_access_point_get_wpa_flags(ap);
    ap_rsn_flags = nm_access_point_get_rsn_flags(ap);

    /* Set password for WEP or WPA-PSK. */
    if ((ap_flags & NM_802_11_AP_FLAGS_PRIVACY)
        || (ap_wpa_flags != NM_802_11_AP_SEC_NONE
            && !NM_FLAGS_ANY(ap_wpa_flags,
                             NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM))
        || (ap_rsn_flags != NM_802_11_AP_SEC_NONE
            && !NM_FLAGS_ANY(ap_rsn_flags,
                             NM_802_11_AP_SEC_KEY_MGMT_OWE | NM_802_11_AP_SEC_KEY_MGMT_OWE_TM))) {
        NMSettingWirelessSecurity *s_wsec = NULL;

        /* Create secret agent */
        nmc->secret_agent = nm_secret_agent_simple_new("nmcli-connect");
        if (nmc->secret_agent) {
            g_signal_connect(nmc->secret_agent,
                             NM_SECRET_AGENT_SIMPLE_REQUEST_SECRETS,
                             G_CALLBACK(nmc_secrets_requested),
                             nmc);
        }

        if (password) {
            if (!connection)
                connection = nm_simple_connection_new();
            if (!s_wsec) {
                s_wsec = (NMSettingWirelessSecurity *) nm_setting_wireless_security_new();
                nm_connection_add_setting(connection, NM_SETTING(s_wsec));
            }

            if (ap_wpa_flags == NM_802_11_AP_SEC_NONE && ap_rsn_flags == NM_802_11_AP_SEC_NONE) {
                /* WEP */
                nm_setting_wireless_security_set_wep_key(s_wsec, 0, password);
                g_object_set(G_OBJECT(s_wsec),
                             NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
                             wep_passphrase ? NM_WEP_KEY_TYPE_PASSPHRASE : NM_WEP_KEY_TYPE_KEY,
                             NULL);
            } else if ((ap_wpa_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK)
                       || (ap_rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_PSK)
                       || (ap_rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_SAE)) {
                /* WPA PSK */
                g_object_set(s_wsec, NM_SETTING_WIRELESS_SECURITY_PSK, password, NULL);
            }
        }
    }
    // FIXME: Creating WPA-Enterprise connections is not supported yet.
    // We are not able to determine and fill all the parameters for
    // 802.1X authentication automatically without user providing
    // the data. Adding nmcli options for the 8021x setting would
    // clutter the command. However, that could be solved later by
    // implementing add/edit connections support for nmcli.

    /* nowait_flag indicates user input. should_wait says whether quit in start().
     * We have to delay exit after add_and_activate_cb() is called, even if
     * the user doesn't want to wait, in order to give NM time to check our
     * permissions. */
    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    save_and_activate_connection(nmc, device, connection, FALSE, nm_object_get_path(NM_OBJECT(ap)));

finish:
    if (bssid1_arr)
        g_byte_array_free(bssid1_arr, TRUE);
    if (bssid2_arr)
        g_byte_array_free(bssid2_arr, TRUE);
    g_free(ssid_ask);
}

static GBytes *
generate_ssid_for_hotspot(void)
{
    GBytes *ssid_bytes;
    char   *ssid = NULL;

    ssid = g_strdup_printf("Hotspot-%s", g_get_host_name());
    if (strlen(ssid) > 32)
        ssid[32] = '\0';
    ssid_bytes = g_bytes_new(ssid, strlen(ssid));
    g_free(ssid);

    return ssid_bytes;
}

#define WPA_PASSKEY_SIZE 12
static void
generate_wpa_key(char *key, size_t len)
{
    guint i;

    g_return_if_fail(key);
    g_return_if_fail(len > WPA_PASSKEY_SIZE);

    for (i = 0; i < WPA_PASSKEY_SIZE; i++) {
        int c;

        do {
            c = nm_random_u64_range(48, 122);
            /* skip characters that look similar */
        } while (NM_IN_SET(c, '1', 'l', 'I', '0', 'O', 'Q', '8', 'B', '5', 'S')
                 || !g_ascii_isalnum(c));

        key[i] = (char) c;
    }
    key[WPA_PASSKEY_SIZE] = '\0';
}

static void
generate_wep_key(char *key, size_t len)
{
    int         i;
    const char *hexdigits = "0123456789abcdef";

    g_return_if_fail(key);
    g_return_if_fail(len > 10);

    /* generate a 10-digit hex WEP key */
    for (i = 0; i < 10; i++) {
        int digit;

        digit  = nm_random_u64_range(0, 16);
        key[i] = hexdigits[digit];
    }
    key[10] = '\0';
}

static gboolean
set_wireless_security_for_hotspot(NMSettingWirelessSecurity *s_wsec,
                                  const char                *wifi_mode,
                                  NMDeviceWifiCapabilities   caps,
                                  const char                *password,
                                  gboolean                   show_password,
                                  GError                   **error)
{
    char        generated_key[20];
    const char *key;
    const char *key_mgmt;

    if (g_strcmp0(wifi_mode, NM_SETTING_WIRELESS_MODE_AP) == 0) {
        if (caps & NM_WIFI_DEVICE_CAP_RSN) {
            nm_setting_wireless_security_add_proto(s_wsec, "rsn");
            nm_setting_wireless_security_add_pairwise(s_wsec, "ccmp");
            nm_setting_wireless_security_add_group(s_wsec, "ccmp");
            key_mgmt = "wpa-psk";
        } else if (caps & NM_WIFI_DEVICE_CAP_WPA) {
            nm_setting_wireless_security_add_proto(s_wsec, "wpa");
            nm_setting_wireless_security_add_pairwise(s_wsec, "tkip");
            nm_setting_wireless_security_add_group(s_wsec, "tkip");
            key_mgmt = "wpa-psk";
        } else
            key_mgmt = "none";
    } else
        key_mgmt = "none";

    if (g_strcmp0(key_mgmt, "wpa-psk") == 0) {
        /* use WPA */
        if (password) {
            if (!nm_utils_wpa_psk_valid(password)) {
                g_set_error(error, NMCLI_ERROR, 0, _("'%s' is not valid WPA PSK"), password);
                return FALSE;
            }
            key = password;
        } else {
            generate_wpa_key(generated_key, sizeof(generated_key));
            key = generated_key;
        }
        g_object_set(s_wsec,
                     NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                     key_mgmt,
                     NM_SETTING_WIRELESS_SECURITY_PSK,
                     key,
                     NULL);
    } else {
        /* use WEP */
        if (password) {
            if (!nm_utils_wep_key_valid(password, NM_WEP_KEY_TYPE_KEY)) {
                g_set_error(error,
                            NMCLI_ERROR,
                            0,
                            _("'%s' is not valid WEP key (it should be 5 or 13 ASCII chars)"),
                            password);
                return FALSE;
            }
            key = password;
        } else {
            generate_wep_key(generated_key, sizeof(generated_key));
            key = generated_key;
        }
        g_object_set(s_wsec,
                     NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                     key_mgmt,
                     NM_SETTING_WIRELESS_SECURITY_WEP_KEY0,
                     key,
                     NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
                     NM_WEP_KEY_TYPE_KEY,
                     NULL);
    }
    if (show_password)
        nmc_print(_("Hotspot password: %s\n"), key);

    return TRUE;
}

static NMConnection *
find_hotspot_conn(NMDevice        *device,
                  const GPtrArray *connections,
                  const char      *con_name,
                  GBytes          *ssid_bytes,
                  const char      *wifi_mode,
                  const char      *band,
                  gint64           channel_int)
{
    NMConnection      *connection;
    NMSettingWireless *s_wifi;
    int                i;

    for (i = 0; i < connections->len; i++) {
        connection = NM_CONNECTION(connections->pdata[i]);

        s_wifi = nm_connection_get_setting_wireless(connection);
        if (!s_wifi)
            continue;

        if (channel_int != -1 && nm_setting_wireless_get_channel(s_wifi) != channel_int)
            continue;

        if (g_strcmp0(nm_setting_wireless_get_mode(s_wifi), wifi_mode) != 0)
            continue;

        if (band && g_strcmp0(nm_setting_wireless_get_band(s_wifi), band) != 0)
            continue;

        if (ssid_bytes && !g_bytes_equal(nm_setting_wireless_get_ssid(s_wifi), ssid_bytes))
            continue;

        if (!nm_device_connection_compatible(device, connection, NULL))
            continue;

        return g_object_ref(connection);
    }

    return NULL;
}

static NMConnection *
create_hotspot_conn(const GPtrArray *connections,
                    const char      *con_name,
                    GBytes          *ssid_bytes,
                    const char      *wifi_mode,
                    const char      *band,
                    gint64           channel_int)
{
    char                      *default_name = NULL;
    NMConnection              *connection;
    NMSettingConnection       *s_con;
    NMSettingWireless         *s_wifi;
    NMSettingWirelessSecurity *s_wsec;
    NMSettingIPConfig         *s_ip4, *s_ip6;
    NMSettingProxy            *s_proxy;

    nm_assert(channel_int == -1 || band);

    connection = nm_simple_connection_new();
    s_con      = (NMSettingConnection *) nm_setting_connection_new();
    nm_connection_add_setting(connection, NM_SETTING(s_con));
    if (!con_name)
        con_name = default_name = nmc_unique_connection_name(connections, "Hotspot");
    g_object_set(s_con,
                 NM_SETTING_CONNECTION_ID,
                 con_name,
                 NM_SETTING_CONNECTION_AUTOCONNECT,
                 FALSE,
                 NULL);
    g_free(default_name);

    s_wifi = (NMSettingWireless *) nm_setting_wireless_new();
    nm_connection_add_setting(connection, NM_SETTING(s_wifi));

    g_object_set(s_wifi,
                 NM_SETTING_WIRELESS_MODE,
                 wifi_mode,
                 NM_SETTING_WIRELESS_SSID,
                 ssid_bytes,
                 NULL);

    if (channel_int != -1) {
        g_object_set(s_wifi,
                     NM_SETTING_WIRELESS_CHANNEL,
                     (guint32) channel_int,
                     NM_SETTING_WIRELESS_BAND,
                     band,
                     NULL);
    } else if (band) {
        g_object_set(s_wifi, NM_SETTING_WIRELESS_BAND, band, NULL);
    }

    s_wsec = (NMSettingWirelessSecurity *) nm_setting_wireless_security_new();
    nm_connection_add_setting(connection, NM_SETTING(s_wsec));

    s_ip4 = (NMSettingIPConfig *) nm_setting_ip4_config_new();
    nm_connection_add_setting(connection, NM_SETTING(s_ip4));
    g_object_set(s_ip4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_SHARED, NULL);

    s_ip6 = (NMSettingIPConfig *) nm_setting_ip6_config_new();
    nm_connection_add_setting(connection, NM_SETTING(s_ip6));
    g_object_set(s_ip6, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_IGNORE, NULL);

    s_proxy = (NMSettingProxy *) nm_setting_proxy_new();
    nm_connection_add_setting(connection, NM_SETTING(s_proxy));
    g_object_set(s_proxy, NM_SETTING_PROXY_METHOD, (int) NM_SETTING_PROXY_METHOD_NONE, NULL);

    return connection;
}

static void
do_device_wifi_hotspot(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    const char                   *ifname     = NULL;
    const char                   *con_name   = NULL;
    gs_unref_bytes GBytes        *ssid_bytes = NULL;
    const char                   *wifi_mode;
    const char                   *band          = NULL;
    const char                   *channel       = NULL;
    gint64                        channel_int   = -1;
    const char                   *password      = NULL;
    gboolean                      show_password = FALSE;
    NMDevice                     *device        = NULL;
    gs_free NMDevice            **devices       = NULL;
    NMDeviceWifiCapabilities      caps;
    gs_unref_object NMConnection *connection = NULL;
    const GPtrArray              *connections;
    NMSettingWirelessSecurity    *s_wsec;
    GError                       *error = NULL;

    /* Set default timeout waiting for operation completion. */
    if (nmc->timeout == -1)
        nmc->timeout = 60;

    devices = nmc_get_devices_sorted(nmc->client);

    next_arg(nmc, &argc, &argv, NULL);
    while (argc > 0) {
        if (argc == 1 && nmc->complete) {
            nmc_complete_strings(*argv,
                                 "ifname",
                                 "con-name",
                                 "ssid",
                                 "band",
                                 "channel",
                                 "password");
        }

        if (strcmp(*argv, "ifname") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            ifname = *argv;
            if (argc == 1 && nmc->complete)
                complete_device(devices, ifname, TRUE);
        } else if (strcmp(*argv, "con-name") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            con_name = *argv;
        } else if (strcmp(*argv, "ssid") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            if (strlen(*argv) > 32) {
                g_string_printf(nmc->return_text, _("Error: ssid is too long."));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            ssid_bytes = g_bytes_new(*argv, strlen(*argv));
        } else if (strcmp(*argv, "band") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            band = *argv;
            if (argc == 1 && nmc->complete)
                nmc_complete_strings(band, "a", "bg");
            if (strcmp(band, "a") && strcmp(band, "bg")) {
                g_string_printf(nmc->return_text,
                                _("Error: band argument value '%s' is invalid; use 'a' or 'bg'."),
                                band);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
        } else if (strcmp(*argv, "channel") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            channel = *argv;
        } else if (strcmp(*argv, "password") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            password = *argv;
            /* --show-password is deprecated in favour of global --show-secrets option */
            /* Keep it here for backwards compatibility */
        } else if (nmc_arg_is_option(*argv, "show-password")) {
            show_password = TRUE;
        } else {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        next_arg(nmc, &argc, &argv, NULL);
    }
    show_password = nmc->nmc_config.show_secrets || show_password;

    if (nmc->complete)
        return;

    if (channel) {
        unsigned long int value;

        if (!band) {
            g_string_printf(nmc->return_text, _("Error: channel requires band too."));
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }
        if (!nmc_string_to_uint(channel, TRUE, 1, 5825, &value)
            || !nm_utils_wifi_is_channel_valid(value, band)) {
            g_string_printf(nmc->return_text,
                            _("Error: channel '%s' not valid for band '%s'."),
                            channel,
                            band);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        channel_int = value;
    }

    /* Find Wi-Fi device. When no ifname is provided, the first Wi-Fi is used. */
    device = find_wifi_device_by_iface(devices, ifname, NULL);
    if (!device) {
        if (ifname)
            g_string_printf(nmc->return_text,
                            _("Error: Device '%s' is not a Wi-Fi device."),
                            ifname);
        else
            g_string_printf(nmc->return_text, _("Error: No Wi-Fi device found."));
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        return;
    }

    /* Check device supported mode */
    caps = nm_device_wifi_get_capabilities(NM_DEVICE_WIFI(device));
    if (caps & NM_WIFI_DEVICE_CAP_AP)
        wifi_mode = NM_SETTING_WIRELESS_MODE_AP;
    else if (caps & NM_WIFI_DEVICE_CAP_ADHOC)
        wifi_mode = NM_SETTING_WIRELESS_MODE_ADHOC;
    else {
        g_string_printf(nmc->return_text,
                        _("Error: Device '%s' supports neither AP nor Ad-Hoc mode."),
                        nm_device_get_iface(device));
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        return;
    }

    connections = nm_client_get_connections(nmc->client);
    connection =
        find_hotspot_conn(device, connections, con_name, ssid_bytes, wifi_mode, band, channel_int);
    if (!connection) {
        /* Create a connection with appropriate parameters */
        if (!ssid_bytes)
            ssid_bytes = generate_ssid_for_hotspot();
        connection =
            create_hotspot_conn(connections, con_name, ssid_bytes, wifi_mode, band, channel_int);
    }

    if (password || !NM_IS_REMOTE_CONNECTION(connection)) {
        s_wsec = nm_connection_get_setting_wireless_security(connection);
        g_return_if_fail(s_wsec);

        if (!set_wireless_security_for_hotspot(s_wsec,
                                               wifi_mode,
                                               caps,
                                               password,
                                               show_password,
                                               &error)) {
            g_string_printf(nmc->return_text, _("Error: Invalid 'password': %s."), error->message);
            g_clear_error(&error);
            nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
            return;
        }
    }

    /* Activate the connection now */
    nmc->nowait_flag = (nmc->timeout == 0);
    nmc->should_wait++;

    save_and_activate_connection(nmc, device, connection, TRUE, NULL);
}

static void
request_rescan_cb(GObject *object, GAsyncResult *result, gpointer user_data)
{
    NmCli  *nmc   = (NmCli *) user_data;
    GError *error = NULL;

    nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(object), result, &error);
    if (error) {
        g_string_printf(nmc->return_text, _("Error: %s."), error->message);
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        g_error_free(error);
    }
    quit();
}

static void
do_device_wifi_rescan(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice                    *device;
    const char                  *ifname  = NULL;
    gs_unref_ptrarray GPtrArray *ssids   = NULL;
    gs_free NMDevice           **devices = NULL;
    GVariantBuilder              builder, array_builder;
    GVariant                    *options;
    int                          i;

    ssids   = g_ptr_array_new();
    devices = nmc_get_devices_sorted(nmc->client);

    next_arg(nmc, &argc, &argv, NULL);
    /* Get the parameters */
    while (argc > 0) {
        if (argc == 1 && nmc->complete)
            nmc_complete_strings(*argv, "ifname", "ssid");

        if (strcmp(*argv, "ifname") == 0) {
            if (ifname) {
                g_string_printf(nmc->return_text, _("Error: '%s' cannot repeat."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            ifname = *argv;
            if (argc == 1 && nmc->complete)
                complete_device(devices, ifname, TRUE);
        } else if (strcmp(*argv, "ssid") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            g_ptr_array_add(ssids, (gpointer) *argv);
        } else if (!nmc->complete) {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        next_arg(nmc, &argc, &argv, NULL);
    }

    if (nmc->complete)
        return;

    /* Find Wi-Fi device to scan on. When no ifname is provided, the first Wi-Fi is used. */
    device = find_wifi_device_by_iface(devices, ifname, NULL);

    if (!device) {
        if (ifname)
            g_string_printf(nmc->return_text,
                            _("Error: Device '%s' is not a Wi-Fi device."),
                            ifname);
        else
            g_string_printf(nmc->return_text, _("Error: No Wi-Fi device found."));
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        return;
    }

    if (ssids->len) {
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_init(&array_builder, G_VARIANT_TYPE("aay"));

        for (i = 0; i < ssids->len; i++) {
            const char *ssid = g_ptr_array_index(ssids, i);

            g_variant_builder_add(&array_builder,
                                  "@ay",
                                  nm_g_variant_new_ay((const guint8 *) ssid, strlen(ssid)));
        }

        g_variant_builder_add(&builder, "{sv}", "ssids", g_variant_builder_end(&array_builder));
        options = g_variant_builder_end(&builder);

        nm_device_wifi_request_scan_options_async(NM_DEVICE_WIFI(device),
                                                  options,
                                                  NULL,
                                                  request_rescan_cb,
                                                  nmc);
    } else
        nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(device), NULL, request_rescan_cb, nmc);

    nmc->should_wait++;
}

static void
string_append_mecard(GString *string, const char *tag, const char *text)
{
    const char *p;
    bool        is_hex = TRUE;
    int         start;

    if (!text)
        return;

    g_string_append(string, tag);
    start = string->len;

    for (p = text; *p; p++) {
        if (!g_ascii_isxdigit(*p))
            is_hex = FALSE;
        if (strchr("\\\":;,", *p))
            g_string_append_c(string, '\\');
        g_string_append_c(string, *p);
    }

    if (is_hex) {
        g_string_insert_c(string, start, '\"');
        g_string_append_c(string, '\"');
    }
    g_string_append_c(string, ';');
}

static void
print_wifi_connection(const NmcConfig *nmc_config, NMConnection *connection)
{
    NMSettingWireless            *s_wireless;
    NMSettingWirelessSecurity    *s_wsec;
    const char                   *key_mgmt = NULL;
    const char                   *psk      = NULL;
    const char                   *type     = NULL;
    GBytes                       *ssid_bytes;
    gs_free char                 *ssid   = NULL;
    nm_auto_free_gstring GString *string = NULL;

    s_wireless = nm_connection_get_setting_wireless(connection);
    g_return_if_fail(s_wireless);

    ssid_bytes = nm_setting_wireless_get_ssid(s_wireless);
    g_return_if_fail(ssid_bytes);
    ssid = nm_utils_ssid_to_utf8(g_bytes_get_data(ssid_bytes, NULL), g_bytes_get_size(ssid_bytes));
    g_return_if_fail(ssid);
    nmc_print("SSID: %s\n", ssid);

    string = g_string_sized_new(64);
    g_string_append(string, "WIFI:");

    s_wsec = nm_connection_get_setting_wireless_security(connection);
    if (s_wsec) {
        key_mgmt = nm_setting_wireless_security_get_key_mgmt(s_wsec);
        psk      = nm_setting_wireless_security_get_psk(s_wsec);
    }

    if (key_mgmt == NULL) {
        type = "nopass";
        nmc_print("%s: %s\n", _("Security"), _("None"));
    } else if (strcmp(key_mgmt, "none") == 0 || strcmp(key_mgmt, "ieee8021x") == 0) {
        type = "WEP";
        nmc_print("%s: WEP\n", _("Security"));
    } else if (strcmp(key_mgmt, "wpa-none") == 0 || strcmp(key_mgmt, "wpa-psk") == 0
               || strcmp(key_mgmt, "sae") == 0) {
        type = "WPA";
        nmc_print("%s: WPA\n", _("Security"));
    } else if (strcmp(key_mgmt, "owe") == 0) {
        type = "nopass";
        nmc_print("%s: OWE\n", _("Security"));
    }

    if (psk)
        nmc_print("%s: %s\n", _("Password"), psk);

    string_append_mecard(string, "T:", type);
    string_append_mecard(string, "S:", ssid);
    string_append_mecard(string, "P:", psk);

    if (nm_setting_wireless_get_hidden(s_wireless))
        g_string_append(string, "H:true;");

    g_string_append_c(string, ';');
    if (nmc_config->use_colors)
        nmc_print_qrcode(string->str);

    nmc_print("\n");
}

static gboolean
wifi_show_device(const NmcConfig *nmc_config, NMDevice *device, GError **error)
{
    NMActiveConnection           *active_conn;
    gs_unref_object NMConnection *connection = NULL;
    gs_unref_variant GVariant    *secrets    = NULL;

    if (!NM_IS_DEVICE_WIFI(device)) {
        g_set_error(error,
                    NMCLI_ERROR,
                    0,
                    _("Error: Device '%s' is not a Wi-Fi device."),
                    nm_device_get_iface(device));
        return FALSE;
    }

    connection = nm_device_get_applied_connection(device, 0, NULL, NULL, error);
    if (!connection)
        return FALSE;

    active_conn = nm_device_get_active_connection(device);
    if (!active_conn) {
        g_set_error(error,
                    NMCLI_ERROR,
                    0,
                    _("no active connection on device '%s'"),
                    nm_device_get_iface(device));
        return FALSE;
    }

    secrets = nm_remote_connection_get_secrets(nm_active_connection_get_connection(active_conn),
                                               NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
                                               NULL,
                                               NULL);
    if (secrets
        && !nm_connection_update_secrets(connection,
                                         NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
                                         secrets,
                                         error)) {
        return FALSE;
    }

    print_wifi_connection(nmc_config, connection);

    return TRUE;
}

static void
do_device_wifi_show_password(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    const char           *ifname  = NULL;
    gs_free NMDevice    **devices = NULL;
    gs_free_error GError *error   = NULL;
    gboolean              found   = FALSE;
    int                   i;

    devices = nmc_get_devices_sorted(nmc->client);

    next_arg(nmc, &argc, &argv, NULL);
    while (argc > 0) {
        if (argc == 1 && nmc->complete)
            nmc_complete_strings(*argv, "ifname");

        if (strcmp(*argv, "ifname") == 0) {
            if (ifname) {
                g_string_printf(nmc->return_text, _("Error: '%s' cannot repeat."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            ifname = *argv;
            if (argc == 1 && nmc->complete)
                complete_device(devices, ifname, TRUE);
        } else if (!nmc->complete) {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        next_arg(nmc, &argc, &argv, NULL);
    }

    if (nmc->complete)
        return;

    for (i = 0; devices[i]; i++) {
        if (ifname && g_strcmp0(nm_device_get_iface(devices[i]), ifname) != 0)
            continue;

        if (wifi_show_device(&nmc->nmc_config, devices[i], &error)) {
            found = TRUE;
        } else {
            if (ifname) {
                g_string_assign(nmc->return_text, error->message);
                nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
                return;
            }
            g_clear_error(&error);
        }

        if (ifname)
            break;
    }

    if (!found) {
        g_string_printf(nmc->return_text, _("Error: No Wi-Fi device found."));
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        return;
    }
}

static NMCCommand device_wifi_cmds[] = {
    {"list", do_device_wifi_list, NULL, TRUE, TRUE},
    {"connect", do_device_wifi_connect, NULL, TRUE, TRUE},
    {"hotspot", do_device_wifi_hotspot, NULL, TRUE, TRUE},
    {"rescan", do_device_wifi_rescan, NULL, TRUE, TRUE},
    {"show-password", do_device_wifi_show_password, NULL, TRUE, TRUE},
    {NULL, do_device_wifi_list, NULL, TRUE, TRUE},
};

static void
do_device_wifi(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    next_arg(nmc, &argc, &argv, NULL);
    nmc_do_cmd(nmc, device_wifi_cmds, *argv, argc, argv);
}

static int
show_device_lldp_list(NMDevice *device, NmCli *nmc, const char *fields_str, int *counter)
{
    const NMMetaAbstractInfo *const *tmpl;
    NmcOutputField                  *arr;
    GPtrArray                       *neighbors;
    const char                      *str;
    int                              i;
    NMC_OUTPUT_DATA_DEFINE_SCOPED(out);
    gs_free char *header_name = NULL;

    neighbors = nm_device_get_lldp_neighbors(device);

    if (!neighbors || !neighbors->len)
        return 0;

    tmpl = (const NMMetaAbstractInfo *const *) nmc_fields_dev_lldp_list;

    /* Main header name */
    header_name = construct_header_name(_("Device LLDP neighbors"), nm_device_get_iface(device));
    out_indices = parse_output_fields(fields_str,
                                      (const NMMetaAbstractInfo *const *) nmc_fields_dev_lldp_list,
                                      FALSE,
                                      NULL,
                                      NULL);
    arr         = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_MAIN_HEADER_ADD | NMC_OF_FLAG_FIELD_NAMES);
    g_ptr_array_add(out.output_data, arr);

    for (i = 0; i < neighbors->len; i++) {
        NMLldpNeighbor *neighbor = neighbors->pdata[i];
        guint           value;

        arr = nmc_dup_fields_array(tmpl, NMC_OF_FLAG_SECTION_PREFIX);
        set_val_str(arr, 0, g_strdup_printf("NEIGHBOR[%d]", (*counter)++));

        set_val_strc(arr, 1, nm_device_get_iface(device));

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_CHASSIS_ID, &str))
            set_val_strc(arr, 2, str);

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_PORT_ID, &str))
            set_val_strc(arr, 3, str);

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_PORT_DESCRIPTION, &str))
            set_val_strc(arr, 4, str);

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_SYSTEM_NAME, &str))
            set_val_strc(arr, 5, str);

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_SYSTEM_DESCRIPTION, &str))
            set_val_strc(arr, 6, str);

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor,
                                                 NM_LLDP_ATTR_SYSTEM_CAPABILITIES,
                                                 &value)) {
            gs_free char *tmp = NULL;

            set_val_str(
                arr,
                7,
                g_strdup_printf("%u (%s)", value, (tmp = nmc_parse_lldp_capabilities(value))));
        }

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor, NM_LLDP_ATTR_IEEE_802_1_PVID, &value))
            set_val_str(arr, 8, nm_strdup_int(value));

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor, NM_LLDP_ATTR_IEEE_802_1_PPVID, &value))
            set_val_str(arr, 9, nm_strdup_int(value));

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor,
                                                 NM_LLDP_ATTR_IEEE_802_1_PPVID_FLAGS,
                                                 &value))
            set_val_str(arr, 10, nm_strdup_int(value));

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor, NM_LLDP_ATTR_IEEE_802_1_VID, &value))
            set_val_str(arr, 11, nm_strdup_int(value));

        if (nm_lldp_neighbor_get_attr_string_value(neighbor,
                                                   NM_LLDP_ATTR_IEEE_802_1_VLAN_NAME,
                                                   &str))
            set_val_strc(arr, 12, str);

        if (nm_lldp_neighbor_get_attr_string_value(neighbor, NM_LLDP_ATTR_DESTINATION, &str))
            set_val_strc(arr, 13, str);

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor, NM_LLDP_ATTR_CHASSIS_ID_TYPE, &value))
            set_val_str(arr, 14, nm_strdup_int(value));

        if (nm_lldp_neighbor_get_attr_uint_value(neighbor, NM_LLDP_ATTR_PORT_ID_TYPE, &value))
            set_val_str(arr, 15, nm_strdup_int(value));

        g_ptr_array_add(out.output_data, arr);
    }

    print_data_prepare_width(out.output_data);
    print_data(&nmc->nmc_config, &nmc->pager_data, out_indices, header_name, 0, &out);

    return neighbors->len;
}

static void
do_device_lldp_list(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMDevice              *device      = NULL;
    gs_free_error GError  *error       = NULL;
    const char            *fields_str  = NULL;
    int                    counter     = 0;
    gs_unref_array GArray *out_indices = NULL;

    next_arg(nmc, &argc, &argv, NULL);
    while (argc > 0) {
        if (argc == 1 && nmc->complete)
            nmc_complete_strings(*argv, "ifname");

        if (strcmp(*argv, "ifname") == 0) {
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }

            device = get_device(nmc, &argc, &argv, &error);
            if (!device) {
                g_string_printf(nmc->return_text, _("Error: %s."), error->message);
                nmc->return_value = error->code;
                return;
            }
        } else {
            g_string_printf(nmc->return_text, _("Error: invalid extra argument '%s'."), *argv);
            nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
            return;
        }

        next_arg(nmc, &argc, &argv, NULL);
    }

    if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "common") == 0)
        fields_str = NMC_FIELDS_DEV_LLDP_LIST_COMMON;
    else if (!nmc->required_fields || g_ascii_strcasecmp(nmc->required_fields, "all") == 0) {
        /* pass */
    } else
        fields_str = nmc->required_fields;

    out_indices = parse_output_fields(fields_str,
                                      (const NMMetaAbstractInfo *const *) nmc_fields_dev_lldp_list,
                                      FALSE,
                                      NULL,
                                      &error);

    if (error) {
        g_string_printf(nmc->return_text, _("Error: 'device lldp list': %s"), error->message);
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (nmc->complete)
        return;

    if (device) {
        show_device_lldp_list(device, nmc, fields_str, &counter);
    } else {
        gs_free NMDevice **devices = nmc_get_devices_sorted(nmc->client);
        guint              i;

        for (i = 0; devices[i]; i++)
            show_device_lldp_list(devices[i], nmc, fields_str, &counter);
    }
}

static NMCCommand device_lldp_cmds[] = {
    {"list", do_device_lldp_list, NULL, TRUE, TRUE},
    {NULL, do_device_lldp_list, NULL, TRUE, TRUE},
};

static void
do_device_lldp(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    if (!nmc->mode_specified)
        nmc->nmc_config_mutable.multiline_output =
            TRUE; /* multiline mode is default for 'device lldp' */

    next_arg(nmc, &argc, &argv, NULL);
    nmc_do_cmd(nmc, device_lldp_cmds, *argv, argc, argv);
}

/*****************************************************************************/

typedef struct {
    NmCli        *nmc;
    NMCheckpoint *checkpoint;
    char        **argv;
    guint         removed_id;
    guint         child_id;
    gboolean      removed;
} CheckpointCbInfo;

static void
free_checkpoint_info(CheckpointCbInfo *info)
{
    g_clear_object(&info->checkpoint);
    g_strfreev(info->argv);
    g_slice_free(CheckpointCbInfo, info);
}

static void
checkpoints_changed_cb(GObject *object, GParamSpec *pspec, CheckpointCbInfo *info)
{
    const GPtrArray *checkpoints;
    guint            i;

    checkpoints = nm_client_get_checkpoints(info->nmc->client);
    for (i = 0; i < checkpoints->len; i++) {
        if (checkpoints->pdata[i] == info->checkpoint) {
            /* Our checkpoint still exists. */
            return;
        }
    }

    g_string_printf(info->nmc->return_text, _("Checkpoint was removed."));
    info->nmc->return_value = NMC_RESULT_ERROR_TIMEOUT_EXPIRED;

    info->removed = TRUE;

    if (!info->child_id) {
        /* The command is done, we're in the confirmation prompt. */
        nmc_print("%s\n", _("No"));
        g_main_loop_quit(loop);
    }
}

static void
checkpoint_destroy_cb(GObject *object, GAsyncResult *result, void *user_data)
{
    NmCli                *nmc   = (NmCli *) user_data;
    gs_free_error GError *error = NULL;

    if (!nm_client_checkpoint_destroy_finish(nmc->client, result, &error)) {
        g_string_printf(nmc->return_text,
                        _("Error: Destroying a checkpoint failed: %s"),
                        error->message);
        nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
    }

    g_main_loop_quit(loop);
}

static void
child_watch_cb(GPid pid, gint wait_status, gpointer user_data)
{
    CheckpointCbInfo *info = (CheckpointCbInfo *) user_data;
    NmCli            *nmc  = info->nmc;
    char             *line;

    info->child_id = 0;
    if (info->removed) {
        g_main_loop_quit(loop);
        goto out;
    }

    while (g_main_loop_is_running(loop)) {
        line = nmc_readline(&nmc->nmc_config, "Type \"%s\" to commit the changes: ", _("Yes"));
        if (g_strcmp0(line, _("Yes")) == 0) {
            g_signal_handler_disconnect(nmc->client, info->removed_id);
            nm_client_checkpoint_destroy(nmc->client,
                                         nm_object_get_path(NM_OBJECT(info->checkpoint)),
                                         NULL,
                                         checkpoint_destroy_cb,
                                         nmc);
            break;
        }
    }
    nmc_cleanup_readline();
out:
    free_checkpoint_info(info);
}

static void
checkpoint_create_cb(GObject *object, GAsyncResult *result, void *user_data)
{
    NMClient             *client = NM_CLIENT(object);
    CheckpointCbInfo     *info   = (CheckpointCbInfo *) user_data;
    gs_free_error GError *error  = NULL;
    GPid                  pid;

    info->checkpoint = nm_client_checkpoint_create_finish(client, result, &error);
    if (!info->checkpoint) {
        g_string_printf(info->nmc->return_text,
                        _("Error: Creating a checkpoint failed: %s"),
                        error->message);
        info->nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        g_main_loop_quit(loop);
        goto err;
    }

    if (!g_spawn_async(NULL,
                       info->argv,
                       NULL,
                       G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_SEARCH_PATH
                           | G_SPAWN_CHILD_INHERITS_STDIN | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL,
                       info,
                       &pid,
                       &error)) {
        g_string_printf(info->nmc->return_text, _("Error: %s"), error->message);
        info->nmc->return_value = NMC_RESULT_ERROR_UNKNOWN;
        g_main_loop_quit(loop);
        goto err;
    }

    info->child_id   = g_child_watch_add(pid, child_watch_cb, info);
    info->removed_id = g_signal_connect(client,
                                        "notify::" NM_CLIENT_CHECKPOINTS,
                                        G_CALLBACK(checkpoints_changed_cb),
                                        info);

    return;

err:
    free_checkpoint_info(info);
}

static void
do_device_checkpoint(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    NMClient                    *client  = nmc->client;
    long unsigned int            timeout = 15;
    int                          option;
    CheckpointCbInfo            *info;
    const GPtrArray             *devices      = NULL;
    gs_unref_ptrarray GPtrArray *devices_free = NULL;

    while ((option = next_arg(nmc, &argc, &argv, "--timeout", NULL)) > 0) {
        switch (option) {
        case 1: /* --timeout */
            argc--;
            argv++;
            if (!argc) {
                g_string_printf(nmc->return_text, _("Error: %s argument is missing."), *(argv - 1));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            if (!nmc_string_to_uint(*argv, TRUE, 0, G_MAXUINT32, &timeout)) {
                g_string_printf(nmc->return_text, _("Error: '%s' is not a valid timeout."), *argv);
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
            break;
        default:
            nm_assert_not_reached();
            break;
        }
    }

    if (argc) {
        if (strcmp(*argv, "--") == 0) {
            devices = nm_client_get_devices(client);
            argc--;
            argv++;
        } else {
            devices = devices_free = get_device_list(nmc, &argc, &argv);
            if (!devices) {
                g_string_printf(nmc->return_text, _("Error: not all devices found."));
                nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
                return;
            }
        }
    }

    if (argc == 0) {
        g_string_printf(nmc->return_text, _("Error: Expected a command to run after '--'"));
        nmc->return_value = NMC_RESULT_ERROR_USER_INPUT;
        return;
    }

    if (nmc->complete)
        return;

    info       = g_slice_new0(CheckpointCbInfo);
    info->nmc  = nmc;
    info->argv = nm_strv_dup(argv, argc, TRUE);

    nmc->should_wait++;
    nm_client_checkpoint_create(client,
                                devices,
                                (guint32) timeout,
                                NM_CHECKPOINT_CREATE_FLAG_NONE,
                                NULL,
                                checkpoint_create_cb,
                                info);
}

/*****************************************************************************/

static gboolean
is_single_word(const char *line)
{
    size_t n1, n2, n3;

    n1 = strspn(line, " \t");
    n2 = strcspn(line + n1, " \t\0") + n1;
    n3 = strspn(line + n2, " \t");

    if (n3 == 0)
        return TRUE;
    else
        return FALSE;
}

static char **
nmcli_device_tab_completion(const char *text, int start, int end)
{
    char               **match_array    = NULL;
    rl_compentry_func_t *generator_func = NULL;

    /* Disable readline's default filename completion */
    rl_attempted_completion_over = 1;

    if (g_strcmp0(rl_prompt, PROMPT_INTERFACE) == 0) {
        /* Disable appending space after completion */
        rl_completion_append_character = '\0';

        if (!is_single_word(rl_line_buffer))
            return NULL;

        generator_func = nmc_rl_gen_func_ifnames;
    } else if (g_strcmp0(rl_prompt, PROMPT_INTERFACES) == 0) {
        generator_func = nmc_rl_gen_func_ifnames;
    }

    if (generator_func)
        match_array = rl_completion_matches(text, generator_func);

    return match_array;
}

void
nmc_command_func_device(const NMCCommand *cmd, NmCli *nmc, int argc, const char *const *argv)
{
    static const NMCCommand cmds[] = {
        {"checkpoint", do_device_checkpoint, usage_device_checkpoint, TRUE, TRUE},
        {"connect", do_device_connect, usage_device_connect, TRUE, TRUE},
        {"disconnect", do_devices_disconnect, usage_device_disconnect, TRUE, TRUE},
        {"delete", do_devices_delete, usage_device_delete, TRUE, TRUE},
        {"down", do_devices_disconnect, usage_device_disconnect, TRUE, TRUE},
        {"lldp", do_device_lldp, usage_device_lldp, FALSE, FALSE},
        {"monitor", do_devices_monitor, usage_device_monitor, TRUE, TRUE},
        {"modify", do_device_modify, usage_device_modify, TRUE, TRUE},
        {"reapply", do_device_reapply, usage_device_reapply, TRUE, TRUE},
        {"status", do_devices_status, usage_device_status, TRUE, TRUE},
        {"set", do_device_set, usage_device_set, TRUE, TRUE},
        {"show", do_device_show, usage_device_show, TRUE, TRUE},
        {"up", do_device_connect, usage_device_connect, TRUE, TRUE},
        {"wifi", do_device_wifi, usage_device_wifi, FALSE, FALSE},
        {NULL, do_devices_status, usage, TRUE, TRUE},
    };

    next_arg(nmc, &argc, &argv, NULL);

    nmc_start_polkit_agent_start_try(nmc);

    rl_attempted_completion_function = (rl_completion_func_t *) nmcli_device_tab_completion;

    nmc_do_cmd(nmc, cmds, *argv, argc, argv);
}

void
nmc_monitor_devices(NmCli *nmc)
{
    do_devices_monitor(NULL, nmc, 0, NULL);
}
