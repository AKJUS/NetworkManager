/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2012 - 2018 Red Hat, Inc.
 */

#include "libnm-client-aux-extern/nm-default-client.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#if HAVE_EDITLINE_READLINE
#include <editline/readline.h>
#else
#include <readline/history.h>
#include <readline/readline.h>
#endif
#include <gio/gunixinputstream.h>

#include "libnm-client-aux-extern/nm-libnm-aux.h"

#include "libnmc-base/nm-vpn-helpers.h"
#include "libnmc-base/nm-client-utils.h"
#include "libnm-glib-aux/nm-secret-utils.h"

#include "utils.h"

/*****************************************************************************/

static char **
_ip_config_get_routes(NMIPConfig *cfg)
{
    gs_unref_hashtable GHashTable *hash = NULL;
    GPtrArray                     *ptr_array;
    char                         **arr;
    guint                          i;

    ptr_array = nm_ip_config_get_routes(cfg);
    if (!ptr_array)
        return NULL;

    if (ptr_array->len == 0)
        return NULL;

    arr = g_new(char *, ptr_array->len + 1);
    for (i = 0; i < ptr_array->len; i++) {
        NMIPRoute         *route = g_ptr_array_index(ptr_array, i);
        gs_strfreev char **names = NULL;
        gsize              j;
        GString           *str;
        guint64            metric;
        gs_free char      *attributes = NULL;

        str = g_string_new(NULL);
        g_string_append_printf(
            str,
            "dst = %s/%u, nh = %s",
            nm_ip_route_get_dest(route),
            nm_ip_route_get_prefix(route),
            nm_ip_route_get_next_hop(route)
                ?: (nm_ip_route_get_family(route) == AF_INET ? "0.0.0.0" : "::"));

        metric = nm_ip_route_get_metric(route);
        if (metric != -1) {
            g_string_append_printf(str, ", mt = %u", (guint) metric);
        }

        names = nm_ip_route_get_attribute_names(route);
        if (names[0]) {
            if (!hash)
                hash = g_hash_table_new(nm_str_hash, g_str_equal);
            else
                g_hash_table_remove_all(hash);

            for (j = 0; names[j]; j++)
                g_hash_table_insert(hash, names[j], nm_ip_route_get_attribute(route, names[j]));

            attributes = nm_utils_format_variant_attributes(hash, ',', '=');
            if (attributes) {
                g_string_append(str, ", ");
                g_string_append(str, attributes);
            }
        }

        arr[i] = g_string_free(str, FALSE);
    }

    nm_assert(i == ptr_array->len);
    arr[i] = NULL;

    return arr;
}

/*****************************************************************************/

static gconstpointer
_metagen_ip4_config_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMIPConfig        *cfg4 = target;
    GPtrArray         *ptr_array;
    char             **arr;
    const char *const *arrc;
    guint              i = 0;
    const char        *str;

    nm_assert(info->info_type < _NMC_GENERIC_INFO_TYPE_IP4_CONFIG_NUM);

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);
    NM_SET_OUT(out_is_default, TRUE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_ADDRESS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        ptr_array = nm_ip_config_get_addresses(cfg4);
        if (ptr_array) {
            arr = g_new(char *, ptr_array->len + 1);
            for (i = 0; i < ptr_array->len; i++) {
                NMIPAddress *addr = g_ptr_array_index(ptr_array, i);

                arr[i] = g_strdup_printf("%s/%u",
                                         nm_ip_address_get_address(addr),
                                         nm_ip_address_get_prefix(addr));
            }
            arr[i] = NULL;
        } else
            arr = NULL;
        goto arr_out;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_GATEWAY:
        str = nm_ip_config_get_gateway(cfg4);
        NM_SET_OUT(out_is_default, !str);
        return str;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_ROUTE:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arr = _ip_config_get_routes(cfg4);
        goto arr_out;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_DNS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_nameservers(cfg4);
        goto arrc_out;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_DOMAIN:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_domains(cfg4);
        goto arrc_out;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_SEARCHES:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_searches(cfg4);
        goto arrc_out;
    case NMC_GENERIC_INFO_TYPE_IP4_CONFIG_WINS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_wins_servers(cfg4);
        goto arrc_out;
    default:
        break;
    }

    g_return_val_if_reached(NULL);

arrc_out:
    NM_SET_OUT(out_is_default, !arrc || !arrc[0]);
    *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
    return arrc;

arr_out:
    NM_SET_OUT(out_is_default, !arr || !arr[0]);
    *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
    *out_to_free = arr;
    return arr;
}

const NmcMetaGenericInfo *const metagen_ip4_config[_NMC_GENERIC_INFO_TYPE_IP4_CONFIG_NUM + 1] = {
#define _METAGEN_IP4_CONFIG(type, name) \
    [type] = NMC_META_GENERIC(name, .info_type = type, .get_fcn = _metagen_ip4_config_get_fcn)
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_ADDRESS, "ADDRESS"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_GATEWAY, "GATEWAY"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_ROUTE, "ROUTE"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_DNS, "DNS"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_DOMAIN, "DOMAIN"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_SEARCHES, "SEARCHES"),
    _METAGEN_IP4_CONFIG(NMC_GENERIC_INFO_TYPE_IP4_CONFIG_WINS, "WINS"),
};

/*****************************************************************************/

static gconstpointer
_metagen_ip6_config_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMIPConfig        *cfg6 = target;
    GPtrArray         *ptr_array;
    char             **arr;
    const char *const *arrc;
    guint              i = 0;
    const char        *str;

    nm_assert(info->info_type < _NMC_GENERIC_INFO_TYPE_IP6_CONFIG_NUM);

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);
    NM_SET_OUT(out_is_default, TRUE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_ADDRESS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        ptr_array = nm_ip_config_get_addresses(cfg6);
        if (ptr_array) {
            arr = g_new(char *, ptr_array->len + 1);
            for (i = 0; i < ptr_array->len; i++) {
                NMIPAddress *addr = g_ptr_array_index(ptr_array, i);

                arr[i] = g_strdup_printf("%s/%u",
                                         nm_ip_address_get_address(addr),
                                         nm_ip_address_get_prefix(addr));
            }
            arr[i] = NULL;
        } else
            arr = NULL;
        goto arr_out;
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_GATEWAY:
        str = nm_ip_config_get_gateway(cfg6);
        NM_SET_OUT(out_is_default, !str);
        return str;
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_ROUTE:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arr = _ip_config_get_routes(cfg6);
        goto arr_out;
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_DNS:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_nameservers(cfg6);
        goto arrc_out;
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_DOMAIN:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_domains(cfg6);
        goto arrc_out;
    case NMC_GENERIC_INFO_TYPE_IP6_CONFIG_SEARCHES:
        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;
        arrc = nm_ip_config_get_searches(cfg6);
        goto arrc_out;
    default:
        break;
    }

    g_return_val_if_reached(NULL);

arrc_out:
    NM_SET_OUT(out_is_default, !arrc || !arrc[0]);
    *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
    return arrc;

arr_out:
    NM_SET_OUT(out_is_default, !arr || !arr[0]);
    *out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_STRV;
    *out_to_free = arr;
    return arr;
}

const NmcMetaGenericInfo *const metagen_ip6_config[_NMC_GENERIC_INFO_TYPE_IP6_CONFIG_NUM + 1] = {
#define _METAGEN_IP6_CONFIG(type, name) \
    [type] = NMC_META_GENERIC(name, .info_type = type, .get_fcn = _metagen_ip6_config_get_fcn)
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_ADDRESS, "ADDRESS"),
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_GATEWAY, "GATEWAY"),
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_ROUTE, "ROUTE"),
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_DNS, "DNS"),
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_DOMAIN, "DOMAIN"),
    _METAGEN_IP6_CONFIG(NMC_GENERIC_INFO_TYPE_IP6_CONFIG_SEARCHES, "SEARCHES"),
};

/*****************************************************************************/

static gconstpointer
_metagen_dhcp_config_get_fcn(NMC_META_GENERIC_INFO_GET_FCN_ARGS)
{
    NMDhcpConfig *dhcp = target;
    guint         i;
    char        **arr = NULL;

    NMC_HANDLE_COLOR(NM_META_COLOR_NONE);

    switch (info->info_type) {
    case NMC_GENERIC_INFO_TYPE_DHCP_CONFIG_OPTION:
    {
        GHashTable    *table;
        gs_free char **arr2 = NULL;
        guint          n;

        if (!NM_FLAGS_HAS(get_flags, NM_META_ACCESSOR_GET_FLAGS_ACCEPT_STRV))
            return NULL;

        table = nm_dhcp_config_get_options(dhcp);
        if (!table)
            goto arr_out;

        arr2 = (char **) nm_strdict_get_keys(table, TRUE, &n);
        if (!n)
            goto arr_out;

        nm_assert(arr2 && !arr2[n] && n == NM_PTRARRAY_LEN(arr2));
        for (i = 0; i < n; i++) {
            const char *k = arr2[i];
            const char *v;

            nm_assert(k);
            v       = g_hash_table_lookup(table, k);
            arr2[i] = g_strdup_printf("%s = %s", k, v);
        }

        arr = g_steal_pointer(&arr2);
        goto arr_out;
    }
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

const NmcMetaGenericInfo *const metagen_dhcp_config[_NMC_GENERIC_INFO_TYPE_DHCP_CONFIG_NUM + 1] = {
#define _METAGEN_DHCP_CONFIG(type, name) \
    [type] = NMC_META_GENERIC(name, .info_type = type, .get_fcn = _metagen_dhcp_config_get_fcn)
    _METAGEN_DHCP_CONFIG(NMC_GENERIC_INFO_TYPE_DHCP_CONFIG_OPTION, "OPTION"),
};

/*****************************************************************************/

gboolean
print_ip_config(NMIPConfig      *cfg,
                int              addr_family,
                const NmcConfig *nmc_config,
                const char      *one_field)
{
    gs_free_error GError *error     = NULL;
    gs_free char         *field_str = NULL;

    if (!cfg)
        return FALSE;

    if (one_field) {
        field_str =
            g_strdup_printf("IP%c.%s", nm_utils_addr_family_to_char(addr_family), one_field);
    }

    if (!nmc_print_table(nmc_config,
                         (gpointer[]) {cfg, NULL},
                         NULL,
                         NULL,
                         addr_family == AF_INET
                             ? NMC_META_GENERIC_GROUP("IP4", metagen_ip4_config, N_("GROUP"))
                             : NMC_META_GENERIC_GROUP("IP6", metagen_ip6_config, N_("GROUP")),
                         field_str,
                         &error)) {
        return FALSE;
    }
    return TRUE;
}

gboolean
print_dhcp_config(NMDhcpConfig    *dhcp,
                  int              addr_family,
                  const NmcConfig *nmc_config,
                  const char      *one_field)
{
    gs_free_error GError *error     = NULL;
    gs_free char         *field_str = NULL;

    if (!dhcp)
        return FALSE;

    if (one_field) {
        field_str =
            g_strdup_printf("DHCP%c.%s", nm_utils_addr_family_to_char(addr_family), one_field);
    }

    if (!nmc_print_table(nmc_config,
                         (gpointer[]) {dhcp, NULL},
                         NULL,
                         NULL,
                         addr_family == AF_INET
                             ? NMC_META_GENERIC_GROUP("DHCP4", metagen_dhcp_config, N_("GROUP"))
                             : NMC_META_GENERIC_GROUP("DHCP6", metagen_dhcp_config, N_("GROUP")),
                         field_str,
                         &error)) {
        return FALSE;
    }
    return TRUE;
}

/*
 * nmc_find_connection:
 * @connections: array of NMConnections to search in
 * @filter_type: "id", "uuid", "path", "filename", or %NULL
 * @filter_val: connection to find (connection name, UUID or path)
 * @out_result: if not NULL, attach all matching connection to this
 *   list. If necessary, a new array will be allocated. If the array
 *   already contains a connection, it will not be added a second time.
 *   All object are referenced by the array. If the function allocates
 *   a new array, it will set the free function to g_object_unref.
 * @complete: print possible completions
 *
 * Find a connection in @list according to @filter_val. @filter_type determines
 * what property is used for comparison. When @filter_type is NULL, compare
 * @filter_val against all types. Otherwise, only compare against the specified
 * type. If 'path' filter type is specified, comparison against numeric index
 * (in addition to the whole path) is allowed.
 *
 * Returns: found connection, or %NULL
 */
NMConnection *
nmc_find_connection(const GPtrArray *connections,
                    const char      *filter_type,
                    const char      *filter_val,
                    GPtrArray      **out_result,
                    gboolean         complete)
{
    NMConnection                *best_candidate_uuid = NULL;
    NMConnection                *best_candidate      = NULL;
    gs_unref_ptrarray GPtrArray *result_allocated    = NULL;
    GPtrArray                   *result              = out_result ? *out_result : NULL;
    const guint                  result_inital_len   = result ? result->len : 0u;
    guint                        i, j;
    gboolean                     must_match_uniquely;

    nm_assert(connections);
    nm_assert(filter_val);

    must_match_uniquely = NM_IN_STRSET(filter_type, "uuid", "path");

    for (i = 0; i < connections->len; i++) {
        gboolean      match_by_uuid = FALSE;
        NMConnection *connection;
        const char   *v;
        const char   *v_num;

        connection = NM_CONNECTION(connections->pdata[i]);

        if (NM_IN_STRSET(filter_type, NULL, "uuid")) {
            v = nm_connection_get_uuid(connection);
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v)) {
                match_by_uuid = TRUE;
                goto found;
            }
            if (filter_type && !nm_str_is_empty(filter_val) && g_str_has_prefix(v, filter_val)) {
                /* If the selector is qualified by "uuid", prefix matches for the UUID are
                 * also OK. At least, if they result in a unique match. */
                nm_assert(must_match_uniquely);
                goto found;
            }
        }

        if (NM_IN_STRSET(filter_type, NULL, "id")) {
            v = nm_connection_get_id(connection);
            if (complete)
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v))
                goto found;
        }

        if (NM_IN_STRSET(filter_type, NULL, "path")) {
            v     = nm_connection_get_path(connection);
            v_num = nm_utils_dbus_path_get_last_component(v);
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v, (*filter_val ? v_num : NULL));
            if (nm_streq0(filter_val, v) || (filter_type && nm_streq0(filter_val, v_num)))
                goto found;
        }

        if (NM_IS_REMOTE_CONNECTION(connections->pdata[i])
            && NM_IN_STRSET(filter_type, NULL, "filename")) {
            v = nm_remote_connection_get_filename(NM_REMOTE_CONNECTION(connections->pdata[i]));
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v))
                goto found;
        }

        continue;

found:

        if (must_match_uniquely && (best_candidate || best_candidate_uuid)) {
            /* We found duplicates. This is wrong. */
            if (out_result && *out_result) {
                /* Remove the element that we added before. */
                g_ptr_array_set_size(*out_result, result_inital_len);
            }
            return NULL;
        }

        if (match_by_uuid) {
            if (!complete && !out_result)
                return connection;
            if (!best_candidate_uuid)
                best_candidate_uuid = connection;
        } else {
            if (!best_candidate)
                best_candidate = connection;
        }

        if (out_result) {
            gboolean already_tracked = FALSE;

            if (!result) {
                result_allocated = g_ptr_array_new_with_free_func(g_object_unref);
                result           = result_allocated;
            } else {
                for (j = 0; j < result->len; j++) {
                    if (connection == result->pdata[j]) {
                        already_tracked = TRUE;
                        break;
                    }
                }
            }
            if (!already_tracked) {
                if (match_by_uuid) {
                    /* the profile is matched exactly (by UUID). We prepend it
                     * to the list of all found profiles. */
                    g_ptr_array_insert(result, result_inital_len, g_object_ref(connection));
                } else
                    g_ptr_array_add(result, g_object_ref(connection));
            }
        }
    }

    if (result_allocated)
        *out_result = g_steal_pointer(&result_allocated);
    return best_candidate_uuid ?: best_candidate;
}

NMActiveConnection *
nmc_find_active_connection(const GPtrArray *active_cons,
                           const char      *filter_type,
                           const char      *filter_val,
                           GPtrArray      **out_result,
                           gboolean         complete)
{
    guint               i, j;
    NMActiveConnection *best_candidate = NULL;
    GPtrArray          *result         = out_result ? *out_result : NULL;

    nm_assert(filter_val);

    for (i = 0; i < active_cons->len; i++) {
        NMRemoteConnection *con;
        NMActiveConnection *candidate = g_ptr_array_index(active_cons, i);
        const char         *v, *v_num;

        /* When filter_type is NULL, compare connection ID (filter_val)
         * against all types. Otherwise, only compare against the specific
         * type. If 'path' or 'apath' filter types are specified, comparison
         * against numeric index (in addition to the whole path) is allowed.
         */
        if (NM_IN_STRSET(filter_type, NULL, "id")) {
            v = nm_active_connection_get_id(candidate);
            if (complete)
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v))
                goto found;
        }

        if (NM_IN_STRSET(filter_type, NULL, "uuid")) {
            v = nm_active_connection_get_uuid(candidate);
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v))
                goto found;
        }

        con = nm_active_connection_get_connection(candidate);

        if (NM_IN_STRSET(filter_type, NULL, "path")) {
            v     = con ? nm_connection_get_path(NM_CONNECTION(con)) : NULL;
            v_num = nm_utils_dbus_path_get_last_component(v);
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v, filter_type ? v_num : NULL);
            if (nm_streq0(filter_val, v) || (filter_type && nm_streq0(filter_val, v_num)))
                goto found;
        }

        if (NM_IN_STRSET(filter_type, NULL, "filename")) {
            v = con ? nm_remote_connection_get_filename(con) : NULL;
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v);
            if (nm_streq0(filter_val, v))
                goto found;
        }

        if (NM_IN_STRSET(filter_type, NULL, "apath")) {
            v     = nm_object_get_path(NM_OBJECT(candidate));
            v_num = nm_utils_dbus_path_get_last_component(v);
            if (complete && (filter_type || *filter_val))
                nmc_complete_strings(filter_val, v, filter_type ? v_num : NULL);
            if (nm_streq0(filter_val, v) || (filter_type && nm_streq0(filter_val, v_num)))
                goto found;
        }

        continue;

found:
        if (!out_result)
            return candidate;
        if (!best_candidate)
            best_candidate = candidate;
        if (!result)
            result = g_ptr_array_new_with_free_func(g_object_unref);
        for (j = 0; j < result->len; j++) {
            if (candidate == result->pdata[j])
                break;
        }
        if (j == result->len)
            g_ptr_array_add(result, g_object_ref(candidate));
    }

    NM_SET_OUT(out_result, result);
    return best_candidate;
}

static gboolean
vpn_openconnect_get_secrets(NMConnection *connection, GPtrArray *secrets)
{
    GError       *error = NULL;
    NMSettingVpn *s_vpn;
    gboolean      ret;

    if (!connection)
        return FALSE;

    if (!nm_connection_is_type(connection, NM_SETTING_VPN_SETTING_NAME))
        return FALSE;

    s_vpn = nm_connection_get_setting_vpn(connection);
    if (!nm_streq0(nm_setting_vpn_get_service_type(s_vpn), NM_SECRET_AGENT_VPN_TYPE_OPENCONNECT))
        return FALSE;

    /* Interactively authenticate to OpenConnect server and get secrets */
    ret = nm_vpn_openconnect_authenticate_helper(s_vpn, secrets, &error);

    if (!ret) {
        nmc_printerr(_("Error: openconnect failed: %s\n"), error->message);
        g_clear_error(&error);
        return FALSE;
    }

    return TRUE;
}

static gboolean
get_secrets_from_user(const NmcConfig *nmc_config,
                      const char      *request_id,
                      const char      *title,
                      const char      *msg,
                      NMConnection    *connection,
                      gboolean         ask,
                      GHashTable      *pwds_hash,
                      GPtrArray       *secrets)
{
    int i;

    /* Check if there is a VPN OpenConnect secret to ask for */
    if (ask)
        vpn_openconnect_get_secrets(connection, secrets);

    for (i = 0; i < secrets->len; i++) {
        NMSecretAgentSimpleSecret *secret = secrets->pdata[i];
        char                      *pwd    = NULL;

        /* First try to find the password in provided passwords file,
         * then ask user. */
        if (pwds_hash && (pwd = g_hash_table_lookup(pwds_hash, secret->entry_id))) {
            pwd = g_strdup(pwd);
        } else {
            if (ask) {
                gboolean echo_on;

                if (secret->value) {
                    if (!g_strcmp0(secret->vpn_type, NM_DBUS_INTERFACE ".openconnect")) {
                        /* Do not present and ask user for openconnect secrets, we already have them */
                        continue;
                    } else {
                        /* Prefill the password if we have it. */
                        rl_startup_hook = nmc_rl_set_deftext;
                        nm_strdup_reset(&nmc_rl_pre_input_deftext, secret->value);
                    }
                }
                if (msg)
                    nmc_print("%s\n", msg);

                echo_on = secret->is_secret ? secret->force_echo || nmc_config->show_secrets : TRUE;

                if (secret->no_prompt_entry_id)
                    pwd = nmc_readline_echo(nmc_config, echo_on, "%s: ", secret->pretty_name);
                else
                    pwd = nmc_readline_echo(nmc_config,
                                            echo_on,
                                            "%s (%s): ",
                                            secret->pretty_name,
                                            secret->entry_id);

                if (!pwd)
                    pwd = g_strdup("");
            } else {
                if (msg)
                    nmc_print("%s\n", msg);
                nmc_printerr(_("Warning: password for '%s' not given in 'passwd-file' "
                               "and nmcli cannot ask without '--ask' option.\n"),
                             secret->entry_id);
            }
        }
        /* No password provided, cancel the secrets. */
        if (!pwd)
            return FALSE;
        nm_free_secret(secret->value);
        secret->value = pwd;
    }
    return TRUE;
}

/**
 * nmc_secrets_requested:
 * @agent: the #NMSecretAgentSimple
 * @request_id: request ID, to eventually pass to
 *   nm_secret_agent_simple_response()
 * @title: a title for the password request
 * @msg: a prompt message for the password request
 * @secrets: (element-type #NMSecretAgentSimpleSecret): array of secrets
 *   being requested.
 * @user_data: user data passed to the function
 *
 * This function is used as a callback for "request-secrets" signal of
 * NMSecretAgentSimpleSecret.
*/
void
nmc_secrets_requested(NMSecretAgentSimple *agent,
                      const char          *request_id,
                      const char          *title,
                      const char          *msg,
                      GPtrArray           *secrets,
                      gpointer             user_data)
{
    NmCli           *nmc        = (NmCli *) user_data;
    NMConnection    *connection = NULL;
    char            *path, *p;
    gboolean         success = FALSE;
    const GPtrArray *connections;

    if (nmc->nmc_config.print_output == NMC_PRINT_PRETTY)
        nmc_terminal_erase_line();

    /* Find the connection for the request */
    path = g_strdup(request_id);
    if (path) {
        p = strrchr(path, '/');
        if (p)
            *p = '\0';
        connections = nm_client_get_connections(nmc->client);
        connection  = nmc_find_connection(connections, "path", path, NULL, FALSE);
        g_free(path);
    }

    success = get_secrets_from_user(&nmc->nmc_config,
                                    request_id,
                                    title,
                                    msg,
                                    connection,
                                    nmc->nmc_config.in_editor || nmc->ask,
                                    nmc->pwds_hash,
                                    secrets);
    if (success)
        nm_secret_agent_simple_response(agent, request_id, secrets);
    else {
        /* Unregister our secret agent on failure, so that another agent
         * may be tried */
        if (nmc->secret_agent) {
            nm_secret_agent_old_unregister(NM_SECRET_AGENT_OLD(nmc->secret_agent), NULL, NULL);
            g_clear_object(&nmc->secret_agent);
        }
    }
}

char *
nmc_unique_connection_name(const GPtrArray *connections, const char *try_name)
{
    NMConnection *connection;
    const char   *name;
    char         *new_name;
    unsigned      num = 1;
    int           i   = 0;

    new_name = g_strdup(try_name);
    while (i < connections->len) {
        connection = NM_CONNECTION(connections->pdata[i]);

        name = nm_connection_get_id(connection);
        if (g_strcmp0(new_name, name) == 0) {
            g_free(new_name);
            new_name = g_strdup_printf("%s-%d", try_name, num++);
            i        = 0;
        } else
            i++;
    }
    return new_name;
}

/* readline state variables */
static gboolean nmcli_in_readline = FALSE;
static gboolean rl_got_line;
static char    *rl_string;

/**
 * nmc_cleanup_readline:
 *
 * Cleanup readline when nmcli is terminated.
 * It makes sure the terminal is not garbled.
 */
void
nmc_cleanup_readline(void)
{
    rl_free_line_state();
    rl_cleanup_after_signal();
}

gboolean
nmc_get_in_readline(void)
{
    return nmcli_in_readline;
}

void
nmc_set_in_readline(gboolean in_readline)
{
    nmcli_in_readline = in_readline;
}

static void
readline_cb(char *line)
{
    rl_got_line = TRUE;

    free(rl_string);
    rl_string = line;

    rl_callback_handler_remove();
}

static gboolean
stdin_ready_cb(int fd, GIOCondition condition, gpointer data)
{
    rl_callback_read_char();
    return TRUE;
}

static char *
nmc_readline_helper(const NmcConfig *nmc_config, const char *prompt)
{
    GSource *io_source;
    char    *result;

    nmc_set_in_readline(TRUE);

    io_source = nm_g_unix_fd_add_source(STDIN_FILENO, G_IO_IN, stdin_ready_cb, NULL);

read_again:
    nm_clear_free(&rl_string);

    rl_got_line = FALSE;
    rl_callback_handler_install(prompt, readline_cb);

    while (!rl_got_line && (g_main_loop_is_running(loop) || nmc_config->offline)
           && !nmc_seen_sigint())
        g_main_context_iteration(NULL, TRUE);

    /* If Ctrl-C was detected, complete the line */
    if (nmc_seen_sigint()) {
        rl_echo_signal_char(SIGINT);
        if (!rl_got_line) {
            rl_stuff_char('\n');
            rl_callback_read_char();
        }
    }

    /* Add string to the history */
    if (rl_string && *rl_string)
        add_history(rl_string);

    if (nmc_seen_sigint()) {
        /* Ctrl-C */
        nmc_clear_sigint();
        if (nmc_config->in_editor || (rl_string && *rl_string)) {
            /* In editor, or the line is not empty */
            /* Call readline again to get new prompt (repeat) */
            goto read_again;
        } else {
            /* Not in editor and line is empty, exit */
            nmc_exit();
        }
    } else if (!rl_string) {
        /* Ctrl-D, exit */
        if (g_main_loop_is_running(loop) || nmc_config->offline)
            nmc_exit();
    }

    /* Return NULL, not empty string */
    if (rl_string && *rl_string == '\0')
        nm_clear_free(&rl_string);

    nm_clear_g_source_inst(&io_source);

    nmc_set_in_readline(FALSE);

    if (!rl_string)
        return NULL;

    result = g_strdup(rl_string);
    nm_clear_free(&rl_string);
    return result;
}

/**
 * nmc_readline:
 * @prompt_fmt: prompt to print (telling user what to enter). It is standard
 *   printf() format string
 * @...: a list of arguments according to the @prompt_fmt format string
 *
 * Wrapper around libreadline's readline() function.
 * If user pressed Ctrl-C, readline() is called again (if not in editor and
 * line is empty, nmcli will quit).
 * If user pressed Ctrl-D on empty line, nmcli will quit.
 *
 * Returns: the user provided string. In case the user entered empty string,
 * this function returns NULL.
 */
char *
nmc_readline(const NmcConfig *nmc_config, const char *prompt_fmt, ...)
{
    va_list       args;
    gs_free char *prompt = NULL;

    rl_initialize();

    va_start(args, prompt_fmt);
    prompt = g_strdup_vprintf(prompt_fmt, args);
    va_end(args);
    return nmc_readline_helper(nmc_config, prompt);
}

static void
nmc_secret_redisplay(void)
{
    int         save_point       = rl_point;
    int         save_end         = rl_end;
    char       *save_line_buffer = rl_line_buffer;
    const char *subst            = nmc_password_subst_char();
    int         subst_len        = strlen(subst);
    int         i;

    rl_point       = g_utf8_strlen(save_line_buffer, save_point) * subst_len;
    rl_end         = g_utf8_strlen(rl_line_buffer, -1) * subst_len;
    rl_line_buffer = g_slice_alloc(rl_end + 1);

    for (i = 0; i + subst_len <= rl_end; i += subst_len)
        memcpy(&rl_line_buffer[i], subst, subst_len);
    rl_line_buffer[i] = '\0';

    rl_redisplay();
    g_slice_free1(rl_end + 1, rl_line_buffer);
    rl_line_buffer = save_line_buffer;
    rl_end         = save_end;
    rl_point       = save_point;
}

/**
 * nmc_readline_echo:
 *
 * The same as nmc_readline() except it can disable echoing of input characters if @echo_on is %FALSE.
 * nmc_readline(TRUE, ...) == nmc_readline(...)
 */
char *
nmc_readline_echo(const NmcConfig *nmc_config, gboolean echo_on, const char *prompt_fmt, ...)
{
    va_list       args;
    gs_free char *prompt = NULL;
    char         *str;
#if HAVE_READLINE_HISTORY
    nm_auto_free HISTORY_STATE *saved_history  = NULL;
    HISTORY_STATE               passwd_history = {
        0,
    };
#else
    int start, curpos;
#endif

    va_start(args, prompt_fmt);
    prompt = g_strdup_vprintf(prompt_fmt, args);
    va_end(args);

    rl_initialize();

    /* Hide the actual password */
    if (!echo_on) {
#if HAVE_READLINE_HISTORY
        saved_history = history_get_history_state();
        history_set_history_state(&passwd_history);
#else
        start = where_history();
#endif
        /* stifling history is important as it tells readline to
         * not store anything, otherwise sensitive data could be
         * leaked */
        stifle_history(0);
        rl_redisplay_function = nmc_secret_redisplay;
    }

    str = nmc_readline_helper(nmc_config, prompt);

    /* Restore the non-hiding behavior */
    if (!echo_on) {
        rl_redisplay_function = rl_redisplay;
#if HAVE_READLINE_HISTORY
        history_set_history_state(saved_history);
#else
        curpos = where_history();
        while (curpos > start)
            remove_history(curpos--);
#endif
    }

    return str;
}

/**
 * nmc_rl_gen_func_basic:
 * @text: text to complete
 * @state: readline state; says whether start from scratch (state == 0)
 * @words: strings for completion
 *
 * Basic function generating list of completion strings for readline.
 * See e.g. http://cnswww.cns.cwru.edu/php/chet/readline/readline.html#SEC49
 */
char *
nmc_rl_gen_func_basic(const char *text, int state, const char *const *words)
{
    static int  list_idx, len;
    const char *name;

    if (!state) {
        list_idx = 0;
        len      = strlen(text);
    }

    /* Return the next name which partially matches one from the 'words' list. */
    while ((name = words[list_idx])) {
        list_idx++;

        if (strncmp(name, text, len) == 0)
            return g_strdup(name);
    }
    return NULL;
}

static struct {
    bool   initialized;
    guint  idx;
    char **values;
} _rl_compentry_func_wrap = {0};

static char *
_rl_compentry_func_wrap_fcn(const char *text, int state)
{
    g_return_val_if_fail(_rl_compentry_func_wrap.initialized, NULL);

    while (_rl_compentry_func_wrap.values
           && _rl_compentry_func_wrap.values[_rl_compentry_func_wrap.idx]
           && !g_str_has_prefix(_rl_compentry_func_wrap.values[_rl_compentry_func_wrap.idx], text))
        _rl_compentry_func_wrap.idx++;

    if (!_rl_compentry_func_wrap.values
        || !_rl_compentry_func_wrap.values[_rl_compentry_func_wrap.idx]) {
        g_strfreev(_rl_compentry_func_wrap.values);
        _rl_compentry_func_wrap.values      = NULL;
        _rl_compentry_func_wrap.initialized = FALSE;
        return NULL;
    }

    return g_strdup(_rl_compentry_func_wrap.values[_rl_compentry_func_wrap.idx++]);
}

NmcCompEntryFunc
nmc_rl_compentry_func_wrap(const char *const *values)
{
    g_strfreev(_rl_compentry_func_wrap.values);
    _rl_compentry_func_wrap.values      = g_strdupv((char **) values);
    _rl_compentry_func_wrap.idx         = 0;
    _rl_compentry_func_wrap.initialized = TRUE;
    return _rl_compentry_func_wrap_fcn;
}

char *
nmc_rl_gen_func_ifnames(const char *text, int state)
{
    int              i;
    const GPtrArray *devices;
    const char     **ifnames;
    char            *ret;

    devices = nm_client_get_devices(nm_cli_global_readline->client);
    if (devices->len == 0)
        return NULL;

    ifnames = g_new(const char *, devices->len + 1);
    for (i = 0; i < devices->len; i++) {
        NMDevice   *dev    = g_ptr_array_index(devices, i);
        const char *ifname = nm_device_get_iface(dev);
        ifnames[i]         = ifname;
    }
    ifnames[i] = NULL;

    ret = nmc_rl_gen_func_basic(text, state, ifnames);

    g_free(ifnames);
    return ret;
}

char *nmc_rl_pre_input_deftext;

int
nmc_rl_set_deftext(void)
{
    if (nmc_rl_pre_input_deftext && rl_startup_hook) {
        rl_insert_text(nmc_rl_pre_input_deftext);
        nm_clear_g_free(&nmc_rl_pre_input_deftext);
        rl_startup_hook = NULL;
    }
    return 0;
}

/**
 * nmc_parse_lldp_capabilities:
 * @value: the capabilities value
 *
 * Parses LLDP capabilities flags
 *
 * Returns: a newly allocated string containing capabilities names separated by commas.
 */
char *
nmc_parse_lldp_capabilities(guint value)
{
    /* IEEE Std 802.1AB-2009 - Table 8.4 */
    const char *names[] = {"other",
                           "repeater",
                           "mac-bridge",
                           "wlan-access-point",
                           "router",
                           "telephone",
                           "docsis-cable-device",
                           "station-only",
                           "c-vlan-component",
                           "s-vlan-component",
                           "tpmr"};
    gboolean    first   = TRUE;
    GString    *str;
    int         i;

    if (!value)
        return g_strdup("none");

    str = g_string_new("");

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        if (value & (1 << i)) {
            if (!first)
                g_string_append_c(str, ',');

            first = FALSE;
            value &= ~(1 << i);
            g_string_append(str, names[i]);
        }
    }

    if (value) {
        if (!first)
            g_string_append_c(str, ',');
        g_string_append(str, "reserved");
    }

    return g_string_free(str, FALSE);
}

static void
command_done(GObject *object, GAsyncResult *res, gpointer user_data)
{
    GTask                *task  = G_TASK(res);
    NmCli                *nmc   = user_data;
    gs_free_error GError *error = NULL;

    if (!g_task_propagate_boolean(task, &error)) {
        nmc->return_value = error->code;
        g_string_assign(nmc->return_text, error->message);
    }

    if (!nmc->should_wait)
        g_main_loop_quit(loop);
}

typedef struct {
    const NMCCommand *cmd;
    int               argc;
    char            **argv;
    GTask            *task;
} CmdCall;

static void
call_cmd(NmCli *nmc, GTask *task, const NMCCommand *cmd, int argc, const char *const *argv);

static void
got_client(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    gs_unref_object GTask *task  = NULL;
    gs_free_error GError  *error = NULL;
    CmdCall               *call  = user_data;
    NmCli                 *nmc;

    nm_assert(NM_IS_CLIENT(source_object));

    task = g_steal_pointer(&call->task);
    nmc  = g_task_get_task_data(task);

    nmc->should_wait--;

    if (!g_async_initable_init_finish(G_ASYNC_INITABLE(source_object), res, &error)) {
        g_object_unref(source_object);
        g_task_return_new_error(task,
                                NMCLI_ERROR,
                                NMC_RESULT_ERROR_UNKNOWN,
                                _("Error: Could not create NMClient object: %s."),
                                error->message);
    } else {
        nmc->client = NM_CLIENT(source_object);
        nmc_warn_if_version_mismatch(nmc->client);
        call_cmd(nmc,
                 g_steal_pointer(&task),
                 call->cmd,
                 call->argc,
                 (const char *const *) call->argv);
    }

    g_strfreev(call->argv);
    nm_g_slice_free(call);
}

typedef struct {
    GString *str;
    char     buf[512];
    CmdCall *call;
} CmdStdinData;

static void read_offline_connection_next(GInputStream *stream, CmdStdinData *data);

static void
read_offline_connection_chunk(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GInputStream                   *stream   = G_INPUT_STREAM(source_object);
    CmdStdinData                   *data     = user_data;
    CmdCall                        *call     = data->call;
    gs_unref_object GTask          *task     = NULL;
    nm_auto_unref_keyfile GKeyFile *keyfile  = NULL;
    gs_free char                   *base_dir = NULL;
    GError                         *error    = NULL;
    gssize                          bytes_read;
    NMConnection                   *connection;
    NmCli                          *nmc;

    bytes_read = g_input_stream_read_finish(stream, res, &error);
    if (bytes_read > 0) {
        /* We need to read more. */
        g_string_append_len(data->str, data->buf, bytes_read);
        read_offline_connection_next(stream, data);
        return;
    }

    /* End reached. */

    task = g_steal_pointer(&call->task);
    nmc  = g_task_get_task_data(task);
    nmc->should_wait--;

    if (bytes_read == -1) {
        g_task_return_error(task, error);
        goto finish;
    }

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_data(keyfile,
                                   data->str->str,
                                   data->str->len,
                                   G_KEY_FILE_NONE,
                                   &error)) {
        g_task_return_error(task, error);
        goto finish;
    }

    base_dir = g_get_current_dir();
    connection =
        nm_keyfile_read(keyfile, base_dir, NM_KEYFILE_HANDLER_FLAGS_NONE, NULL, NULL, &error);
    if (!connection) {
        g_task_return_error(task, error);
        goto finish;
    }

    g_ptr_array_add(nmc->offline_connections, connection);
    call->cmd->func(call->cmd, nmc, call->argc, (const char *const *) call->argv);
    g_task_return_boolean(task, TRUE);

finish:
    g_strfreev(call->argv);
    nm_g_slice_free(call);
    g_string_free(data->str, TRUE);
    nm_g_slice_free(data);
}

static void
read_offline_connection_next(GInputStream *stream, CmdStdinData *data)
{
    g_input_stream_read_async(stream,
                              data->buf,
                              sizeof(data->buf),
                              G_PRIORITY_DEFAULT,
                              NULL,
                              read_offline_connection_chunk,
                              data);
}

static void
read_offline_connection(CmdCall *call)
{
    gs_unref_object GInputStream *stream = NULL;
    CmdStdinData                 *data;

    stream     = g_unix_input_stream_new(STDIN_FILENO, TRUE);
    data       = g_slice_new(CmdStdinData);
    data->call = call;
    data->str  = g_string_new_len(NULL, sizeof(data->buf));

    read_offline_connection_next(stream, data);
}

static NMConnection *
dummy_offline_connection(void)
{
    NMConnection *connection;

    connection = nm_simple_connection_new();
    nm_connection_add_setting(connection, nm_setting_connection_new());
    return connection;
}

static void
call_cmd(NmCli *nmc, GTask *task, const NMCCommand *cmd, int argc, const char *const *argv)
{
    CmdCall *call;

    if (nmc->nmc_config.offline) {
        if (!cmd->supports_offline) {
            g_task_return_new_error(task,
                                    NMCLI_ERROR,
                                    NMC_RESULT_ERROR_USER_INPUT,
                                    _("Error: command doesn't support --offline mode."));
            g_object_unref(task);
            return;
        }

        if (!nmc->offline_connections)
            nmc->offline_connections = g_ptr_array_new_full(1, g_object_unref);

        if (cmd->needs_offline_conn) {
            g_return_if_fail(nmc->offline_connections->len == 0);

            if (nmc->complete) {
                g_ptr_array_add(nmc->offline_connections, dummy_offline_connection());
                cmd->func(cmd, nmc, argc, argv);
                g_task_return_boolean(task, TRUE);
                g_object_unref(task);
                return;
            }

            nmc->should_wait++;
            call  = g_slice_new(CmdCall);
            *call = (CmdCall) {
                .cmd  = cmd,
                .argc = argc,
                .argv = nm_strv_dup(argv, argc, TRUE),
                .task = task,
            };
            read_offline_connection(call);
            return;
        } else {
            cmd->func(cmd, nmc, argc, argv);
            g_task_return_boolean(task, TRUE);
            g_object_unref(task);
        }
    } else if (nmc->client || !cmd->needs_client) {
        /* Check whether NetworkManager is running */
        if (cmd->needs_nm_running && !nm_client_get_nm_running(nmc->client)) {
            g_task_return_new_error(task,
                                    NMCLI_ERROR,
                                    NMC_RESULT_ERROR_NM_NOT_RUNNING,
                                    _("Error: NetworkManager is not running."));
        } else {
            cmd->func(cmd, nmc, argc, argv);
            g_task_return_boolean(task, TRUE);
        }

        g_object_unref(task);
    } else {
        nm_assert(nmc->client == NULL);

        nmc->should_wait++;
        call  = g_slice_new(CmdCall);
        *call = (CmdCall) {
            .cmd  = cmd,
            .argc = argc,
            .argv = nm_strv_dup(argv, argc, TRUE),
            .task = task,
        };
        nmc_client_new_async(NULL,
                             got_client,
                             call,
                             NM_CLIENT_INSTANCE_FLAGS,
                             (guint) NM_CLIENT_INSTANCE_FLAGS_NO_AUTO_FETCH_PERMISSIONS,
                             NULL);
    }
}

static void
nmc_complete_help(const char *prefix)
{
    nmc_complete_strings(prefix, "help");
    if (*prefix == '-')
        nmc_complete_strings(prefix, "-help", "--help");
}

/**
 * nmc_do_cmd:
 * @nmc: Client instance
 * @cmds: Command table
 * @cmd: Command
 * @argc: Argument count
 * @argv: Arguments vector. Must be a global variable.
 *
 * Picks the right callback to handle command from the command table.
 * If --help argument follows and the usage callback is specified for the command
 * it calls the usage callback.
 *
 * The command table is terminated with a %NULL command. The terminating
 * entry's handlers are called if the command is empty.
 *
 * The argument vector needs to be a pointer to the global arguments vector that is
 * never freed, since the command handler will be called asynchronously and there's
 * no callback to free the memory in (for simplicity).
 */
void
nmc_do_cmd(NmCli *nmc, const NMCCommand cmds[], const char *cmd, int argc, const char *const *argv)
{
    const NMCCommand      *c;
    gs_unref_object GTask *task = NULL;

    task = nm_g_task_new(NULL, NULL, nmc_do_cmd, command_done, nmc);
    g_task_set_task_data(task, nmc, NULL);

    if (argc == 0 && nmc->complete) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    if (argc == 1 && nmc->complete) {
        for (c = cmds; c->cmd; ++c) {
            if (!*cmd || matches(cmd, c->cmd))
                nmc_print("%s\n", c->cmd);
        }
        nmc_complete_help(cmd);
        g_task_return_boolean(task, TRUE);
        return;
    }

    for (c = cmds; c->cmd; ++c) {
        if (cmd && matches(cmd, c->cmd))
            break;
    }

    if (c->cmd) {
        /* A valid command was specified. */
        if (c->usage && argc == 2 && nmc->complete)
            nmc_complete_help(*(argv + 1));
        if (!nmc->complete && c->usage && nmc_arg_is_help(*(argv + 1))) {
            c->usage();
            g_task_return_boolean(task, TRUE);
        } else {
            call_cmd(nmc, g_steal_pointer(&task), c, argc, (const char *const *) argv);
        }
    } else if (cmd) {
        /* Not a known command. */
        if (nmc_arg_is_help(cmd) && c->usage) {
            c->usage();
            g_task_return_boolean(task, TRUE);
        } else {
            g_task_return_new_error(
                task,
                NMCLI_ERROR,
                NMC_RESULT_ERROR_USER_INPUT,
                _("Error: argument '%s' not understood. Try passing --help instead."),
                cmd);
        }
    } else if (c->func) {
        /* No command, run the default handler. */
        call_cmd(nmc, g_steal_pointer(&task), c, argc, (const char *const *) argv);
    } else {
        /* No command and no default handler. */
        g_task_return_new_error(task,
                                NMCLI_ERROR,
                                NMC_RESULT_ERROR_USER_INPUT,
                                _("Error: missing argument. Try passing --help."));
    }
}

/**
 * nmc_complete_strings:
 * @prefix: a string to match
 * @nargs: the number of elements in @args. Or -1 if @args is a NULL terminated
 *   strv array.
 * @args: the argument list. If @nargs is not -1, then some elements may
 *   be %NULL to indicate to silently skip the values.
 *
 * Prints all the matching candidates for completion. Useful when there's
 * no better way to suggest completion other than a hardcoded string list.
 */
void
nmc_complete_strv(const char *prefix, gssize nargs, const char *const *args)
{
    gsize i, n;

    if (prefix && !prefix[0])
        prefix = NULL;

    if (nargs < 0) {
        nm_assert(nargs == -1);
        n = NM_PTRARRAY_LEN(args);
    } else
        n = (gsize) nargs;

    for (i = 0; i < n; i++) {
        const char *candidate = args[i];

        if (!candidate)
            continue;
        if (prefix && !matches(prefix, candidate))
            continue;

        nmc_print("%s\n", candidate);
    }
}

/**
 * nmc_complete_bool:
 * @prefix: a string to match
 * @...: a %NULL-terminated list of candidate strings
 *
 * Prints all the matching possible boolean values for completion.
 */
void
nmc_complete_bool(const char *prefix)
{
    nmc_complete_strings(prefix, "true", "yes", "on", "false", "no", "off");
}

/**
 * nmc_error_get_simple_message:
 * @error: a GError
 *
 * Returns a simplified message for some errors hard to understand.
 */
const char *
nmc_error_get_simple_message(GError *error)
{
    /* Return a clear message instead of the obscure D-Bus policy error */
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED))
        return _("access denied");
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
        return _("NetworkManager is not running");
    else
        return error->message;
}

/*****************************************************************************/

NM_UTILS_LOOKUP_STR_DEFINE(nm_connectivity_to_string,
                           NMConnectivityState,
                           NM_UTILS_LOOKUP_DEFAULT(N_("unknown")),
                           NM_UTILS_LOOKUP_ITEM(NM_CONNECTIVITY_NONE, N_("none")),
                           NM_UTILS_LOOKUP_ITEM(NM_CONNECTIVITY_PORTAL, N_("portal")),
                           NM_UTILS_LOOKUP_ITEM(NM_CONNECTIVITY_LIMITED, N_("limited")),
                           NM_UTILS_LOOKUP_ITEM(NM_CONNECTIVITY_FULL, N_("full")),
                           NM_UTILS_LOOKUP_ITEM_IGNORE(NM_CONNECTIVITY_UNKNOWN), );
