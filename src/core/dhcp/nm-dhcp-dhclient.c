/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2005 - 2012 Red Hat, Inc.
 */

#include <config.h>
#define __CONFIG_H__

#define _XOPEN_SOURCE
#include <time.h>
#undef _XOPEN_SOURCE

#include "src/core/nm-default-daemon.h"

#if WITH_DHCLIENT

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "libnm-glib-aux/nm-dedup-multi.h"

#include "nm-utils.h"
#include "nm-dhcp-dhclient-utils.h"
#include "nm-dhcp-manager.h"
#include "NetworkManagerUtils.h"
#include "nm-dhcp-listener.h"
#include "nm-dhcp-client-logging.h"

/*****************************************************************************/

static const char *
_addr_family_to_path_part(int addr_family)
{
    nm_assert(NM_IN_SET(addr_family, AF_INET, AF_INET6));
    return (addr_family == AF_INET6) ? "6" : "";
}

/*****************************************************************************/

#define NM_TYPE_DHCP_DHCLIENT (nm_dhcp_dhclient_get_type())
#define NM_DHCP_DHCLIENT(obj) \
    (_NM_G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_DHCP_DHCLIENT, NMDhcpDhclient))
#define NM_DHCP_DHCLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_DHCP_DHCLIENT, NMDhcpDhclientClass))
#define NM_IS_DHCP_DHCLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_DHCP_DHCLIENT))
#define NM_IS_DHCP_DHCLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_DHCP_DHCLIENT))
#define NM_DHCP_DHCLIENT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_DHCP_DHCLIENT, NMDhcpDhclientClass))

typedef struct _NMDhcpDhclient      NMDhcpDhclient;
typedef struct _NMDhcpDhclientClass NMDhcpDhclientClass;

static GType nm_dhcp_dhclient_get_type(void);

/*****************************************************************************/

typedef struct {
    char           *conf_file;
    const char     *def_leasefile;
    char           *lease_file;
    char           *pid_file;
    NMDhcpListener *dhcp_listener;
} NMDhcpDhclientPrivate;

struct _NMDhcpDhclient {
    NMDhcpClient          parent;
    NMDhcpDhclientPrivate _priv;
};

struct _NMDhcpDhclientClass {
    NMDhcpClientClass parent;
};

G_DEFINE_TYPE(NMDhcpDhclient, nm_dhcp_dhclient, NM_TYPE_DHCP_CLIENT)

#define NM_DHCP_DHCLIENT_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMDhcpDhclient, NM_IS_DHCP_DHCLIENT)

/*****************************************************************************/

static GBytes *read_duid_from_lease(NMDhcpDhclient *self);

/*****************************************************************************/

static const char *
nm_dhcp_dhclient_get_path(void)
{
    return nm_utils_find_helper("dhclient", DHCLIENT_PATH, NULL);
}

/**
 * get_dhclient_leasefile():
 * @addr_family: AF_INET or AF_INET6
 * @iface: the interface name of the device on which DHCP will be done
 * @uuid: the connection UUID to which the returned lease should belong
 * @out_preferred_path: on return, the "most preferred" leasefile path
 *
 * Returns the path of an existing leasefile (if any) for this interface and
 * connection UUID.  Also returns the "most preferred" leasefile path, which
 * may be different than any found leasefile.
 *
 * Returns: an existing leasefile, or %NULL if no matching leasefile could be found
 */
static char *
get_dhclient_leasefile(int         addr_family,
                       const char *iface,
                       const char *uuid,
                       char      **out_preferred_path)
{
    gs_free char *path = NULL;

    if (nm_dhcp_utils_get_leasefile_path(addr_family, "dhclient", iface, uuid, &path)) {
        NM_SET_OUT(out_preferred_path, g_strdup(path));
        return g_steal_pointer(&path);
    }

    NM_SET_OUT(out_preferred_path, g_steal_pointer(&path));

    /* If the leasefile we're looking for doesn't exist yet in the new location
     * (eg, /var/lib/NetworkManager) then look in old locations to maintain
     * backwards compatibility with external tools (like dracut) that put
     * leasefiles there.
     */

    /* Old Debian, SUSE, and Mandriva location */
    g_free(path);
    path = g_strdup_printf(LOCALSTATEDIR "/lib/dhcp/dhclient%s-%s-%s.lease",
                           _addr_family_to_path_part(addr_family),
                           uuid,
                           iface);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&path);

    /* Old Red Hat and Fedora location */
    g_free(path);
    path = g_strdup_printf(LOCALSTATEDIR "/lib/dhclient/dhclient%s-%s-%s.lease",
                           _addr_family_to_path_part(addr_family),
                           uuid,
                           iface);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&path);

    /* Fail */
    return NULL;
}

static char *
find_existing_config(NMDhcpDhclient *self, int addr_family, const char *iface, const char *uuid)
{
    char *path;

    /* NetworkManager-overridden configuration can be used to ship DHCP config
     * with NetworkManager itself. It can be uuid-specific, device-specific
     * or generic.
     */
    if (uuid) {
        path = g_strdup_printf(NMCONFDIR "/dhclient%s-%s.conf",
                               _addr_family_to_path_part(addr_family),
                               uuid);
        _LOGD("looking for existing config %s", path);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }

    path = g_strdup_printf(NMCONFDIR "/dhclient%s-%s.conf",
                           _addr_family_to_path_part(addr_family),
                           iface);
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    path = g_strdup_printf(NMCONFDIR "/dhclient%s.conf", _addr_family_to_path_part(addr_family));
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    /* Distribution's dhclient configuration is used so that we can use
     * configuration shipped with dhclient (if any).
     *
     * This replaces conditional compilation based on distribution name. Fedora
     * and Debian store the configs in /etc/dhcp while upstream defaults to /etc
     * which is then used by many other distributions. Some distributions
     * (including Fedora) don't even provide a default configuration file.
     */
    path = g_strdup_printf(SYSCONFDIR "/dhcp/dhclient%s-%s.conf",
                           _addr_family_to_path_part(addr_family),
                           iface);
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    path = g_strdup_printf(SYSCONFDIR "/dhclient%s-%s.conf",
                           _addr_family_to_path_part(addr_family),
                           iface);
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    path =
        g_strdup_printf(SYSCONFDIR "/dhcp/dhclient%s.conf", _addr_family_to_path_part(addr_family));
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    path = g_strdup_printf(SYSCONFDIR "/dhclient%s.conf", _addr_family_to_path_part(addr_family));
    _LOGD("looking for existing config %s", path);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);

    return NULL;
}

/* NM provides interface-specific options; thus the same dhclient config
 * file cannot be used since DHCP transactions can happen in parallel.
 * Since some distros don't have default per-interface dhclient config files,
 * read their single config file and merge that into a custom per-interface
 * config file along with the NM options.
 */
static char *
create_dhclient_config(NMDhcpDhclient     *self,
                       int                 addr_family,
                       const char         *iface,
                       const char         *uuid,
                       GBytes             *client_id,
                       gboolean            send_client_id,
                       const char         *anycast_address,
                       const char         *hostname,
                       guint32             timeout,
                       gboolean            use_fqdn,
                       NMDhcpHostnameFlags hostname_flags,
                       const char         *mud_url,
                       const char *const  *reject_servers,
                       GBytes            **out_new_client_id)
{
    gs_free char *orig_path    = NULL;
    gs_free char *orig_content = NULL;
    char         *new_path     = NULL;
    gs_free char *new_content  = NULL;
    GError       *error        = NULL;

    g_return_val_if_fail(iface != NULL, NULL);

    new_path = g_strdup_printf(NMSTATEDIR "/dhclient%s-%s.conf",
                               _addr_family_to_path_part(addr_family),
                               iface);
    _LOGD("creating composite dhclient config %s", new_path);

    orig_path = find_existing_config(self, addr_family, iface, uuid);
    if (orig_path)
        _LOGD("merging existing dhclient config %s", orig_path);
    else
        _LOGD("no existing dhclient configuration to merge");

    if (orig_path && g_file_test(orig_path, G_FILE_TEST_EXISTS)) {
        if (!g_file_get_contents(orig_path, &orig_content, NULL, &error)) {
            _LOGW("error reading dhclient configuration %s: %s", orig_path, error->message);
            g_error_free(error);
        }
    }

    new_content = nm_dhcp_dhclient_create_config(iface,
                                                 addr_family,
                                                 client_id,
                                                 send_client_id,
                                                 anycast_address,
                                                 hostname,
                                                 timeout,
                                                 use_fqdn,
                                                 hostname_flags,
                                                 mud_url,
                                                 reject_servers,
                                                 orig_path,
                                                 orig_content,
                                                 out_new_client_id);
    nm_assert(new_content);

    if (!g_file_set_contents(new_path, new_content, -1, &error)) {
        _LOGW("error creating dhclient configuration: %s", error->message);
        g_error_free(error);
        g_free(new_path);
        return NULL;
    }

    return new_path;
}

static gboolean
dhclient_start(NMDhcpClient *client,
               gboolean      set_mode,
               gboolean      release,
               gboolean      set_duid,
               pid_t        *out_pid,
               GError      **error)
{
    NMDhcpDhclient              *self = NM_DHCP_DHCLIENT(client);
    NMDhcpDhclientPrivate       *priv = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    gs_unref_ptrarray GPtrArray *argv = NULL;
    pid_t                        pid;
    gs_free_error GError        *local = NULL;
    const char                  *iface;
    const char                  *uuid;
    const char                  *system_bus_address;
    const char                  *dhclient_path;
    char                        *binary_name;
    gs_free char                *cmd_str                  = NULL;
    gs_free char                *pid_file                 = NULL;
    gs_free char                *system_bus_address_env   = NULL;
    gs_free char                *preferred_leasefile_path = NULL;
    int                          addr_family;
    const NMDhcpClientConfig    *client_config;
    char                         pd_length_str[16];

    g_return_val_if_fail(!priv->pid_file, FALSE);
    client_config = nm_dhcp_client_get_config(client);
    addr_family   = client_config->addr_family;

    NM_SET_OUT(out_pid, 0);

    dhclient_path = nm_dhcp_dhclient_get_path();
    if (!dhclient_path) {
        nm_utils_error_set_literal(error, NM_UTILS_ERROR_UNKNOWN, "dhclient binary not found");
        return FALSE;
    }

    iface = client_config->iface;
    uuid  = client_config->uuid;

    pid_file = g_strdup_printf(NMRUNDIR "/dhclient%s-%s.pid",
                               _addr_family_to_path_part(addr_family),
                               iface);

    /* Kill any existing dhclient from the pidfile */
    binary_name = g_path_get_basename(dhclient_path);
    nm_dhcp_client_stop_existing(pid_file, binary_name);
    g_free(binary_name);

    if (release) {
        /* release doesn't use the pidfile after killing an old client */
        nm_clear_g_free(&pid_file);
    }

    g_free(priv->lease_file);
    priv->lease_file = get_dhclient_leasefile(addr_family, iface, uuid, &preferred_leasefile_path);
    nm_assert(preferred_leasefile_path);
    if (!priv->lease_file) {
        /* No existing leasefile, dhclient will create one at the preferred path */
        priv->lease_file = g_steal_pointer(&preferred_leasefile_path);
    } else if (!nm_streq0(priv->lease_file, preferred_leasefile_path)) {
        gs_unref_object GFile *src = g_file_new_for_path(priv->lease_file);
        gs_unref_object GFile *dst = g_file_new_for_path(preferred_leasefile_path);

        /* Try to copy the existing leasefile to the preferred location */
        if (!g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &local)) {
            gs_free char *s_path = NULL;
            gs_free char *d_path = NULL;

            /* Failure; just use the existing leasefile */
            _LOGW("failed to copy leasefile %s to %s: %s",
                  (s_path = g_file_get_path(src)),
                  (d_path = g_file_get_path(dst)),
                  local->message);
            g_clear_error(&local);
        } else {
            /* Success; use the preferred leasefile path */
            g_free(priv->lease_file);
            priv->lease_file = g_file_get_path(dst);
        }
    }

    /* Save the DUID to the leasefile dhclient will actually use */
    if (set_duid && addr_family == AF_INET6) {
        if (!nm_dhcp_dhclient_save_duid(priv->lease_file,
                                        nm_dhcp_client_get_effective_client_id(client),
                                        client_config->v6.enforce_duid,
                                        &local)) {
            nm_utils_error_set(error,
                               NM_UTILS_ERROR_UNKNOWN,
                               "failed to save DUID to '%s': %s",
                               priv->lease_file,
                               local->message);
            return FALSE;
        }
    }

    argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer) dhclient_path);

    g_ptr_array_add(argv, (gpointer) "-d");

    /* Be quiet. dhclient logs to syslog anyway. And we duplicate the syslog
     * to stderr in case of NM running with --debug.
     */
    g_ptr_array_add(argv, (gpointer) "-q");

    if (release)
        g_ptr_array_add(argv, (gpointer) "-r");

    if (!release && client_config->addr_family == AF_INET && client_config->v4.request_broadcast) {
        g_ptr_array_add(argv, (gpointer) "-B");
    }

    if (addr_family == AF_INET6) {
        guint       prefixes = client_config->v6.needed_prefixes;
        const char *mode_opt;

        g_ptr_array_add(argv, (gpointer) "-6");

        if (!set_mode)
            mode_opt = NULL;
        else if (!client_config->v6.info_only)
            mode_opt = "-N";
        else if (prefixes == 0)
            mode_opt = "-S";
        else
            mode_opt = NULL;

        if (mode_opt)
            g_ptr_array_add(argv, (gpointer) mode_opt);

        if (prefixes > 0 && client_config->v6.pd_hint_length > 0) {
            if (!IN6_IS_ADDR_UNSPECIFIED(&client_config->v6.pd_hint_addr)) {
                _LOGW("dhclient only supports a length as prefix delegation hint, not a prefix");
            }

            nm_sprintf_buf(pd_length_str, "%u", client_config->v6.pd_hint_length);
            g_ptr_array_add(argv, "--prefix-len-hint");
            g_ptr_array_add(argv, pd_length_str);
        }

        while (prefixes--)
            g_ptr_array_add(argv, (gpointer) "-P");
    }
    g_ptr_array_add(argv, (gpointer) "-sf"); /* Set script file */
    g_ptr_array_add(argv, (gpointer) nm_dhcp_helper_path);

    if (pid_file) {
        g_ptr_array_add(argv, (gpointer) "-pf"); /* Set pid file */
        g_ptr_array_add(argv, (gpointer) pid_file);
    }

    g_ptr_array_add(argv, (gpointer) "-lf"); /* Set lease file */
    g_ptr_array_add(argv, (gpointer) priv->lease_file);

    if (priv->conf_file) {
        g_ptr_array_add(argv, (gpointer) "-cf"); /* Set interface config file */
        g_ptr_array_add(argv, (gpointer) priv->conf_file);
    }

    if (client_config->v4.dscp_explicit) {
        _LOGW("dhclient does not support specifying a custom DSCP value; the TOS field will be set "
              "to LOWDELAY (0x10).");
    }

    if (client_config->v4.ipv6_only_preferred) {
        _LOGW("the dhclient backend does not support the \"IPv6-Only Preferred\" option; ignoring "
              "it");
    }

    /* Usually the system bus address is well-known; but if it's supposed
     * to be something else, we need to push it to dhclient, since dhclient
     * sanitizes the environment it gives the action scripts.
     */
    system_bus_address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (system_bus_address) {
        system_bus_address_env = g_strdup_printf("DBUS_SYSTEM_BUS_ADDRESS=%s", system_bus_address);
        g_ptr_array_add(argv, (gpointer) "-e");
        g_ptr_array_add(argv, (gpointer) system_bus_address_env);
    }

    g_ptr_array_add(argv, (gpointer) iface);
    g_ptr_array_add(argv, NULL);

    _LOGD("running: %s", (cmd_str = g_strjoinv(" ", (char **) argv->pdata)));

    if (!g_spawn_async(NULL,
                       (char **) argv->pdata,
                       NULL,
                       G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL
                           | G_SPAWN_STDERR_TO_DEV_NULL,
                       nm_utils_setpgid,
                       NULL,
                       &pid,
                       &local)) {
        nm_utils_error_set(error,
                           NM_UTILS_ERROR_UNKNOWN,
                           "dhclient failed to start: %s",
                           local->message);
        return FALSE;
    }

    _LOGI("dhclient started with pid %lld", (long long int) pid);

    if (!release)
        nm_dhcp_client_watch_child(client, pid);

    priv->pid_file = g_steal_pointer(&pid_file);

    NM_SET_OUT(out_pid, pid);
    return TRUE;
}

static gboolean
ip4_start(NMDhcpClient *client, GError **error)
{
    NMDhcpDhclient           *self          = NM_DHCP_DHCLIENT(client);
    NMDhcpDhclientPrivate    *priv          = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    gs_unref_bytes GBytes    *new_client_id = NULL;
    const NMDhcpClientConfig *client_config;

    client_config = nm_dhcp_client_get_config(client);

    nm_assert(client_config->addr_family == AF_INET);

    priv->conf_file = create_dhclient_config(self,
                                             AF_INET,
                                             client_config->iface,
                                             client_config->uuid,
                                             client_config->client_id,
                                             client_config->v4.send_client_id,
                                             client_config->anycast_address,
                                             client_config->hostname,
                                             client_config->timeout,
                                             client_config->use_fqdn,
                                             client_config->hostname_flags,
                                             client_config->mud_url,
                                             client_config->reject_servers,
                                             &new_client_id);
    if (!priv->conf_file) {
        nm_utils_error_set_literal(error,
                                   NM_UTILS_ERROR_UNKNOWN,
                                   "error creating dhclient configuration file");
        return FALSE;
    }

    /* Note that the effective-client-id for IPv4 here might be unknown/NULL. */
    nm_assert(!new_client_id || !client_config->client_id);
    nm_dhcp_client_set_effective_client_id(client, client_config->client_id ?: new_client_id);

    return dhclient_start(client, FALSE, FALSE, FALSE, NULL, error);
}

static gboolean
ip6_start(NMDhcpClient *client, const struct in6_addr *ll_addr, GError **error)
{
    NMDhcpDhclient           *self = NM_DHCP_DHCLIENT(client);
    NMDhcpDhclientPrivate    *priv = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    const NMDhcpClientConfig *config;
    gs_unref_bytes GBytes    *effective_client_id = NULL;

    config = nm_dhcp_client_get_config(client);

    nm_assert(config->addr_family == AF_INET6);

    if (config->v6.iaid_explicit)
        _LOGW("dhclient does not support specifying an IAID for DHCPv6, it will be ignored");

    priv->conf_file = create_dhclient_config(self,
                                             AF_INET6,
                                             config->iface,
                                             config->uuid,
                                             NULL,
                                             TRUE,
                                             config->anycast_address,
                                             config->hostname,
                                             config->timeout,
                                             TRUE,
                                             config->hostname_flags,
                                             config->mud_url,
                                             NULL,
                                             NULL);
    if (!priv->conf_file) {
        nm_utils_error_set_literal(error,
                                   NM_UTILS_ERROR_UNKNOWN,
                                   "error creating dhclient configuration file");
        return FALSE;
    }

    nm_assert(config->client_id);
    if (!config->v6.enforce_duid)
        effective_client_id = read_duid_from_lease(self);
    nm_dhcp_client_set_effective_client_id(client, effective_client_id ?: config->client_id);

    return dhclient_start(client, TRUE, FALSE, TRUE, NULL, error);
}

static void
stop(NMDhcpClient *client, gboolean release)
{
    NMDhcpDhclient        *self = NM_DHCP_DHCLIENT(client);
    NMDhcpDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    int                    errsv;

    NM_DHCP_CLIENT_CLASS(nm_dhcp_dhclient_parent_class)->stop(client, release);

    if (priv->conf_file)
        if (remove(priv->conf_file) == -1) {
            errsv = errno;
            _LOGD("could not remove dhcp config file \"%s\": %d (%s)",
                  priv->conf_file,
                  errsv,
                  nm_strerror_native(errsv));
        }
    if (priv->pid_file) {
        if (remove(priv->pid_file) == -1) {
            errsv = errno;
            _LOGD("could not remove dhcp pid file \"%s\": %s (%d)",
                  priv->pid_file,
                  nm_strerror_native(errsv),
                  errsv);
        }
        nm_clear_g_free(&priv->pid_file);
    }

    if (release) {
        pid_t rpid = -1;

        if (dhclient_start(client, FALSE, TRUE, FALSE, &rpid, NULL)) {
            /* Wait a few seconds for the release to happen */
            nm_dhcp_client_stop_pid(rpid, nm_dhcp_client_get_iface(client));
        }
    }
}

static GBytes *
read_duid_from_lease(NMDhcpDhclient *self)
{
    NMDhcpClient             *client = NM_DHCP_CLIENT(self);
    NMDhcpDhclientPrivate    *priv   = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    const NMDhcpClientConfig *client_config;
    GBytes                   *duid      = NULL;
    gs_free char             *leasefile = NULL;
    GError                   *error     = NULL;

    client_config = nm_dhcp_client_get_config(client);

    /* Look in interface-specific leasefile first for backwards compat */
    leasefile = get_dhclient_leasefile(AF_INET6,
                                       nm_dhcp_client_get_iface(client),
                                       client_config->uuid,
                                       NULL);
    if (leasefile) {
        _LOGD("looking for DUID in '%s'", leasefile);
        duid = nm_dhcp_dhclient_read_duid(leasefile, &error);
        if (error) {
            _LOGW("failed to read leasefile '%s': %s", leasefile, error->message);
            g_clear_error(&error);
        }
        if (duid)
            return duid;
    }

    /* Otherwise, read the default machine-wide DUID */
    _LOGD("looking for default DUID in '%s'", priv->def_leasefile);
    duid = nm_dhcp_dhclient_read_duid(priv->def_leasefile, &error);
    if (error) {
        _LOGW("failed to read leasefile '%s': %s", priv->def_leasefile, error->message);
        g_clear_error(&error);
    }

    return duid;
}

/*****************************************************************************/

static void
nm_dhcp_dhclient_init(NMDhcpDhclient *self)
{
    static const char *const FILES[] = {
        SYSCONFDIR "/dhclient6.leases", /* default */
        LOCALSTATEDIR "/lib/dhcp/dhclient6.leases",
        LOCALSTATEDIR "/lib/dhclient/dhclient6.leases",
    };
    NMDhcpDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE(self);
    int                    i;

    priv->def_leasefile = FILES[0];
    for (i = 0; i < G_N_ELEMENTS(FILES); i++) {
        if (g_file_test(FILES[i], G_FILE_TEST_EXISTS)) {
            priv->def_leasefile = FILES[i];
            break;
        }
    }

    priv->dhcp_listener = g_object_ref(nm_dhcp_listener_get());
    g_signal_connect(priv->dhcp_listener,
                     NM_DHCP_LISTENER_EVENT,
                     G_CALLBACK(nm_dhcp_client_handle_event),
                     self);
}

static void
dispose(GObject *object)
{
    NMDhcpDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE(object);

    if (priv->dhcp_listener) {
        g_signal_handlers_disconnect_by_func(priv->dhcp_listener,
                                             G_CALLBACK(nm_dhcp_client_handle_event),
                                             NM_DHCP_DHCLIENT(object));
        g_clear_object(&priv->dhcp_listener);
    }

    nm_clear_g_free(&priv->pid_file);
    nm_clear_g_free(&priv->conf_file);
    nm_clear_g_free(&priv->lease_file);

    G_OBJECT_CLASS(nm_dhcp_dhclient_parent_class)->dispose(object);
}

static void
nm_dhcp_dhclient_class_init(NMDhcpDhclientClass *dhclient_class)
{
    NMDhcpClientClass *client_class = NM_DHCP_CLIENT_CLASS(dhclient_class);
    GObjectClass      *object_class = G_OBJECT_CLASS(dhclient_class);

    object_class->dispose = dispose;

    client_class->ip4_start = ip4_start;
    client_class->ip6_start = ip6_start;
    client_class->stop      = stop;
}

const NMDhcpClientFactory _nm_dhcp_client_factory_dhclient = {
    .name       = "dhclient",
    .get_type_4 = nm_dhcp_dhclient_get_type,
    .get_type_6 = nm_dhcp_dhclient_get_type,
    .get_path   = nm_dhcp_dhclient_get_path,
};

#endif /* WITH_DHCLIENT */
