/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2007 - 2018 Red Hat, Inc.
 * Copyright (C) 2007 - 2008 Novell, Inc.
 */

#include "libnm-core-impl/nm-default-libnm-core.h"

#include "nm-connection.h"

#include <arpa/inet.h>

#include "libnm-glib-aux/nm-uuid.h"
#include "libnm-glib-aux/nm-ref-string.h"
#include "nm-connection-private.h"
#include "nm-utils.h"
#include "nm-setting-private.h"
#include "libnm-core-intern/nm-core-internal.h"

/**
 * SECTION:nm-connection
 * @short_description: Describes a connection to specific network or provider
 *
 * An #NMConnection describes all the settings and configuration values that
 * are necessary to configure network devices for operation on a specific
 * network.  Connections are the fundamental operating object for
 * NetworkManager; no device is connected without a #NMConnection, or
 * disconnected without having been connected with a #NMConnection.
 *
 * Each #NMConnection contains a list of #NMSetting objects usually referenced
 * by name (using nm_connection_get_setting_by_name()) or by type (with
 * nm_connection_get_setting()).  The settings describe the actual parameters
 * with which the network devices are configured, including device-specific
 * parameters (MTU, SSID, APN, channel, rate, etc) and IP-level parameters
 * (addresses, routes, addressing methods, etc).
 *
 */

/*****************************************************************************/

enum {
    SECRETS_UPDATED,
    SECRETS_CLEARED,
    CHANGED,
    LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_INTERFACE(NMConnection, nm_connection, G_TYPE_OBJECT)

/*****************************************************************************/

static gboolean _nm_connection_clear_settings(NMConnection *connection, NMConnectionPrivate *priv);

/*****************************************************************************/

void
_nm_connection_private_clear(NMConnectionPrivate *priv)
{
    if (priv->self) {
        _nm_connection_clear_settings(priv->self, priv);
        nm_clear_pointer(&priv->path, nm_ref_string_unref);
        priv->self = NULL;
    }
}

static void
_nm_connection_private_free(gpointer data)
{
    NMConnectionPrivate *priv = data;

    _nm_connection_private_clear(priv);

    nm_g_slice_free(priv);
}

static NMConnectionPrivate *
_nm_connection_get_private_from_qdata(NMConnection *connection)
{
    GQuark               key;
    NMConnectionPrivate *priv;

    nm_assert(NM_IS_CONNECTION(connection));
    nm_assert(!NM_IS_SIMPLE_CONNECTION(connection));

    key = NM_CACHED_QUARK("NMConnectionPrivate");

    priv = g_object_get_qdata((GObject *) connection, key);
    if (G_UNLIKELY(!priv)) {
        priv  = g_slice_new(NMConnectionPrivate);
        *priv = (NMConnectionPrivate) {
            .self = connection,
        };
        g_object_set_qdata_full((GObject *) connection, key, priv, _nm_connection_private_free);
    }

    return priv;
}

#define NM_CONNECTION_GET_PRIVATE(connection)                                  \
    ({                                                                         \
        NMConnection        *_connection = (connection);                       \
        NMConnectionPrivate *_priv;                                            \
                                                                               \
        if (G_LIKELY(NM_IS_SIMPLE_CONNECTION(_connection)))                    \
            _priv = _NM_SIMPLE_CONNECTION_GET_CONNECTION_PRIVATE(_connection); \
        else                                                                   \
            _priv = _nm_connection_get_private_from_qdata(_connection);        \
                                                                               \
        nm_assert(_priv && _priv->self == _connection);                        \
                                                                               \
        _priv;                                                                 \
    })

/*****************************************************************************/

static void
_signal_emit_changed(NMConnection *self)
{
    g_signal_emit(self, signals[CHANGED], 0);
}

static void
_signal_emit_secrets_updated(NMConnection *self, const char *setting_name)
{
    g_signal_emit(self, signals[SECRETS_UPDATED], 0, setting_name);
}

static void
_signal_emit_secrets_cleared(NMConnection *self)
{
    g_signal_emit(self, signals[SECRETS_CLEARED], 0);
}

/*****************************************************************************/

static void
_setting_notify_changed_cb(NMSetting *setting, GParamSpec *pspec, NMConnection *self)
{
    _signal_emit_changed(self);
}

static void
_setting_notify_connect(NMConnection *connection, NMSetting *setting)
{
    g_signal_connect(setting, "notify", G_CALLBACK(_setting_notify_changed_cb), connection);
}

static void
_setting_notify_disconnect(NMConnection *connection, NMSetting *setting)
{
    g_signal_handlers_disconnect_by_func(setting,
                                         G_CALLBACK(_setting_notify_changed_cb),
                                         connection);
}

static void
_setting_notify_block(NMConnection *connection, NMSetting *setting)
{
    g_signal_handlers_block_by_func(setting, G_CALLBACK(_setting_notify_changed_cb), connection);
}

static void
_setting_notify_unblock(NMConnection *connection, NMSetting *setting)
{
    g_signal_handlers_unblock_by_func(setting, G_CALLBACK(_setting_notify_changed_cb), connection);
}

/*****************************************************************************/

static gboolean
_nm_connection_clear_settings(NMConnection *connection, NMConnectionPrivate *priv)
{
    gboolean changed = FALSE;
    int      i;

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        if (priv->settings[i]) {
            _setting_notify_disconnect(connection, priv->settings[i]);
            g_clear_object(&priv->settings[i]);
            changed = TRUE;
        }
    }
    return changed;
}

static void
_nm_connection_add_setting(NMConnection *connection, NMSetting *setting)
{
    const NMMetaSettingInfo *setting_info;
    NMConnectionPrivate     *priv;
    NMSetting               *s_old;

    nm_assert(NM_IS_CONNECTION(connection));
    nm_assert(NM_IS_SETTING(setting));

    setting_info = _nm_meta_setting_info_from_class(NM_SETTING_GET_CLASS(setting));
    if (!setting_info)
        g_return_if_reached();

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    s_old = priv->settings[setting_info->meta_type];
    if (s_old == setting) {
        g_object_unref(s_old);
        return;
    }

    priv->settings[setting_info->meta_type] = setting;

    _setting_notify_connect(connection, setting);

    if (s_old) {
        _setting_notify_disconnect(connection, s_old);
        g_object_unref(s_old);
    }
}

/**
 * nm_connection_add_setting:
 * @connection: a #NMConnection
 * @setting: (transfer full): the #NMSetting to add to the connection object
 *
 * Adds a #NMSetting to the connection, replacing any previous #NMSetting of the
 * same name which has previously been added to the #NMConnection.  The
 * connection takes ownership of the #NMSetting object and does not increase
 * the setting object's reference count.
 **/
void
nm_connection_add_setting(NMConnection *connection, NMSetting *setting)
{
    g_return_if_fail(NM_IS_CONNECTION(connection));
    g_return_if_fail(NM_IS_SETTING(setting));

    _nm_connection_add_setting(connection, setting);
    _signal_emit_changed(connection);
}

gboolean
_nm_connection_remove_setting(NMConnection *connection, GType setting_type)
{
    NMConnectionPrivate     *priv;
    NMSetting               *setting;
    const NMMetaSettingInfo *setting_info;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);

    setting_info = _nm_meta_setting_info_from_gtype(setting_type);
    if (!setting_info)
        g_return_val_if_reached(FALSE);

    priv    = NM_CONNECTION_GET_PRIVATE(connection);
    setting = g_steal_pointer(&priv->settings[setting_info->meta_type]);
    if (!setting)
        return FALSE;

    _setting_notify_disconnect(connection, setting);
    _signal_emit_changed(connection);
    g_object_unref(setting);
    return TRUE;
}

/**
 * nm_connection_remove_setting:
 * @connection: a #NMConnection
 * @setting_type: the #GType of the setting object to remove
 *
 * Removes the #NMSetting with the given #GType from the #NMConnection.  This
 * operation dereferences the #NMSetting object.
 **/
void
nm_connection_remove_setting(NMConnection *connection, GType setting_type)
{
    _nm_connection_remove_setting(connection, setting_type);
}

static gpointer
_get_setting_by_metatype(NMConnectionPrivate *priv, NMMetaSettingType meta_type)
{
    nm_assert(priv);
    nm_assert(_NM_INT_NOT_NEGATIVE(meta_type));
    nm_assert(meta_type < _NM_META_SETTING_TYPE_NUM);

    return priv->settings[meta_type];
}

/* The "unsafe" part here is that _nm_connection_get_setting_by_metatype() has a compile
 * time check that meta_type is valid. With the unsafe variant, the caller must ensure that,
 * and we only get an nm_assert() check -- which is basically nothing. */
gpointer
_nm_connection_get_setting_by_metatype_unsafe(NMConnection *connection, NMMetaSettingType meta_type)
{
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    return _get_setting_by_metatype(NM_CONNECTION_GET_PRIVATE(connection), meta_type);
}

/**
 * nm_connection_get_setting:
 * @connection: a #NMConnection
 * @setting_type: the #GType of the setting object to return
 *
 * Gets the #NMSetting with the given #GType, if one has been previously added
 * to the #NMConnection.
 *
 * Returns: (transfer none): the #NMSetting, or %NULL if no setting of that type was previously
 * added to the #NMConnection
 **/
NMSetting *
nm_connection_get_setting(NMConnection *connection, GType setting_type)
{
    NMSetting               *setting;
    const NMMetaSettingInfo *setting_info;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    setting_info = _nm_meta_setting_info_from_gtype(setting_type);

    if (!setting_info)
        g_return_val_if_reached(NULL);

    setting = NM_CONNECTION_GET_PRIVATE(connection)->settings[setting_info->meta_type];

    nm_assert(!setting || G_TYPE_CHECK_INSTANCE_TYPE(setting, setting_type));

    return setting;
}

NMSettingIPConfig *
nm_connection_get_setting_ip_config(NMConnection *connection, int addr_family)
{
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    nm_assert_addr_family(addr_family);

    return NM_SETTING_IP_CONFIG(_get_setting_by_metatype(NM_CONNECTION_GET_PRIVATE(connection),
                                                         (addr_family == AF_INET)
                                                             ? NM_META_SETTING_TYPE_IP4_CONFIG
                                                             : NM_META_SETTING_TYPE_IP6_CONFIG));
}

/**
 * nm_connection_get_setting_by_name:
 * @connection: a #NMConnection
 * @name: a setting name
 *
 * Gets the #NMSetting with the given name, if one has been previously added
 * the #NMConnection.
 *
 * Returns: (transfer none): the #NMSetting, or %NULL if no setting with that name was previously
 * added to the #NMConnection
 **/
NMSetting *
nm_connection_get_setting_by_name(NMConnection *connection, const char *name)
{
    const NMMetaSettingInfo *setting_info;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    setting_info = nm_meta_setting_infos_by_name(name);
    return setting_info ? _get_setting_by_metatype(NM_CONNECTION_GET_PRIVATE(connection),
                                                   setting_info->meta_type)
                        : NULL;
}

/*****************************************************************************/

gpointer /* (NMSetting *) */
_nm_connection_check_main_setting(NMConnection *connection,
                                  const char   *setting_name,
                                  GError      **error)
{
    NMSetting *setting;

    nm_assert(NM_IS_CONNECTION(connection));
    nm_assert(setting_name);

    if (!nm_connection_is_type(connection, setting_name)) {
        nm_utils_error_set(error,
                           NM_UTILS_ERROR_CONNECTION_AVAILABLE_INCOMPATIBLE,
                           "connection type is not \"%s\"",
                           setting_name);
        return NULL;
    }

    setting = nm_connection_get_setting_by_name(connection, setting_name);
    if (!setting) {
        nm_utils_error_set(error,
                           NM_UTILS_ERROR_CONNECTION_AVAILABLE_INCOMPATIBLE,
                           "connection misses \"%s\" settings",
                           setting_name);
        return NULL;
    }

    return setting;
}

/*****************************************************************************/

static gboolean
validate_permissions_type(GVariant *variant, GError **error)
{
    GVariant *s_con;
    GVariant *permissions;
    gboolean  valid = TRUE;

    /* Ensure the connection::permissions item (if present) is the correct
     * type, otherwise the g_object_set() will throw a warning and ignore the
     * error, leaving us with no permissions.
     */
    s_con = g_variant_lookup_value(variant,
                                   NM_SETTING_CONNECTION_SETTING_NAME,
                                   NM_VARIANT_TYPE_SETTING);
    if (!s_con)
        return TRUE;

    permissions = g_variant_lookup_value(s_con, NM_SETTING_CONNECTION_PERMISSIONS, NULL);
    if (permissions) {
        if (!g_variant_is_of_type(permissions, G_VARIANT_TYPE_STRING_ARRAY)) {
            g_set_error_literal(error,
                                NM_CONNECTION_ERROR,
                                NM_CONNECTION_ERROR_INVALID_PROPERTY,
                                _("wrong type; should be a list of strings."));
            g_prefix_error(error,
                           "%s.%s: ",
                           NM_SETTING_CONNECTION_SETTING_NAME,
                           NM_SETTING_CONNECTION_PERMISSIONS);
            valid = FALSE;
        }
        g_variant_unref(permissions);
    }

    g_variant_unref(s_con);
    return valid;
}

static void
_auto_settings(NMSetting ***p_settings)
{
    NMSetting **settings = *p_settings;
    int         i;

    if (settings) {
        for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
            if (settings[i])
                g_object_unref(settings[i]);
        }
    }
}

/**
 * _nm_connection_replace_settings:
 * @connection: a #NMConnection
 * @new_settings: a #GVariant of type %NM_VARIANT_TYPE_CONNECTION, with the new settings
 * @parse_flags: flags.
 * @error: location to store error, or %NULL
 *
 * Replaces @connection's settings with @new_settings (which must be
 * syntactically valid, and describe a known type of connection, but does not
 * need to result in a connection that passes nm_connection_verify()).
 *
 * Returns: %TRUE if connection was updated, %FALSE if @new_settings could not
 *   be deserialized (in which case @connection will be unchanged).
 *   Only exception is the NM_SETTING_PARSE_FLAGS_NORMALIZE flag: if normalization
 *   fails, the input @connection is already modified and the original settings
 *   are lost.
 **/
gboolean
_nm_connection_replace_settings(NMConnection       *connection,
                                GVariant           *new_settings,
                                NMSettingParseFlags parse_flags,
                                GError            **error)
{
    NMSetting                          *settings[_NM_META_SETTING_TYPE_NUM] = {};
    nm_auto(_auto_settings) NMSetting **settings_cleanup                    = settings;
    GVariantIter                        iter;
    const char                         *setting_name;
    GVariant                           *setting_dict;
    gboolean                            changed;
    gboolean                            success;
    guint                               n_settings = 0;
    int                                 i;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(g_variant_is_of_type(new_settings, NM_VARIANT_TYPE_CONNECTION), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    nm_assert(!NM_FLAGS_ANY(parse_flags, ~NM_SETTING_PARSE_FLAGS_ALL));
    nm_assert(!NM_FLAGS_ALL(parse_flags,
                            NM_SETTING_PARSE_FLAGS_STRICT | NM_SETTING_PARSE_FLAGS_BEST_EFFORT));

    if (!NM_FLAGS_HAS(parse_flags, NM_SETTING_PARSE_FLAGS_BEST_EFFORT)
        && !validate_permissions_type(new_settings, error))
        return FALSE;

    g_variant_iter_init(&iter, new_settings);
    while (g_variant_iter_next(&iter, "{&s@a{sv}}", &setting_name, &setting_dict)) {
        gs_unref_variant GVariant *setting_dict_free = NULL;
        GError                    *local             = NULL;
        NMSetting                 *setting;
        const NMMetaSettingInfo   *setting_info;

        setting_dict_free = setting_dict;

        setting_info = nm_meta_setting_infos_by_name(setting_name);
        if (!setting_info) {
            if (NM_FLAGS_HAS(parse_flags, NM_SETTING_PARSE_FLAGS_BEST_EFFORT))
                continue;
            g_set_error_literal(error,
                                NM_CONNECTION_ERROR,
                                NM_CONNECTION_ERROR_INVALID_SETTING,
                                _("unknown setting name"));
            g_prefix_error(error, "%s: ", setting_name);
            return FALSE;
        }

        _nm_assert_setting_info(setting_info, 0);

        if (settings[setting_info->meta_type]) {
            if (NM_FLAGS_HAS(parse_flags, NM_SETTING_PARSE_FLAGS_STRICT)) {
                g_set_error_literal(error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_INVALID_SETTING,
                                    _("duplicate setting name"));
                g_prefix_error(error, "%s: ", setting_name);
                return FALSE;
            }
            /* last wins. We remove the setting of this type, and will
             * add the new one afterwards. */
            g_clear_object(&settings[setting_info->meta_type]);
            break;
        }

        setting = _nm_setting_new_from_dbus(setting_info->get_setting_gtype(),
                                            setting_dict,
                                            new_settings,
                                            parse_flags,
                                            &local);

        if (!setting) {
            if (NM_FLAGS_HAS(parse_flags, NM_SETTING_PARSE_FLAGS_BEST_EFFORT))
                continue;
            g_propagate_error(error, local);
            return FALSE;
        }

        settings[setting_info->meta_type] = setting;
        n_settings++;
    }

    if (_nm_connection_clear_settings(connection, NM_CONNECTION_GET_PRIVATE(connection)))
        changed = TRUE;
    else
        changed = (n_settings > 0);

    /* Note: @settings might be empty in which case the connection
     * has no NMSetting instances... which is fine, just something
     * to be aware of. */
    if (n_settings > 0) {
        for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
            if (settings[i])
                _nm_connection_add_setting(connection, g_steal_pointer(&settings[i]));
        }
        settings_cleanup = NULL;
    }

    /* If verification/normalization fails, the original connection
     * is already lost. From an API point of view, it would be nicer
     * not to touch the input argument if we fail at the end.
     * However, that would require creating a temporary connection
     * to validate it first. As none of the caller cares about the
     * state of the @connection when normalization fails, just do it
     * this way. */
    if (NM_FLAGS_HAS(parse_flags, NM_SETTING_PARSE_FLAGS_NORMALIZE))
        success = nm_connection_normalize(connection, NULL, NULL, error);
    else
        success = TRUE;

    if (changed)
        _signal_emit_changed(connection);
    return success;
}

/**
 * nm_connection_replace_settings:
 * @connection: a #NMConnection
 * @new_settings: a #GVariant of type %NM_VARIANT_TYPE_CONNECTION, with the new settings
 * @error: location to store error, or %NULL
 *
 * Replaces @connection's settings with @new_settings (which must be
 * syntactically valid, and describe a known type of connection, but does not
 * need to result in a connection that passes nm_connection_verify()).
 *
 * Returns: %TRUE if connection was updated, %FALSE if @new_settings could not
 *   be deserialized (in which case @connection will be unchanged).
 **/
gboolean
nm_connection_replace_settings(NMConnection *connection, GVariant *new_settings, GError **error)
{
    return _nm_connection_replace_settings(connection,
                                           new_settings,
                                           NM_SETTING_PARSE_FLAGS_NONE,
                                           error);
}

/**
 * nm_connection_replace_settings_from_connection:
 * @connection: a #NMConnection
 * @new_connection: a #NMConnection to replace the settings of @connection with
 *
 * Deep-copies the settings of @new_connection and replaces the settings of @connection
 * with the copied settings.
 **/
void
nm_connection_replace_settings_from_connection(NMConnection *connection,
                                               NMConnection *new_connection)
{
    NMConnectionPrivate *priv;
    NMConnectionPrivate *new_priv;
    gboolean             changed = FALSE;
    int                  i;

    g_return_if_fail(NM_IS_CONNECTION(connection));
    g_return_if_fail(NM_IS_CONNECTION(new_connection));

    /* When 'connection' and 'new_connection' are the same object simply return
     * in order not to destroy 'connection'.
     */
    if (connection == new_connection)
        return;

    /* No need to validate permissions like nm_connection_replace_settings()
     * since we're dealing with an NMConnection which has already done that.
     */

    priv     = NM_CONNECTION_GET_PRIVATE(connection);
    new_priv = NM_CONNECTION_GET_PRIVATE(new_connection);

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *s_old;

        if (new_priv->settings[i] == priv->settings[i])
            continue;

        changed = TRUE;

        s_old = g_steal_pointer(&priv->settings[i]);

        if (new_priv->settings[i]) {
            priv->settings[i] = nm_setting_duplicate(new_priv->settings[i]);
            _setting_notify_connect(connection, priv->settings[i]);
        }
        if (s_old) {
            _setting_notify_disconnect(connection, s_old);
            g_object_unref(s_old);
        }
    }

    if (changed)
        _signal_emit_changed(connection);
}

/**
 * nm_connection_clear_settings:
 * @connection: a #NMConnection
 *
 * Deletes all of @connection's settings.
 **/
void
nm_connection_clear_settings(NMConnection *connection)
{
    g_return_if_fail(NM_IS_CONNECTION(connection));

    if (_nm_connection_clear_settings(connection, NM_CONNECTION_GET_PRIVATE(connection)))
        _signal_emit_changed(connection);
}

/**
 * nm_connection_compare:
 * @a: a #NMConnection
 * @b: a second #NMConnection to compare with the first
 * @flags: compare flags, e.g. %NM_SETTING_COMPARE_FLAG_EXACT
 *
 * Compares two #NMConnection objects for similarity, with comparison behavior
 * modified by a set of flags.  See nm_setting_compare() for a description of
 * each flag's behavior.
 *
 * Returns: %TRUE if the comparison succeeds, %FALSE if it does not
 **/
gboolean
nm_connection_compare(NMConnection *a, NMConnection *b, NMSettingCompareFlags flags)
{
    NMConnectionPrivate *a_priv;
    NMConnectionPrivate *b_priv;
    int                  i;

    if (a == b)
        return TRUE;
    if (!a || !b)
        return FALSE;

    a_priv = NM_CONNECTION_GET_PRIVATE(a);
    b_priv = NM_CONNECTION_GET_PRIVATE(b);

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        if (a_priv->settings[i] == b_priv->settings[i])
            continue;

        if (!a_priv->settings[i] || !b_priv->settings[i])
            return FALSE;

        if (!_nm_setting_compare(a, a_priv->settings[i], b, b_priv->settings[i], flags))
            return FALSE;
    }
    return TRUE;
}

static gboolean
diff_one_connection(NMConnection         *a,
                    NMConnection         *b,
                    NMSettingCompareFlags flags,
                    gboolean              invert_results,
                    GHashTable           *diffs)
{
    NMConnectionPrivate *a_priv     = NM_CONNECTION_GET_PRIVATE(a);
    NMConnectionPrivate *b_priv     = b ? NM_CONNECTION_GET_PRIVATE(b) : NULL;
    gboolean             diff_found = FALSE;
    int                  i;

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *a_setting = a_priv->settings[i];

        if (a_setting) {
            NMSetting  *b_setting    = b ? b_priv->settings[i] : NULL;
            const char *setting_name = nm_setting_get_name(a_setting);
            GHashTable *results;
            gboolean    new_results = TRUE;

            results = g_hash_table_lookup(diffs, setting_name);
            if (results)
                new_results = FALSE;

            if (!_nm_setting_diff(a, a_setting, b, b_setting, flags, invert_results, &results))
                diff_found = TRUE;

            if (new_results && results)
                g_hash_table_insert(diffs, g_strdup(setting_name), results);
        }
    }

    return diff_found;
}

/**
 * nm_connection_diff:
 * @a: a #NMConnection
 * @b: a second #NMConnection to compare with the first
 * @flags: compare flags, e.g. %NM_SETTING_COMPARE_FLAG_EXACT
 * @out_settings: (out) (element-type utf8 GLib.HashTable(utf8,uint32)) (optional) (nullable):
 * if the connections differ, on return a hash table
 * mapping setting names to second-level GHashTable (utf8 to guint32), which
 * contains the key names that differ mapped to one or more of
 * %NMSettingDiffResult as a bitfield
 *
 * Compares two #NMConnection objects for similarity, with comparison behavior
 * modified by a set of flags.  See nm_setting_compare() for a description of
 * each flag's behavior.  If the connections differ, settings and keys within
 * each setting that differ are added to the returned @out_settings hash table.
 * No values are returned, only key names.
 *
 * Returns: %TRUE if the connections contain the same values, %FALSE if they do
 * not
 **/
gboolean
nm_connection_diff(NMConnection         *a,
                   NMConnection         *b,
                   NMSettingCompareFlags flags,
                   GHashTable          **out_settings)
{
    GHashTable *diffs;
    gboolean    diff_found = FALSE;

    g_return_val_if_fail(NM_IS_CONNECTION(a), FALSE);
    g_return_val_if_fail(!out_settings || !*out_settings, FALSE);
    g_return_val_if_fail(!b || NM_IS_CONNECTION(b), FALSE);

    if (a == b)
        return TRUE;

    diffs = g_hash_table_new_full(nm_str_hash,
                                  g_str_equal,
                                  g_free,
                                  (GDestroyNotify) g_hash_table_destroy);

    /* Diff A to B, then B to A to capture keys in B that aren't in A */
    if (diff_one_connection(a, b, flags, FALSE, diffs))
        diff_found = TRUE;
    if (b && diff_one_connection(b, a, flags, TRUE, diffs))
        diff_found = TRUE;

    nm_assert(diff_found == (g_hash_table_size(diffs) != 0));

    if (g_hash_table_size(diffs) == 0) {
        g_hash_table_destroy(diffs);
        diffs = NULL;
    }

    NM_SET_OUT(out_settings, diffs);

    return !diff_found;
}

NMSetting *
_nm_connection_find_base_type_setting(NMConnection *connection)
{
    NMConnectionPrivate *priv         = NM_CONNECTION_GET_PRIVATE(connection);
    NMSetting           *setting      = NULL;
    NMSettingPriority    setting_prio = NM_SETTING_PRIORITY_USER;
    int                  i;

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting        *s_iter = priv->settings[i];
        NMSettingPriority s_iter_prio;

        if (!s_iter)
            continue;

        s_iter_prio = _nm_setting_get_base_type_priority(s_iter);
        if (s_iter_prio == NM_SETTING_PRIORITY_INVALID)
            continue;

        if (setting) {
            if (s_iter_prio > setting_prio) {
                continue;
            } else if (s_iter_prio == setting_prio) {
                NMSettingConnection *s_con = nm_connection_get_setting_connection(connection);
                const char          *type;

                if (s_con) {
                    type = nm_setting_connection_get_connection_type(s_con);
                    if (type)
                        return nm_connection_get_setting_by_name(connection, type);
                }
                return NULL;
            }
        }
        setting      = s_iter;
        setting_prio = s_iter_prio;
    }
    return setting;
}

static gboolean
_normalize_connection_uuid(NMConnection *self)
{
    NMSettingConnection *s_con = nm_connection_get_setting_connection(self);
    char                 uuid_normalized[37];
    const char          *uuid;

    nm_assert(s_con);

    uuid = nm_setting_connection_get_uuid(s_con);

    if (uuid) {
        gboolean uuid_is_normalized;

        if (!nm_uuid_is_valid_nm(uuid, &uuid_is_normalized, uuid_normalized))
            return nm_assert_unreachable_val(FALSE);
        if (!uuid_is_normalized)
            return FALSE;
    } else
        nm_uuid_generate_random_str_arr(uuid_normalized);

    g_object_set(s_con, NM_SETTING_CONNECTION_UUID, uuid_normalized, NULL);
    return TRUE;
}

static gboolean
_normalize_connection(NMConnection *self)
{
    NMSettingConnection *s_con   = nm_connection_get_setting_connection(self);
    gboolean             changed = FALSE;

    if (nm_setting_connection_get_read_only(s_con)) {
        g_object_set(s_con, NM_SETTING_CONNECTION_READ_ONLY, FALSE, NULL);
        changed = TRUE;
    }

    return changed;
}

gboolean
_nm_setting_connection_verify_secondaries(GArray *secondaries, GError **error)
{
    const char *const *strv;
    const guint        len              = nm_g_array_len(secondaries);
    guint              has_normalizable = FALSE;
    gboolean           has_invalid      = FALSE;
    gboolean           has_duplicate    = FALSE;
    guint              i;

    if (len == 0)
        return TRUE;

    /* For historic reasons, the secondaries were not normalized/validated.
     *
     * Now, when we find any invalid/non-normalized values, we reject/normalize
     * them. We also filter out duplicates. */

    strv = nm_strvarray_get_strv_notempty(secondaries, NULL);

    for (i = 0; i < len; i++) {
        const char *uuid = strv[i];
        gboolean    normalized;

        if (!nm_uuid_is_valid_nm(uuid, &normalized, NULL)) {
            has_invalid = TRUE;
            goto out;
        }
        if (normalized)
            has_normalizable = TRUE;
    }
    if (has_normalizable)
        goto out;

    if (len > 1) {
        gs_free const char **strv_to_free = NULL;
        const char         **strv2;

        strv2 = nm_strv_dup_shallow_maybe_a(20, strv, len, &strv_to_free);
        nm_strv_sort(strv2, len);
        has_duplicate = nm_strv_has_duplicate(strv2, len, TRUE);
    }

out:
    if (has_invalid) {
        g_set_error_literal(error,
                            NM_CONNECTION_ERROR,
                            NM_CONNECTION_ERROR_INVALID_PROPERTY,
                            _("has an invalid UUID"));
    } else if (has_normalizable) {
        g_set_error_literal(error,
                            NM_CONNECTION_ERROR,
                            NM_CONNECTION_ERROR_INVALID_PROPERTY,
                            _("has a UUID that requires normalization"));
    } else if (has_duplicate) {
        g_set_error_literal(error,
                            NM_CONNECTION_ERROR,
                            NM_CONNECTION_ERROR_INVALID_PROPERTY,
                            _("has duplicate UUIDs"));
    } else
        return TRUE;

    g_prefix_error(error,
                   "%s.%s: ",
                   NM_SETTING_CONNECTION_SETTING_NAME,
                   NM_SETTING_CONNECTION_SECONDARIES);
    return FALSE;
}

gboolean
_nm_setting_connection_verify_no_duplicate_addresses(GArray *addresses, GError **error)
{
    guint i, j;

    if (addresses->len <= 1) {
        return TRUE;
    } else {
        for (i = 0; i < addresses->len - 1; i++) {
            for (j = i + 1; j < addresses->len; j++) {
                if (nm_streq0(nm_g_array_index(addresses, const char *, i),
                              nm_g_array_index(addresses, const char *, j)))
                    return FALSE;
            }
        }
    }

    return TRUE;
}

int
_get_ip_address_family(const char *ip_address)
{
    struct in_addr  ipv4_addr;
    struct in6_addr ipv6_addr;

    if (inet_pton(AF_INET, ip_address, &ipv4_addr))
        return AF_INET;
    else if (inet_pton(AF_INET6, ip_address, &ipv6_addr))
        return AF_INET6;
    else
        return -1;
}

static gboolean
_normalize_connection_secondaries(NMConnection *self)
{
    NMSettingConnection *s_con = nm_connection_get_setting_connection(self);
    GArray              *secondaries;
    gs_strfreev char   **strv = NULL;
    guint                i;
    guint                j;

    nm_assert(s_con);

    secondaries = _nm_setting_connection_get_secondaries(s_con);
    if (nm_g_array_len(secondaries) == 0)
        return FALSE;

    if (_nm_setting_connection_verify_secondaries(secondaries, NULL))
        return FALSE;

    strv = nm_strvarray_get_strv_notempty_dup(secondaries, NULL);
    for (i = 0, j = 0; strv[i]; i++) {
        gs_free char *s = g_steal_pointer(&strv[i]);
        char          uuid_normalized[37];
        gboolean      uuid_is_normalized;

        if (!nm_uuid_is_valid_nm(s, &uuid_is_normalized, uuid_normalized))
            continue;

        if (nm_strv_contains(strv, j, uuid_is_normalized ? uuid_normalized : s))
            continue;

        strv[j++] = uuid_is_normalized ? g_strdup(uuid_normalized) : g_steal_pointer(&s);
    }
    strv[j] = NULL;

    g_object_set(s_con, NM_SETTING_CONNECTION_SECONDARIES, strv, NULL);
    return TRUE;
}

static gboolean
_normalize_connection_ip_ping_addresses(NMConnection *self)
{
    NMSettingConnection *s_con = nm_connection_get_setting_connection(self);
    GArray              *addresses;
    gs_strfreev char   **strv = NULL;
    guint                i, j, k;

    nm_assert(s_con);

    addresses = _nm_setting_connection_get_ip_ping_addresses(s_con);
    if (nm_g_array_len(addresses) == 0)
        return FALSE;

    if (_nm_setting_connection_verify_no_duplicate_addresses(addresses, NULL))
        return FALSE;

    strv = nm_strvarray_get_strv_notempty_dup(addresses, NULL);

    for (i = 0, j = 0; strv[i]; i++) {
        gboolean found = FALSE;

        for (k = 0; k < j; k++) {
            if (nm_streq0(strv[i], strv[k])) {
                found = TRUE;
                break;
            }
        }

        if (found) {
            continue;
        }

        strv[j++] = strv[i];
    }
    strv[j] = NULL;

    g_object_set(s_con, NM_SETTING_CONNECTION_IP_PING_ADDRESSES, strv, NULL);

    return TRUE;
}

static gboolean
_normalize_connection_type(NMConnection *self)
{
    NMSettingConnection *s_con  = nm_connection_get_setting_connection(self);
    NMSetting           *s_base = NULL;
    const char          *type;

    type = nm_setting_connection_get_connection_type(s_con);

    if (type) {
        s_base = nm_connection_get_setting_by_name(self, type);

        if (!s_base) {
            GType base_type = nm_setting_lookup_type(type);

            g_return_val_if_fail(base_type, FALSE);
            nm_connection_add_setting(self, g_object_new(base_type, NULL));
            return TRUE;
        }
    } else {
        s_base = _nm_connection_find_base_type_setting(self);
        g_return_val_if_fail(s_base, FALSE);

        type = nm_setting_get_name(s_base);
        g_object_set(s_con, NM_SETTING_CONNECTION_TYPE, type, NULL);
        return TRUE;
    }

    return FALSE;
}

const char *
_nm_connection_detect_bluetooth_type(NMConnection *self)
{
    NMSettingBluetooth *s_bt = nm_connection_get_setting_bluetooth(self);

    if (s_bt && nm_setting_bluetooth_get_connection_type(s_bt)) {
        if (nm_connection_get_setting_gsm(self) || nm_connection_get_setting_cdma(self))
            return NM_SETTING_BLUETOOTH_TYPE_DUN;
        if (nm_connection_get_setting_bridge(self))
            return NM_SETTING_BLUETOOTH_TYPE_NAP;
        return NM_SETTING_BLUETOOTH_TYPE_PANU;
    }

    /* NULL means the connection is not a bluetooth type, or it needs
     * no normalization, as the type is set explicitly. */
    return NULL;
}

const char *
_nm_connection_detect_port_type(NMConnection *connection, NMSetting **out_s_port)
{
    NMConnectionPrivate *priv      = NM_CONNECTION_GET_PRIVATE(connection);
    const char          *port_type = NULL;
    NMSetting           *s_port    = NULL;
    int                  i;
    static const struct {
        NMMetaSettingType meta_type;
        const char       *controller_type_name;
    } infos[] = {
        {NM_META_SETTING_TYPE_BRIDGE_PORT, NM_SETTING_BRIDGE_SETTING_NAME},
        {NM_META_SETTING_TYPE_BOND_PORT, NM_SETTING_BOND_SETTING_NAME},
        {NM_META_SETTING_TYPE_TEAM_PORT, NM_SETTING_TEAM_SETTING_NAME},
        {NM_META_SETTING_TYPE_OVS_PORT, NM_SETTING_OVS_BRIDGE_SETTING_NAME},
        {NM_META_SETTING_TYPE_OVS_INTERFACE, NM_SETTING_OVS_PORT_SETTING_NAME},
    };

    for (i = 0; i < (int) G_N_ELEMENTS(infos); i++) {
        NMSetting *setting = priv->settings[infos[i].meta_type];

        if (!setting)
            continue;

        if (port_type) {
            /* there are more then one matching port types, cannot detect the port type. */
            port_type = NULL;
            s_port    = NULL;
            break;
        }
        port_type = infos[i].controller_type_name;
        s_port    = setting;
    }

    if (out_s_port)
        *out_s_port = s_port;
    return port_type;
}

static gboolean
_normalize_connection_port_type(NMConnection *self)
{
    NMSettingConnection *s_con = nm_connection_get_setting_connection(self);
    const char          *port_type, *port_setting_type;

    if (!s_con)
        return FALSE;
    if (!nm_setting_connection_get_controller(s_con))
        return FALSE;

    port_type = nm_setting_connection_get_port_type(s_con);
    if (port_type) {
        if (_nm_setting_port_type_is_valid(port_type, &port_setting_type) && port_setting_type) {
            NMSetting *s_port;

            s_port = nm_connection_get_setting_by_name(self, port_setting_type);
            if (!s_port) {
                GType p_type = nm_setting_lookup_type(port_setting_type);

                g_return_val_if_fail(p_type, FALSE);
                nm_connection_add_setting(self, g_object_new(p_type, NULL));
                return TRUE;
            }
        }
    } else {
        if ((port_type = _nm_connection_detect_port_type(self, NULL))) {
            g_object_set(s_con, NM_SETTING_CONNECTION_PORT_TYPE, port_type, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_normalize_ethernet_link_neg(NMConnection *self)
{
    NMSettingWired *s_wired = nm_connection_get_setting_wired(self);

    if (s_wired) {
        guint32     speed  = nm_setting_wired_get_speed(s_wired);
        const char *duplex = nm_setting_wired_get_duplex(s_wired);

        if ((speed && !duplex) || (!speed && duplex)) {
            speed  = 0;
            duplex = NULL;
            g_object_set(s_wired,
                         NM_SETTING_WIRED_SPEED,
                         (guint) speed,
                         NM_SETTING_WIRED_DUPLEX,
                         duplex,
                         NULL);
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * _supports_addr_family:
 * @self: a #NMConnection
 * @family: AF_*
 *
 * Check whether the connection supports certain L3 address family,
 * in order to be able to tell whether is should have the corresponding
 * setting ("ipv4" for AF_INET and "ipv6" for AF_INET6).
 *
 * If AF_UNSPEC is given, then the function checks whether the connection
 * supports any L3 configuration at all.
 *
 * Returns: %TRUE if the AF is supported, %FALSE otherwise
 **/
static gboolean
_supports_addr_family(NMConnection *self, int family)
{
    const char          *connection_type = nm_connection_get_connection_type(self);
    NMSettingConnection *s_con;

    g_return_val_if_fail(connection_type, TRUE);
    if (strcmp(connection_type, NM_SETTING_OVS_INTERFACE_SETTING_NAME) == 0)
        return TRUE;
    if (strcmp(connection_type, NM_SETTING_WPAN_SETTING_NAME) == 0)
        return FALSE;
    if (strcmp(connection_type, NM_SETTING_6LOWPAN_SETTING_NAME) == 0)
        return family == AF_INET6 || family == AF_UNSPEC;
    if ((s_con = nm_connection_get_setting_connection(self))
        && (nm_streq0(nm_setting_connection_get_port_type(s_con), NM_SETTING_VRF_SETTING_NAME)))
        return TRUE;

    return !nm_setting_connection_get_controller(nm_connection_get_setting_connection(self));
}

static gboolean
_normalize_ip_config(NMConnection *self, GHashTable *parameters)
{
    NMSettingIPConfig *s_ip4, *s_ip6;
    NMSettingProxy    *s_proxy;
    NMSetting         *setting;
    gboolean           changed = FALSE;
    guint              num, i;
    int                dhcp_send_hostname_v2;

    s_ip4   = nm_connection_get_setting_ip4_config(self);
    s_ip6   = nm_connection_get_setting_ip6_config(self);
    s_proxy = nm_connection_get_setting_proxy(self);

    if (_supports_addr_family(self, AF_INET)) {
        if (!s_ip4) {
            const char *default_ip4_method = NULL;

            if (parameters) {
                default_ip4_method =
                    g_hash_table_lookup(parameters,
                                        NM_CONNECTION_NORMALIZE_PARAM_IP4_CONFIG_METHOD);
            }
            if (!default_ip4_method) {
                const char *type = nm_connection_get_connection_type(self);

                if (NM_IN_STRSET(type,
                                 NM_SETTING_WIREGUARD_SETTING_NAME,
                                 NM_SETTING_DUMMY_SETTING_NAME))
                    default_ip4_method = NM_SETTING_IP4_CONFIG_METHOD_DISABLED;
                else
                    default_ip4_method = NM_SETTING_IP4_CONFIG_METHOD_AUTO;
            }

            /* But if no IP4 setting was specified, assume the caller was just
             * being lazy and use the default method.
             */
            setting = nm_setting_ip4_config_new();

            g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, default_ip4_method, NULL);
            nm_connection_add_setting(self, setting);
            changed = TRUE;
        } else {
            if (nm_setting_ip_config_get_gateway(s_ip4)
                && nm_setting_ip_config_get_never_default(s_ip4)) {
                g_object_set(s_ip4, NM_SETTING_IP_CONFIG_GATEWAY, NULL, NULL);
                changed = TRUE;
            }

            if (nm_streq0(nm_setting_ip_config_get_method(s_ip4),
                          NM_SETTING_IP4_CONFIG_METHOD_DISABLED)
                && !nm_setting_ip_config_get_may_fail(s_ip4)) {
                g_object_set(s_ip4, NM_SETTING_IP_CONFIG_MAY_FAIL, TRUE, NULL);
                changed = TRUE;
            }

            num = nm_setting_ip_config_get_num_addresses(s_ip4);
            if (num > 1
                && nm_streq0(nm_setting_ip_config_get_method(s_ip4),
                             NM_SETTING_IP4_CONFIG_METHOD_SHARED)) {
                for (i = num - 1; i > 0; i--)
                    nm_setting_ip_config_remove_address(s_ip4, i);
                changed = TRUE;
            }

            dhcp_send_hostname_v2 = nm_setting_ip_config_get_dhcp_send_hostname_v2(s_ip4);
            if (dhcp_send_hostname_v2 != NM_TERNARY_DEFAULT
                && dhcp_send_hostname_v2 != nm_setting_ip_config_get_dhcp_send_hostname(s_ip4)) {
                g_object_set(s_ip4,
                             NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME,
                             dhcp_send_hostname_v2,
                             NULL);
                changed = TRUE;
            }
        }
    } else {
        if (s_ip4) {
            nm_connection_remove_setting(self, NM_TYPE_SETTING_IP4_CONFIG);
            changed = TRUE;
        }
    }

    if (_supports_addr_family(self, AF_INET6)) {
        if (!s_ip6) {
            const char *default_ip6_method = NULL;

            if (parameters) {
                default_ip6_method =
                    g_hash_table_lookup(parameters,
                                        NM_CONNECTION_NORMALIZE_PARAM_IP6_CONFIG_METHOD);
            }
            if (!default_ip6_method) {
                const char *type = nm_connection_get_connection_type(self);

                if (NM_IN_STRSET(type,
                                 NM_SETTING_WIREGUARD_SETTING_NAME,
                                 NM_SETTING_DUMMY_SETTING_NAME))
                    default_ip6_method = NM_SETTING_IP6_CONFIG_METHOD_DISABLED;
                else
                    default_ip6_method = NM_SETTING_IP6_CONFIG_METHOD_AUTO;
            }

            /* If no IP6 setting was specified, then assume that means IP6 config is
             * allowed to fail.
             */
            setting = nm_setting_ip6_config_new();

            g_object_set(setting,
                         NM_SETTING_IP_CONFIG_METHOD,
                         default_ip6_method,
                         NM_SETTING_IP_CONFIG_MAY_FAIL,
                         TRUE,
                         NULL);
            nm_connection_add_setting(self, setting);
            changed = TRUE;
        } else {
            const char *token;

            token = nm_setting_ip6_config_get_token((NMSettingIP6Config *) s_ip6);
            if (token
                && nm_setting_ip6_config_get_addr_gen_mode((NMSettingIP6Config *) s_ip6)
                       == NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64) {
                struct in6_addr i6_token;
                char            normalized[NM_INET_ADDRSTRLEN];

                if (inet_pton(AF_INET6, token, &i6_token) == 1
                    && _nm_utils_inet6_is_token(&i6_token)) {
                    nm_inet6_ntop(&i6_token, normalized);
                    if (g_strcmp0(token, normalized)) {
                        g_object_set(s_ip6, NM_SETTING_IP6_CONFIG_TOKEN, normalized, NULL);
                        changed = TRUE;
                    }
                }
            }

            if (nm_setting_ip_config_get_gateway(s_ip6)
                && nm_setting_ip_config_get_never_default(s_ip6)) {
                g_object_set(s_ip6, NM_SETTING_IP_CONFIG_GATEWAY, NULL, NULL);
                changed = TRUE;
            }

            if (NM_IN_STRSET(nm_setting_ip_config_get_method(s_ip6),
                             NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
                             NM_SETTING_IP6_CONFIG_METHOD_DISABLED)
                && !nm_setting_ip_config_get_may_fail(s_ip6)) {
                g_object_set(s_ip6, NM_SETTING_IP_CONFIG_MAY_FAIL, TRUE, NULL);
                changed = TRUE;
            }

            dhcp_send_hostname_v2 = nm_setting_ip_config_get_dhcp_send_hostname_v2(s_ip6);
            if (dhcp_send_hostname_v2 != NM_TERNARY_DEFAULT
                && dhcp_send_hostname_v2 != nm_setting_ip_config_get_dhcp_send_hostname(s_ip6)) {
                g_object_set(s_ip6,
                             NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME,
                             dhcp_send_hostname_v2,
                             NULL);
                changed = TRUE;
            }
        }
    } else {
        if (s_ip6) {
            nm_connection_remove_setting(self, NM_TYPE_SETTING_IP6_CONFIG);
            changed = TRUE;
        }
    }

    if (_supports_addr_family(self, AF_UNSPEC)) {
        if (!s_proxy) {
            setting = nm_setting_proxy_new();
            nm_connection_add_setting(self, setting);
            changed = TRUE;
        }
    } else {
        if (s_proxy) {
            nm_connection_remove_setting(self, NM_TYPE_SETTING_PROXY);
            changed = TRUE;
        }
    }

    return changed;
}

static gboolean
_normalize_infiniband(NMConnection *self)
{
    NMSettingInfiniband *s_infini = nm_connection_get_setting_infiniband(self);
    gboolean             changed  = FALSE;
    const char          *interface_name;
    int                  p_key;

    if (!s_infini)
        return FALSE;

    if (nm_setting_infiniband_get_mtu(s_infini) > NM_INFINIBAND_MAX_MTU) {
        if (NM_IN_STRSET(nm_setting_infiniband_get_transport_mode(s_infini),
                         "datagram",
                         "connected")) {
            g_object_set(s_infini, NM_SETTING_INFINIBAND_MTU, (guint) NM_INFINIBAND_MAX_MTU, NULL);
            changed = TRUE;
        }
    }

    if ((p_key = nm_setting_infiniband_get_p_key(s_infini)) != -1
        && (interface_name = nm_connection_get_interface_name(self))) {
        const char *virtual_iface_name;

        virtual_iface_name = nm_setting_infiniband_get_virtual_interface_name(s_infini);

        if (!nm_streq0(interface_name, virtual_iface_name)) {
            g_object_set(nm_connection_get_setting_connection(self),
                         NM_SETTING_CONNECTION_INTERFACE_NAME,
                         virtual_iface_name,
                         NULL);
            changed = TRUE;
        }
    }

    return changed;
}

static gboolean
_normalize_bond_mode(NMConnection *self)
{
    NMSettingBond *s_bond = nm_connection_get_setting_bond(self);

    /* Convert mode from numeric to string notation */
    if (s_bond) {
        const char *mode = nm_setting_bond_get_option_by_name(s_bond, NM_SETTING_BOND_OPTION_MODE);
        int         mode_int = nm_utils_bond_mode_string_to_int(mode);

        if (mode_int != -1) {
            const char *mode_new = nm_utils_bond_mode_int_to_string(mode_int);

            if (!nm_streq0(mode_new, mode)) {
                nm_setting_bond_add_option(s_bond, NM_SETTING_BOND_OPTION_MODE, mode_new);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean
_normalize_bond_options(NMConnection *self)
{
    NMSettingBond *s_bond  = nm_connection_get_setting_bond(self);
    gboolean       changed = FALSE;
    const char    *name, *mode_str;
    NMBondMode     mode;
    guint32        num, i;

    /* Strip away unsupported options for current mode */
    if (s_bond) {
        mode_str = nm_setting_bond_get_option_by_name(s_bond, NM_SETTING_BOND_OPTION_MODE);
        mode     = _nm_setting_bond_mode_from_string(mode_str);
        if (mode == NM_BOND_MODE_UNKNOWN)
            return FALSE;
again:
        num = nm_setting_bond_get_num_options(s_bond);
        for (i = 0; i < num; i++) {
            if (nm_setting_bond_get_option(s_bond, i, &name, NULL)
                && !_nm_setting_bond_option_supported(name, mode)) {
                nm_setting_bond_remove_option(s_bond, name);
                changed = TRUE;
                goto again;
            }
        }
    }

    return changed;
}

static gboolean
_normalize_wireless_mac_address_randomization(NMSettingWireless *s_wifi)
{
    const char               *desired_cloned_mac_address;
    const char               *cloned_mac_address;
    NMSettingMacRandomization desired_mac_address_randomization;
    NMSettingMacRandomization mac_address_randomization;
    gboolean                  changed = FALSE;

    _nm_setting_wireless_normalize_mac_address_randomization(s_wifi,
                                                             &desired_cloned_mac_address,
                                                             &desired_mac_address_randomization);

    mac_address_randomization = nm_setting_wireless_get_mac_address_randomization(s_wifi);
    cloned_mac_address        = nm_setting_wireless_get_cloned_mac_address(s_wifi);

    /* Note that "mac_address_randomization" is possibly the string owned by
     * "s_wifi".  We must be careful that modifying "s_wifi" might invalidate
     * the string. */

    if (!nm_streq0(cloned_mac_address, desired_cloned_mac_address)) {
        g_object_set(s_wifi,
                     NM_SETTING_WIRELESS_CLONED_MAC_ADDRESS,
                     desired_cloned_mac_address,
                     NULL);
        changed = TRUE;
    }

    if (mac_address_randomization != desired_mac_address_randomization) {
        g_object_set(s_wifi,
                     NM_SETTING_WIRELESS_MAC_ADDRESS_RANDOMIZATION,
                     (guint) desired_mac_address_randomization,
                     NULL);
        changed = TRUE;
    }

    return changed;
}

static gboolean
_normalize_wireless(NMConnection *self)
{
    NMSettingWireless *s_wifi  = nm_connection_get_setting_wireless(self);
    gboolean           changed = FALSE;

    if (!s_wifi)
        return FALSE;

    if (nm_setting_wireless_get_rate(s_wifi) != 0) {
        g_object_set(s_wifi, NM_SETTING_WIRELESS_RATE, 0u, NULL);
        changed = TRUE;
    }

    if (nm_setting_wireless_get_tx_power(s_wifi) != 0) {
        g_object_set(s_wifi, NM_SETTING_WIRELESS_TX_POWER, 0u, NULL);
        changed = TRUE;
    }

    if (_normalize_wireless_mac_address_randomization(s_wifi))
        changed = TRUE;

    return changed;
}

static gboolean
_normalize_macsec(NMConnection *self)
{
    NMSettingMacsec *s_macsec = nm_connection_get_setting_macsec(self);
    gboolean         changed  = FALSE;

    if (!s_macsec)
        return FALSE;

    if (nm_setting_macsec_get_mode(s_macsec) != NM_SETTING_MACSEC_MODE_PSK) {
        if (nm_setting_macsec_get_mka_cak(s_macsec)) {
            g_object_set(s_macsec, NM_SETTING_MACSEC_MKA_CAK, NULL, NULL);
            changed = TRUE;
        }
        if (nm_setting_macsec_get_mka_ckn(s_macsec)) {
            g_object_set(s_macsec, NM_SETTING_MACSEC_MKA_CKN, NULL, NULL);
            changed = TRUE;
        }
    }

    return changed;
}

static gboolean
_normalize_team_config(NMConnection *self)
{
    NMSettingTeam *s_team = nm_connection_get_setting_team(self);

    if (s_team) {
        const char *config = nm_setting_team_get_config(s_team);

        if (config && !*config) {
            g_object_set(s_team, NM_SETTING_TEAM_CONFIG, NULL, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_normalize_team_port_config(NMConnection *self)
{
    NMSettingTeamPort *s_team_port = nm_connection_get_setting_team_port(self);

    if (s_team_port) {
        const char *config = nm_setting_team_port_get_config(s_team_port);

        if (config && !*config) {
            g_object_set(s_team_port, NM_SETTING_TEAM_PORT_CONFIG, NULL, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_normalize_bluetooth_type(NMConnection *self)
{
    const char *type = _nm_connection_detect_bluetooth_type(self);

    if (type) {
        g_object_set(nm_connection_get_setting_bluetooth(self),
                     NM_SETTING_BLUETOOTH_TYPE,
                     type,
                     NULL);
        return TRUE;
    }
    return FALSE;
}

static gboolean
_normalize_ovs_interface_type(NMConnection *self)
{
    NMSettingOvsInterface *s_ovs_interface = nm_connection_get_setting_ovs_interface(self);
    gboolean               modified;
    int                    v;

    if (!s_ovs_interface)
        return FALSE;

    v = _nm_setting_ovs_interface_verify_interface_type(
        s_ovs_interface,
        nm_setting_ovs_interface_get_interface_type(s_ovs_interface),
        self,
        TRUE,
        &modified,
        NULL,
        NULL);
    if (v != TRUE)
        g_return_val_if_reached(modified);

    return modified;
}

static gboolean
_normalize_ip_tunnel_wired_setting(NMConnection *self)
{
    NMSettingIPTunnel *s_ip_tunnel;

    s_ip_tunnel = nm_connection_get_setting_ip_tunnel(self);
    if (!s_ip_tunnel)
        return FALSE;

    if (nm_connection_get_setting_wired(self)
        && !NM_IN_SET(nm_setting_ip_tunnel_get_mode(s_ip_tunnel),
                      NM_IP_TUNNEL_MODE_GRETAP,
                      NM_IP_TUNNEL_MODE_IP6GRETAP)) {
        nm_connection_remove_setting(self, NM_TYPE_SETTING_WIRED);
        return TRUE;
    }

    return FALSE;
}

static gboolean
_normalize_sriov_vf_order(NMConnection *self)
{
    NMSettingSriov *s_sriov;

    s_sriov = NM_SETTING_SRIOV(nm_connection_get_setting(self, NM_TYPE_SETTING_SRIOV));
    if (!s_sriov)
        return FALSE;

    return _nm_setting_sriov_sort_vfs(s_sriov);
}

static gboolean
_normalize_bridge_vlan_order(NMConnection *self)
{
    NMSettingBridge *s_bridge;

    s_bridge = nm_connection_get_setting_bridge(self);
    if (!s_bridge)
        return FALSE;

    return _nm_setting_bridge_sort_vlans(s_bridge);
}

static gboolean
_normalize_bridge_port_vlan_order(NMConnection *self)
{
    NMSettingBridgePort *s_port;

    s_port = nm_connection_get_setting_bridge_port(self);
    if (!s_port)
        return FALSE;

    return _nm_setting_bridge_port_sort_vlans(s_port);
}

static gboolean
_normalize_ovs_port_trunks(NMConnection *self)
{
    NMSettingOvsPort *s_ovs_port;

    s_ovs_port = nm_connection_get_setting_ovs_port(self);
    if (!s_ovs_port)
        return FALSE;

    return _nm_setting_ovs_port_sort_trunks(s_ovs_port);
}

static gboolean
_normalize_gsm_auto_config(NMConnection *self)
{
    NMSettingGsm *s_gsm;

    s_gsm = nm_connection_get_setting_gsm(self);
    if (!s_gsm)
        return FALSE;

    if (!nm_setting_gsm_get_auto_config(s_gsm))
        return FALSE;

    if (!nm_setting_gsm_get_apn(s_gsm) && !nm_setting_gsm_get_username(s_gsm)
        && !nm_setting_gsm_get_password(s_gsm))
        return FALSE;

    g_object_set(s_gsm, NM_SETTING_GSM_AUTO_CONFIG, FALSE, NULL);
    return TRUE;
}

static gboolean
_normalize_802_1x_empty_strings(NMConnection *self)
{
    NMSetting8021x *s_8021x;
    gboolean        changed = FALSE;

    s_8021x =
        _get_setting_by_metatype(NM_CONNECTION_GET_PRIVATE(self), NM_META_SETTING_TYPE_802_1X);
    if (!s_8021x)
        return FALSE;

#define _norm_8021x(s_8021x, getter, prop_name, p_changed)       \
    G_STMT_START                                                 \
    {                                                            \
        NMSetting8021x *_s_8021x   = (s_8021x);                  \
        gboolean       *_p_changed = (p_changed);                \
        const char     *_v;                                      \
                                                                 \
        _v = getter(_s_8021x);                                   \
        if (_v && _v[0] == '\0') {                               \
            g_object_set(_s_8021x, "" prop_name "", NULL, NULL); \
            *(_p_changed) = TRUE;                                \
        }                                                        \
    }                                                            \
    G_STMT_END

    _norm_8021x(s_8021x, nm_setting_802_1x_get_identity, NM_SETTING_802_1X_IDENTITY, &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_anonymous_identity,
                NM_SETTING_802_1X_ANONYMOUS_IDENTITY,
                &changed);
    _norm_8021x(s_8021x, nm_setting_802_1x_get_pac_file, NM_SETTING_802_1X_PAC_FILE, &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_subject_match,
                NM_SETTING_802_1X_SUBJECT_MATCH,
                &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_phase2_subject_match,
                NM_SETTING_802_1X_PHASE2_SUBJECT_MATCH,
                &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_domain_suffix_match,
                NM_SETTING_802_1X_DOMAIN_SUFFIX_MATCH,
                &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_phase2_domain_suffix_match,
                NM_SETTING_802_1X_PHASE2_DOMAIN_SUFFIX_MATCH,
                &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_domain_match,
                NM_SETTING_802_1X_DOMAIN_MATCH,
                &changed);
    _norm_8021x(s_8021x,
                nm_setting_802_1x_get_phase2_domain_match,
                NM_SETTING_802_1X_PHASE2_DOMAIN_MATCH,
                &changed);

    return changed;
}

static gboolean
_normalize_required_settings(NMConnection *self)
{
    NMSettingBluetooth *s_bt = nm_connection_get_setting_bluetooth(self);
    NMSetting          *s_bridge;
    gboolean            changed = FALSE;

    if (nm_connection_get_setting_vlan(self) || nm_connection_get_setting_bridge(self)) {
        if (!nm_connection_get_setting_wired(self)) {
            nm_connection_add_setting(self, nm_setting_wired_new());
            changed = TRUE;
        }
    }
    if (s_bt
        && nm_streq0(nm_setting_bluetooth_get_connection_type(s_bt),
                     NM_SETTING_BLUETOOTH_TYPE_NAP)) {
        if (!nm_connection_get_setting_bridge(self)) {
            s_bridge = nm_setting_bridge_new();
            g_object_set(s_bridge, NM_SETTING_BRIDGE_STP, FALSE, NULL);
            nm_connection_add_setting(self, s_bridge);
            changed = TRUE;
        }
    }
    return changed;
}

static gboolean
_normalize_invalid_port_port_settings(NMConnection *self)
{
    NMSettingConnection *s_con = nm_connection_get_setting_connection(self);
    const char          *port_type;
    gboolean             changed = FALSE;

    port_type = nm_setting_connection_get_port_type(s_con);

    if (!nm_streq0(port_type, NM_SETTING_BRIDGE_SETTING_NAME)
        && _nm_connection_remove_setting(self, NM_TYPE_SETTING_BRIDGE_PORT))
        changed = TRUE;

    if (!nm_streq0(port_type, NM_SETTING_BOND_SETTING_NAME)
        && _nm_connection_remove_setting(self, NM_TYPE_SETTING_BOND_PORT))
        changed = TRUE;

    if (!nm_streq0(port_type, NM_SETTING_TEAM_SETTING_NAME)
        && _nm_connection_remove_setting(self, NM_TYPE_SETTING_TEAM_PORT))
        changed = TRUE;

    return changed;
}

/**
 * nm_connection_verify:
 * @connection: the #NMConnection to verify
 * @error: location to store error, or %NULL
 *
 * Validates the connection and all its settings.  Each setting's properties
 * have allowed values, and some values are dependent on other values.  For
 * example, if a Wi-Fi connection is security enabled, the #NMSettingWireless
 * setting object's 'security' property must contain the setting name of the
 * #NMSettingWirelessSecurity object, which must also be present in the
 * connection for the connection to be valid.  As another example, the
 * #NMSettingWired object's 'mac-address' property must be a validly formatted
 * MAC address.  The returned #GError contains information about which
 * setting and which property failed validation, and how it failed validation.
 *
 * Returns: %TRUE if the connection is valid, %FALSE if it is not
 **/
gboolean
nm_connection_verify(NMConnection *connection, GError **error)
{
    NMSettingVerifyResult result;

    result = _nm_connection_verify(connection, error);

    /* we treat normalizable connections as valid. */
    if (result == NM_SETTING_VERIFY_NORMALIZABLE)
        g_clear_error(error);

    return result == NM_SETTING_VERIFY_SUCCESS || result == NM_SETTING_VERIFY_NORMALIZABLE;
}

NMSettingVerifyResult
_nm_connection_verify(NMConnection *connection, GError **error)
{
    NMConnectionPrivate  *priv;
    NMSettingIPConfig    *s_ip4;
    NMSettingIPConfig    *s_ip6;
    NMSettingProxy       *s_proxy;
    gs_free_error GError *normalizable_error      = NULL;
    NMSettingVerifyResult normalizable_error_type = NM_SETTING_VERIFY_SUCCESS;
    int                   i;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NM_SETTING_VERIFY_ERROR);
    g_return_val_if_fail(!error || !*error, NM_SETTING_VERIFY_ERROR);

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    if (!_get_setting_by_metatype(priv, NM_META_SETTING_TYPE_CONNECTION)) {
        g_set_error_literal(error,
                            NM_CONNECTION_ERROR,
                            NM_CONNECTION_ERROR_MISSING_SETTING,
                            _("setting not found"));
        g_prefix_error(error, "%s: ", NM_SETTING_CONNECTION_SETTING_NAME);
        return NM_SETTING_VERIFY_ERROR;
    }

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting            *setting      = priv->settings[nm_meta_setting_types_by_priority[i]];
        GError               *verify_error = NULL;
        NMSettingVerifyResult verify_result;

        if (!setting)
            continue;

        /* verify all settings. We stop if we find the first non-normalizable
         * @NM_SETTING_VERIFY_ERROR. If we find normalizable errors we continue
         * but remember the error to return it to the user.
         * @NM_SETTING_VERIFY_NORMALIZABLE_ERROR has a higher priority then
         * @NM_SETTING_VERIFY_NORMALIZABLE, so, if we encounter such an error type,
         * we remember it instead (to return it as output).
         **/
        verify_result = _nm_setting_verify(setting, connection, &verify_error);
        if (verify_result == NM_SETTING_VERIFY_NORMALIZABLE
            || verify_result == NM_SETTING_VERIFY_NORMALIZABLE_ERROR) {
            if (verify_result == NM_SETTING_VERIFY_NORMALIZABLE_ERROR
                && normalizable_error_type == NM_SETTING_VERIFY_NORMALIZABLE) {
                /* NORMALIZABLE_ERROR has higher priority. */
                g_clear_error(&normalizable_error);
            }
            if (!normalizable_error) {
                g_propagate_error(&normalizable_error, verify_error);
                verify_error            = NULL;
                normalizable_error_type = verify_result;
            }
        } else if (verify_result != NM_SETTING_VERIFY_SUCCESS) {
            g_propagate_error(error, verify_error);
            g_return_val_if_fail(verify_result == NM_SETTING_VERIFY_ERROR, NM_SETTING_VERIFY_ERROR);
            return NM_SETTING_VERIFY_ERROR;
        }
        g_clear_error(&verify_error);
    }

    s_ip4   = _get_setting_by_metatype(priv, NM_META_SETTING_TYPE_IP4_CONFIG);
    s_ip6   = _get_setting_by_metatype(priv, NM_META_SETTING_TYPE_IP6_CONFIG);
    s_proxy = _get_setting_by_metatype(priv, NM_META_SETTING_TYPE_PROXY);

    nm_assert(normalizable_error_type != NM_SETTING_VERIFY_ERROR);
    if (NM_IN_SET(normalizable_error_type,
                  NM_SETTING_VERIFY_SUCCESS,
                  NM_SETTING_VERIFY_NORMALIZABLE)) {
        if (_supports_addr_family(connection, AF_INET)) {
            if (!s_ip4 && normalizable_error_type == NM_SETTING_VERIFY_SUCCESS) {
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_MISSING_SETTING,
                                    _("setting is required for non-port connections"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_IP4_CONFIG_SETTING_NAME);

                /* having a controller without IP config was not a verify() error, accept
                 * it for backward compatibility. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE;
            }
        } else {
            if (s_ip4) {
                g_clear_error(&normalizable_error);
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_INVALID_SETTING,
                                    _("setting not allowed in port connection"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_IP4_CONFIG_SETTING_NAME);
                /* having a port with IP config *was* and is a verify() error. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE_ERROR;
            }
        }

        if (_supports_addr_family(connection, AF_INET6)) {
            if (!s_ip6 && normalizable_error_type == NM_SETTING_VERIFY_SUCCESS) {
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_MISSING_SETTING,
                                    _("setting is required for non-port connections"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_IP6_CONFIG_SETTING_NAME);

                /* having a controller without IP config was not a verify() error, accept
                 * it for backward compatibility. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE;
            }
        } else {
            if (s_ip6) {
                g_clear_error(&normalizable_error);
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_INVALID_SETTING,
                                    _("setting not allowed in port connection"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_IP6_CONFIG_SETTING_NAME);
                /* having a port with IP config *was* and is a verify() error. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE_ERROR;
            }
        }

        if (_supports_addr_family(connection, AF_UNSPEC)) {
            if (!s_proxy && normalizable_error_type == NM_SETTING_VERIFY_SUCCESS) {
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_MISSING_SETTING,
                                    _("setting is required for non-port connections"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_PROXY_SETTING_NAME);

                /* having a controller without proxy config was not a verify() error, accept
                 * it for backward compatibility. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE;
            }
        } else {
            if (s_proxy) {
                g_clear_error(&normalizable_error);
                g_set_error_literal(&normalizable_error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_INVALID_SETTING,
                                    _("setting not allowed in port connection"));
                g_prefix_error(&normalizable_error, "%s: ", NM_SETTING_PROXY_SETTING_NAME);
                /* having a port with proxy config *was* and is a verify() error. */
                normalizable_error_type = NM_SETTING_VERIFY_NORMALIZABLE_ERROR;
            }
        }
    }

    if (normalizable_error_type != NM_SETTING_VERIFY_SUCCESS) {
        g_propagate_error(error, normalizable_error);
        normalizable_error = NULL;
        return normalizable_error_type;
    }

    return NM_SETTING_VERIFY_SUCCESS;
}

/**
 * nm_connection_verify_secrets:
 * @connection: the #NMConnection to verify in
 * @error: location to store error, or %NULL
 *
 * Verifies the secrets in the connection.
 *
 * Returns: %TRUE if the secrets are valid, %FALSE if they are not
 *
 * Since: 1.2
 **/
gboolean
nm_connection_verify_secrets(NMConnection *connection, GError **error)
{
    NMConnectionPrivate *priv;
    int                  i;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(!error || !*error, FALSE);

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        if (!priv->settings[i])
            continue;
        if (!nm_setting_verify_secrets(priv->settings[i], connection, error))
            return FALSE;
    }
    return TRUE;
}

static gboolean
_connection_normalize(NMConnection *connection,
                      GHashTable   *parameters,
                      gboolean     *modified,
                      GError      **error)
{
    NMSettingVerifyResult success;
    gboolean              was_modified;

#if NM_MORE_ASSERTS > 10
    /* only call this _nm_connection_verify() confirms that the connection
     * requires normalization and is normalizable. */
    nm_assert(NM_IN_SET(_nm_connection_verify(connection, NULL),
                        NM_SETTING_VERIFY_NORMALIZABLE,
                        NM_SETTING_VERIFY_NORMALIZABLE_ERROR));
#endif

    /* Try to perform all kind of normalizations on the settings to fix it.
     * We only do this, after verifying that the connection contains no un-normalizable
     * errors, because in that case we rather fail without touching the settings. */

    was_modified = FALSE;

    was_modified |= _normalize_connection_uuid(connection);
    was_modified |= _normalize_connection_type(connection);
    was_modified |= _normalize_connection_port_type(connection);
    was_modified |= _normalize_connection_secondaries(connection);
    was_modified |= _normalize_connection_ip_ping_addresses(connection);
    was_modified |= _normalize_connection(connection);
    was_modified |= _normalize_required_settings(connection);
    was_modified |= _normalize_invalid_port_port_settings(connection);
    was_modified |= _normalize_ip_config(connection, parameters);
    was_modified |= _normalize_ethernet_link_neg(connection);
    was_modified |= _normalize_infiniband(connection);
    was_modified |= _normalize_bond_mode(connection);
    was_modified |= _normalize_bond_options(connection);
    was_modified |= _normalize_wireless(connection);
    was_modified |= _normalize_macsec(connection);
    was_modified |= _normalize_team_config(connection);
    was_modified |= _normalize_team_port_config(connection);
    was_modified |= _normalize_bluetooth_type(connection);
    was_modified |= _normalize_ovs_interface_type(connection);
    was_modified |= _normalize_ip_tunnel_wired_setting(connection);
    was_modified |= _normalize_sriov_vf_order(connection);
    was_modified |= _normalize_bridge_vlan_order(connection);
    was_modified |= _normalize_bridge_port_vlan_order(connection);
    was_modified |= _normalize_gsm_auto_config(connection);
    was_modified |= _normalize_802_1x_empty_strings(connection);
    was_modified |= _normalize_ovs_port_trunks(connection);

    was_modified = !!was_modified;

    /* Verify anew */
    success = _nm_connection_verify(connection, error);

    NM_SET_OUT(modified, was_modified);

    if (success != NM_SETTING_VERIFY_SUCCESS) {
        /* we would expect, that after normalization, the connection can be verified.
         * Also treat NM_SETTING_VERIFY_NORMALIZABLE as failure, because there is something
         * odd going on. */
        if (error && !*error) {
            g_set_error_literal(error,
                                NM_CONNECTION_ERROR,
                                NM_CONNECTION_ERROR_FAILED,
                                _("Unexpected failure to normalize the connection"));
        }
        g_warning("connection did not verify after normalization: %s",
                  error ? (*error)->message : "??");
        g_return_val_if_reached(FALSE);
    }

    /* we would expect, that the connection was modified during normalization. */
    g_return_val_if_fail(was_modified, TRUE);

    return TRUE;
}

/**
 * nm_connection_normalize:
 * @connection: the #NMConnection to normalize
 * @parameters: (nullable) (element-type utf8 gpointer): a #GHashTable with
 * normalization parameters to allow customization of the normalization by providing
 * specific arguments. Unknown arguments will be ignored and the default will be
 * used. The keys must be strings compared with g_str_equal() function.
 * The values are opaque and depend on the parameter name.
 * @modified: (out) (optional): outputs whether any settings were modified.
 * @error: location to store error, or %NULL. Contains the reason,
 * why the connection is invalid, if the function returns an error.
 *
 * Does some basic normalization and fixup of well known inconsistencies
 * and deprecated fields. If the connection was modified in any way,
 * the output parameter @modified is set %TRUE.
 *
 * Finally the connection will be verified and %TRUE returns if the connection
 * is valid. As this function only performs some specific normalization steps
 * it cannot repair all connections. If the connection has errors that
 * cannot be normalized, the connection will not be modified.
 *
 * Returns: %TRUE if the connection is valid, %FALSE if it is not
 **/
gboolean
nm_connection_normalize(NMConnection *connection,
                        GHashTable   *parameters,
                        gboolean     *modified,
                        GError      **error)
{
    NMSettingVerifyResult success;
    gs_free_error GError *normalizable_error = NULL;

    success = _nm_connection_verify(connection, &normalizable_error);

    if (!NM_IN_SET(success, NM_SETTING_VERIFY_NORMALIZABLE, NM_SETTING_VERIFY_NORMALIZABLE_ERROR)) {
        if (normalizable_error) {
            nm_assert(success == NM_SETTING_VERIFY_ERROR);
            g_propagate_error(error, g_steal_pointer(&normalizable_error));
        } else
            nm_assert(success == NM_SETTING_VERIFY_SUCCESS);

        NM_SET_OUT(modified, FALSE);

        if (success != NM_SETTING_VERIFY_SUCCESS) {
            if (error && !*error) {
                g_set_error_literal(error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_FAILED,
                                    _("Unexpected failure to verify the connection"));
                return FALSE;
            }
            return FALSE;
        }

        if (error && *error)
            return FALSE;
        return TRUE;
    }

    return _connection_normalize(connection, parameters, modified, error);
}

gboolean
_nm_connection_ensure_normalized(NMConnection  *connection,
                                 gboolean       allow_modify,
                                 const char    *expected_uuid,
                                 gboolean       coerce_uuid,
                                 NMConnection **out_connection_clone,
                                 GError       **error)
{
    gs_unref_object NMConnection *connection_clone = NULL;
    gs_free_error GError         *local            = NULL;
    NMSettingVerifyResult         vresult;

    nm_assert(NM_IS_CONNECTION(connection));
    nm_assert(!out_connection_clone || !*out_connection_clone);
    nm_assert(!expected_uuid || nm_uuid_is_normalized(expected_uuid));

    if (expected_uuid) {
        if (nm_streq0(expected_uuid, nm_connection_get_uuid(connection)))
            expected_uuid = NULL;
        else if (!coerce_uuid || (!allow_modify && !out_connection_clone)) {
            g_set_error(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_INVALID_PROPERTY,
                        _("unexpected uuid %s instead of %s"),
                        nm_connection_get_uuid(connection),
                        expected_uuid);
            return FALSE;
        }
    }

    vresult = _nm_connection_verify(connection, &local);
    if (vresult != NM_SETTING_VERIFY_SUCCESS) {
        if (!NM_IN_SET(vresult,
                       NM_SETTING_VERIFY_NORMALIZABLE,
                       NM_SETTING_VERIFY_NORMALIZABLE_ERROR)) {
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }
        if (!allow_modify) {
            if (!out_connection_clone) {
                /* even NM_SETTING_VERIFY_NORMALIZABLE is treated as an error. We could normalize,
                 * but are not allowed to (and no out argument is provided for cloning).  */
                g_propagate_error(error, g_steal_pointer(&local));
                return FALSE;
            }
            connection_clone = nm_simple_connection_new_clone(connection);
            connection       = connection_clone;
        }
        if (!_connection_normalize(connection, NULL, NULL, error))
            g_return_val_if_reached(FALSE);
    }

    if (expected_uuid) {
        NMSettingConnection *s_con;

        if (!allow_modify && !connection_clone) {
            nm_assert(out_connection_clone);
            connection_clone = nm_simple_connection_new_clone(connection);
            connection       = connection_clone;
        }
        s_con = nm_connection_get_setting_connection(connection);
        g_object_set(s_con, NM_SETTING_CONNECTION_UUID, expected_uuid, NULL);
    }

    NM_SET_OUT(out_connection_clone, g_steal_pointer(&connection_clone));
    return TRUE;
}

/*****************************************************************************/

#if NM_MORE_ASSERTS
static void
_nm_assert_connection_unchanging_changed_cb(NMConnection *connection, gpointer user_data)
{
    nm_assert_not_reached();
}

static void
_nm_assert_connection_unchanging_secrets_updated_cb(NMConnection *connection,
                                                    const char   *setting_name,
                                                    gpointer      user_data)
{
    nm_assert_not_reached();
}

const char _nm_assert_connection_unchanging_user_data = 0;

void
nm_assert_connection_unchanging(NMConnection *connection)
{
    if (!connection)
        return;

    nm_assert(NM_IS_CONNECTION(connection));

    if (g_signal_handler_find(connection,
                              G_SIGNAL_MATCH_DATA,
                              0,
                              0,
                              NULL,
                              NULL,
                              (gpointer) &_nm_assert_connection_unchanging_user_data)
        != 0) {
        /* avoid connecting the assertion handler multiple times. */
        return;
    }

    g_signal_connect(connection,
                     NM_CONNECTION_CHANGED,
                     G_CALLBACK(_nm_assert_connection_unchanging_changed_cb),
                     (gpointer) &_nm_assert_connection_unchanging_user_data);
    g_signal_connect(connection,
                     NM_CONNECTION_SECRETS_CLEARED,
                     G_CALLBACK(_nm_assert_connection_unchanging_changed_cb),
                     (gpointer) &_nm_assert_connection_unchanging_user_data);
    g_signal_connect(connection,
                     NM_CONNECTION_SECRETS_UPDATED,
                     G_CALLBACK(_nm_assert_connection_unchanging_secrets_updated_cb),
                     (gpointer) &_nm_assert_connection_unchanging_user_data);
}
#endif

/*****************************************************************************/

/**
 * nm_connection_update_secrets:
 * @connection: the #NMConnection
 * @setting_name: the setting object name to which the secrets apply
 * @secrets: a #GVariant of secrets, of type %NM_VARIANT_TYPE_CONNECTION
 *   or %NM_VARIANT_TYPE_SETTING
 * @error: location to store error, or %NULL
 *
 * Update the specified setting's secrets, given a dictionary of secrets
 * intended for that setting (deserialized from D-Bus for example).  Will also
 * extract the given setting's secrets hash if given a connection dictionary.
 * If @setting_name is %NULL, expects a fully serialized #NMConnection as
 * returned by nm_connection_to_dbus() and will update all secrets from all
 * settings contained in @secrets.
 *
 * Returns: %TRUE if the secrets were successfully updated, %FALSE if the update
 * failed (tried to update secrets for a setting that doesn't exist, etc)
 **/
gboolean
nm_connection_update_secrets(NMConnection *connection,
                             const char   *setting_name,
                             GVariant     *secrets,
                             GError      **error)
{
    NMSetting   *setting;
    gboolean     success      = TRUE;
    gboolean     updated      = FALSE;
    GVariant    *setting_dict = NULL;
    GVariantIter iter;
    const char  *key;
    gboolean     full_connection;
    int          success_detail;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);

    full_connection = g_variant_is_of_type(secrets, NM_VARIANT_TYPE_CONNECTION);

    g_return_val_if_fail(full_connection || g_variant_is_of_type(secrets, NM_VARIANT_TYPE_SETTING),
                         FALSE);
    g_return_val_if_fail(!error || !*error, FALSE);
    g_return_val_if_fail(setting_name || full_connection, FALSE);

    /* Empty @secrets means success */
    if (g_variant_n_children(secrets) == 0)
        return TRUE;

    if (setting_name) {
        /* Update just one setting's secrets */
        setting = nm_connection_get_setting_by_name(connection, setting_name);
        if (!setting) {
            g_set_error_literal(error,
                                NM_CONNECTION_ERROR,
                                NM_CONNECTION_ERROR_SETTING_NOT_FOUND,
                                setting_name);
            return FALSE;
        }

        if (full_connection) {
            setting_dict = g_variant_lookup_value(secrets, setting_name, NM_VARIANT_TYPE_SETTING);
            if (!setting_dict) {
                /* The connection dictionary didn't contain any secrets for
                 * @setting_name; just return success.
                 */
                return TRUE;
            }
        }

        _setting_notify_block(connection, setting);
        success_detail = _nm_setting_update_secrets(setting, setting_dict ?: secrets, error);
        _setting_notify_unblock(connection, setting);

        nm_clear_pointer(&setting_dict, g_variant_unref);

        if (success_detail == NM_SETTING_UPDATE_SECRET_ERROR) {
            nm_assert(!error || *error);
            return FALSE;
        }
        if (success_detail == NM_SETTING_UPDATE_SECRET_SUCCESS_MODIFIED)
            updated = TRUE;
    } else {
        /* check first, whether all the settings exist... */
        g_variant_iter_init(&iter, secrets);
        while (g_variant_iter_next(&iter, "{&s@a{sv}}", &key, NULL)) {
            setting = nm_connection_get_setting_by_name(connection, key);
            if (!setting) {
                g_set_error_literal(error,
                                    NM_CONNECTION_ERROR,
                                    NM_CONNECTION_ERROR_SETTING_NOT_FOUND,
                                    key);
                return FALSE;
            }
        }

        /* Update each setting with any secrets from the connection dictionary */
        g_variant_iter_init(&iter, secrets);
        while (g_variant_iter_next(&iter, "{&s@a{sv}}", &key, &setting_dict)) {
            gs_free_error GError *local = NULL;

            /* Update the secrets for this setting */
            setting = nm_connection_get_setting_by_name(connection, key);

            _setting_notify_block(connection, setting);
            success_detail =
                _nm_setting_update_secrets(setting, setting_dict, error ? &local : NULL);
            _setting_notify_unblock(connection, setting);

            g_variant_unref(setting_dict);

            if (success_detail == NM_SETTING_UPDATE_SECRET_ERROR) {
                if (success) {
                    if (error) {
                        nm_assert(local);
                        g_propagate_error(error, g_steal_pointer(&local));
                        error = NULL;
                    } else
                        nm_assert(!local);
                    success = FALSE;
                }
                break;
            }
            if (success_detail == NM_SETTING_UPDATE_SECRET_SUCCESS_MODIFIED)
                updated = TRUE;
        }
    }

    if (updated)
        _signal_emit_secrets_updated(connection, setting_name);

    return success;
}

static const char *
_need_secrets(NMConnection *connection, gboolean check_rerequest, GPtrArray **hints)
{
    NMSetting           *setting_before = NULL;
    NMConnectionPrivate *priv;
    int                  i;

    nm_assert(NM_IS_CONNECTION(connection));
    nm_assert(!hints || !*hints);

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    /* Get list of settings in priority order */
    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[nm_meta_setting_types_by_priority[i]];
        GPtrArray *secrets;

        if (!setting)
            continue;

        nm_assert(!setting_before || _nm_setting_sort_for_nm_assert(setting_before, setting) < 0);
        nm_assert(!setting_before || _nm_setting_compare_priority(setting_before, setting) <= 0);
        setting_before = setting;

        secrets = _nm_setting_need_secrets(setting, check_rerequest);
        if (!secrets)
            continue;

        if (hints)
            *hints = secrets;
        else
            g_ptr_array_free(secrets, TRUE);

        return nm_setting_get_name(setting);
    }

    return NULL;
}

/**
 * nm_connection_need_secrets:
 * @connection: the #NMConnection
 * @hints: (out) (element-type utf8) (nullable) (transfer container):
 *   the address of a pointer to a #GPtrArray, initialized to %NULL, which on
 *   return points to an allocated #GPtrArray containing the property names of
 *   secrets of the #NMSetting which may be required; the caller owns the array
 *   and must free the array itself with g_ptr_array_free(), but not free its
 *   elements
 *
 * Returns the name of the first setting object in the connection which would
 * need secrets to make a successful connection.  The returned hints are only
 * intended as a guide to what secrets may be required, because in some
 * circumstances, there is no way to conclusively determine exactly which
 * secrets are needed.
 *
 * Returns: (nullable): the setting name of the #NMSetting object which has
 *   invalid or missing secrets
 **/
const char *
nm_connection_need_secrets(NMConnection *connection, GPtrArray **hints)
{
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);
    g_return_val_if_fail(!hints || !*hints, NULL);

    return _need_secrets(connection, FALSE, hints);
}

/**
 * nm_connection_need_secrets_for_rerequest:
 * @connection: the #NMConnection
 *
 * Returns: %TRUE if some secret needs to be re-requested
 **/
gboolean
nm_connection_need_secrets_for_rerequest(NMConnection *connection)
{
    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);

    return !!_need_secrets(connection, TRUE, NULL);
}

/**
 * nm_connection_clear_secrets:
 * @connection: the #NMConnection
 *
 * Clears and frees any secrets that may be stored in the connection, to avoid
 * keeping secret data in memory when not needed.
 **/
void
nm_connection_clear_secrets(NMConnection *connection)
{
    return nm_connection_clear_secrets_with_flags(connection, NULL, NULL);
}

/**
 * nm_connection_clear_secrets_with_flags:
 * @connection: the #NMConnection
 * @func: (scope call) (nullable): function to be called to determine whether a
 *     specific secret should be cleared or not. If %NULL, all secrets are cleared.
 * @user_data: caller-supplied data passed to @func
 *
 * Clears and frees secrets determined by @func.
 **/
void
nm_connection_clear_secrets_with_flags(NMConnection                    *connection,
                                       NMSettingClearSecretsWithFlagsFn func,
                                       gpointer                         user_data)
{
    NMConnectionPrivate *priv;
    int                  i;

    g_return_if_fail(NM_IS_CONNECTION(connection));

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[i];

        if (!setting)
            continue;

        _setting_notify_block(connection, setting);
        _nm_setting_clear_secrets(setting, func, user_data);
        _setting_notify_unblock(connection, setting);
    }

    _signal_emit_secrets_cleared(connection);
}

static gboolean
_clear_secrets_by_secret_flags_cb(NMSetting           *setting,
                                  const char          *secret,
                                  NMSettingSecretFlags flags,
                                  gpointer             user_data)
{
    NMSettingSecretFlags filter_flags = GPOINTER_TO_UINT(user_data);
    gboolean             remove_secret;

    if (filter_flags == NM_SETTING_SECRET_FLAG_NONE) {
        /* Can't use bitops with SECRET_FLAG_NONE so handle that specifically */
        remove_secret = (flags != NM_SETTING_SECRET_FLAG_NONE);
    } else {
        /* Otherwise, if the secret has at least one of the desired flags keep it */
        remove_secret = !NM_FLAGS_ANY(flags, filter_flags);
    }

    return remove_secret;
}

/**
 * _nm_connection_clear_secrets_by_secret_flags:
 * @self: the #NMConnection to filter (will be modified)
 * @filter_flags: the secret flags to control whether to drop/remove
 *   a secret or to keep it. The meaning of the filter flags is to
 *   preserve the secrets. The secrets that have matching (see below)
 *   flags are kept, the others are dropped.
 *
 * Removes/drops secrets from @self according to @filter_flags.
 * If @filter_flags is %NM_SETTING_SECRET_NONE, then only secrets that
 * have %NM_SETTING_SECRET_NONE flags are kept.
 * Otherwise, only secrets with secret flags are kept that have at least
 * one of the filter flags.
 */
void
_nm_connection_clear_secrets_by_secret_flags(NMConnection *self, NMSettingSecretFlags filter_flags)
{
    nm_connection_clear_secrets_with_flags(self,
                                           _clear_secrets_by_secret_flags_cb,
                                           GUINT_TO_POINTER(filter_flags));
}

/*****************************************************************************/

/*****************************************************************************/

/* Returns always a non-NULL, floating variant that must
 * be unrefed by the caller. */
GVariant *
_nm_connection_for_each_secret(NMConnection                  *self,
                               GVariant                      *secrets,
                               gboolean                       remove_non_secrets,
                               _NMConnectionForEachSecretFunc callback,
                               gpointer                       callback_data)
{
    GVariantBuilder secrets_builder;
    GVariantBuilder setting_builder;
    GVariantIter    secrets_iter;
    GVariantIter   *setting_iter;
    const char     *setting_name;

    /* This function, given a dict of dicts representing new secrets of
     * an NMConnection, walks through each toplevel dict (which represents a
     * NMSetting), and for each setting, walks through that setting dict's
     * properties.  For each property that's a secret, it will check that
     * secret's flags in the backing NMConnection object, and call a supplied
     * callback.
     *
     * The one complexity is that the VPN setting's 'secrets' property is
     * *also* a dict (since the key/value pairs are arbitrary and known
     * only to the VPN plugin itself).  That means we have three levels of
     * dicts that we potentially have to traverse here.  The differences
     * are handled by the virtual for_each_secret() function.
     */

    g_return_val_if_fail(callback, NULL);

    g_variant_iter_init(&secrets_iter, secrets);
    g_variant_builder_init(&secrets_builder, NM_VARIANT_TYPE_CONNECTION);
    while (g_variant_iter_next(&secrets_iter, "{&sa{sv}}", &setting_name, &setting_iter)) {
        _nm_unused nm_auto_free_variant_iter GVariantIter *setting_iter_free = setting_iter;
        NMSetting                                         *setting;
        const char                                        *secret_name;
        GVariant                                          *val;

        setting = nm_connection_get_setting_by_name(self, setting_name);
        if (!setting)
            continue;

        g_variant_builder_init(&setting_builder, NM_VARIANT_TYPE_SETTING);
        while (g_variant_iter_next(setting_iter, "{&sv}", &secret_name, &val)) {
            _nm_unused gs_unref_variant GVariant *val_free = val;

            NM_SETTING_GET_CLASS(setting)->for_each_secret(setting,
                                                           secret_name,
                                                           val,
                                                           remove_non_secrets,
                                                           callback,
                                                           callback_data,
                                                           &setting_builder);
        }

        g_variant_builder_add(&secrets_builder, "{sa{sv}}", setting_name, &setting_builder);
    }

    return g_variant_builder_end(&secrets_builder);
}

/*****************************************************************************/

typedef struct {
    NMConnectionFindSecretFunc find_func;
    gpointer                   find_func_data;
    gboolean                   found;
} FindSecretData;

static gboolean
find_secret_for_each_func(NMSettingSecretFlags flags, gpointer user_data)
{
    FindSecretData *data = user_data;

    if (!data->found)
        data->found = data->find_func(flags, data->find_func_data);
    return FALSE;
}

gboolean
_nm_connection_find_secret(NMConnection              *self,
                           GVariant                  *secrets,
                           NMConnectionFindSecretFunc callback,
                           gpointer                   callback_data)
{
    gs_unref_variant GVariant *dummy = NULL;
    FindSecretData             data  = {
                     .find_func      = callback,
                     .find_func_data = callback_data,
                     .found          = FALSE,
    };

    dummy = _nm_connection_for_each_secret(self, secrets, FALSE, find_secret_for_each_func, &data);
    return data.found;
}

/*****************************************************************************/

static const NMConnectionSerializationOptions _connection_serialization_options_empty = {
    .timestamp =
        {
            .has = FALSE,
            .val = 0,
        },
    .seen_bssids = NULL,
};

gboolean
nm_connection_serialization_options_equal(const NMConnectionSerializationOptions *a,
                                          const NMConnectionSerializationOptions *b)
{
    if (!a)
        a = &_connection_serialization_options_empty;
    if (!b)
        b = &_connection_serialization_options_empty;

    if (a == b)
        return TRUE;

    if (a->timestamp.has != b->timestamp.has)
        return FALSE;
    if (a->timestamp.has && a->timestamp.val != b->timestamp.val)
        return FALSE;
    if (!nm_strv_equal(a->seen_bssids ?: NM_STRV_EMPTY_CC(), b->seen_bssids ?: NM_STRV_EMPTY_CC()))
        return FALSE;

    return TRUE;
}

/**
 * nm_connection_to_dbus:
 * @connection: the #NMConnection
 * @flags: serialization flags, e.g. %NM_CONNECTION_SERIALIZE_ALL
 *
 * Converts the #NMConnection into a #GVariant of type
 * %NM_VARIANT_TYPE_CONNECTION describing the connection, suitable for
 * marshalling over D-Bus or otherwise serializing.
 *
 * Returns: (transfer none): a new floating #GVariant describing the connection,
 * its settings, and each setting's properties.
 **/
GVariant *
nm_connection_to_dbus(NMConnection *connection, NMConnectionSerializationFlags flags)
{
    return nm_connection_to_dbus_full(connection, flags, NULL);
}

GVariant *
nm_connection_to_dbus_full(NMConnection                           *connection,
                           NMConnectionSerializationFlags          flags,
                           const NMConnectionSerializationOptions *options)
{
    NMConnectionPrivate *priv;
    GVariantBuilder      builder;
    gboolean             any = FALSE;
    int                  i;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    if (!options)
        options = &_connection_serialization_options_empty;

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[nm_meta_setting_types_by_priority[i]];
        GVariant  *setting_dict;

        if (!setting)
            continue;

        setting_dict = _nm_setting_to_dbus(setting, connection, flags, options);
        if (!setting_dict)
            continue;
        if (!any) {
            any = TRUE;
            g_variant_builder_init(&builder, NM_VARIANT_TYPE_CONNECTION);
        }
        g_variant_builder_add(&builder, "{s@a{sv}}", nm_setting_get_name(setting), setting_dict);
    }

    if (!any)
        return NULL;

    return g_variant_builder_end(&builder);
}

/**
 * nm_connection_is_type:
 * @connection: the #NMConnection
 * @type: a setting name to check the connection's type against (like
 * %NM_SETTING_WIRELESS_SETTING_NAME or %NM_SETTING_WIRED_SETTING_NAME)
 *
 * A convenience function to check if the given @connection is a particular
 * type (ie wired, Wi-Fi, ppp, etc). Checks the #NMSettingConnection:type
 * property of the connection and matches that against @type.
 *
 * Returns: %TRUE if the connection is of the given @type, %FALSE if not
 **/
gboolean
nm_connection_is_type(NMConnection *connection, const char *type)
{
    g_return_val_if_fail(type, FALSE);

    return nm_streq0(type, nm_connection_get_connection_type(connection));
}

int
_nm_setting_sort_for_nm_assert(NMSetting *a, NMSetting *b)
{
    g_assert(NM_IS_SETTING(a));
    g_assert(NM_IS_SETTING(b));
    g_assert(a != b);
    g_assert(G_OBJECT_TYPE(a) != G_OBJECT_TYPE(b));

    NM_CMP_RETURN(_nm_setting_compare_priority(a, b));
    NM_CMP_DIRECT_STRCMP(nm_setting_get_name(a), nm_setting_get_name(b));

    g_assert_not_reached();
    return 0;
}

NMSetting **
_nm_connection_get_settings_arr(NMConnection *connection)
{
    nm_assert(NM_IS_CONNECTION(connection));

    return NM_CONNECTION_GET_PRIVATE(connection)->settings;
}

/**
 * nm_connection_get_settings:
 * @connection: the #NMConnection instance
 * @out_length: (out) (optional): the length of the returned array
 *
 * Retrieves the settings in @connection.
 *
 * The returned array is %NULL-terminated.
 *
 * Returns: (array length=out_length) (transfer container) (nullable): a
 *   %NULL-terminated array containing every setting of @connection.
 *   If the connection has no settings, %NULL is returned.
 *
 * Since: 1.10
 */
NMSetting **
nm_connection_get_settings(NMConnection *connection, guint *out_length)
{
    NMConnectionPrivate *priv;
    NMSetting          **arr;
    int                  len;
    int                  i;
    int                  j;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    len = 0;
    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        if (priv->settings[i])
            len++;
    }

    NM_SET_OUT(out_length, len);

    if (len == 0)
        return NULL;

    arr = g_new(NMSetting *, len + 1);
    for (i = 0, j = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[nm_meta_setting_types_by_priority[i]];

        if (setting) {
            nm_assert(j == 0 || _nm_setting_sort_for_nm_assert(arr[j - 1], setting) < 0);
            arr[j++] = setting;
        }
    }
    arr[len] = NULL;

    return arr;
}

/**
 * nm_connection_for_each_setting_value:
 * @connection: the #NMConnection
 * @func: (scope call): user-supplied function called for each setting's property
 * @user_data: user data passed to @func at each invocation
 *
 * Iterates over the properties of each #NMSetting object in the #NMConnection,
 * calling the supplied user function for each property.
 **/
void
nm_connection_for_each_setting_value(NMConnection        *connection,
                                     NMSettingValueIterFn func,
                                     gpointer             user_data)
{
    gs_free NMSetting **settings = NULL;
    guint               length   = 0;
    guint               i;

    g_return_if_fail(NM_IS_CONNECTION(connection));
    g_return_if_fail(func);

    settings = nm_connection_get_settings(connection, &length);
    for (i = 1; i < length; i++)
        g_object_ref(settings[i]);
    for (i = 0; i < length; i++)
        nm_setting_enumerate_values(settings[i], func, user_data);
    for (i = 1; i < length; i++)
        g_object_unref(settings[i]);
}

/**
 * _nm_connection_aggregate:
 * @connection: the #NMConnection for which values are to be aggregated.
 * @type: one of the supported aggregate types.
 * @arg: the input/output argument that depends on @type.
 *
 * For example, with %NM_CONNECTION_AGGREGATE_ANY_SECRETS and
 * %NM_CONNECTION_AGGREGATE_ANY_SYSTEM_SECRET_FLAGS @arg is a boolean
 * output argument. It is either %NULL or a pointer to an gboolean
 * out-argument. The function will always set @arg if given.
 * Also, the return value of the function is likewise the result
 * that is set to @arg.
 *
 * Returns: a boolean result with the meaning depending on the aggregation
 *   type @type.
 */
gboolean
_nm_connection_aggregate(NMConnection *connection, NMConnectionAggregateType type, gpointer arg)
{
    NMConnectionPrivate *priv;
    gboolean             arg_boolean;
    gboolean             completed_early;
    gpointer             my_arg;
    int                  i;

    g_return_val_if_fail(NM_IS_CONNECTION(connection), FALSE);

    switch (type) {
    case NM_CONNECTION_AGGREGATE_ANY_SECRETS:
        arg_boolean = FALSE;
        my_arg      = &arg_boolean;
        goto good;
    case NM_CONNECTION_AGGREGATE_ANY_SYSTEM_SECRET_FLAGS:
        arg_boolean = FALSE;
        my_arg      = &arg_boolean;
        goto good;
    }
    g_return_val_if_reached(FALSE);

good:
    priv = NM_CONNECTION_GET_PRIVATE(connection);

    completed_early = FALSE;
    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[i];

        if (!setting)
            continue;
        if (_nm_setting_aggregate(setting, type, my_arg)) {
            completed_early = TRUE;
            break;
        }
        nm_assert(my_arg != &arg_boolean || !arg_boolean);
    }

    if (my_arg == &arg_boolean) {
        nm_assert(completed_early == arg_boolean);
        if (arg)
            *((gboolean *) arg) = arg_boolean;
        return arg_boolean;
    }

    nm_assert_not_reached();
    return FALSE;
}

/**
 * nm_connection_dump:
 * @connection: the #NMConnection
 *
 * Print the connection (including secrets!) to stdout. For debugging
 * purposes ONLY, should NOT be used for serialization of the setting,
 * or machine-parsed in any way. The output format is not guaranteed to
 * be stable and may change at any time.
 **/
void
nm_connection_dump(NMConnection *connection)
{
    NMConnectionPrivate *priv;
    int                  i;

    if (!connection)
        return;

    g_return_if_fail(NM_IS_CONNECTION(connection));

    priv = NM_CONNECTION_GET_PRIVATE(connection);

    for (i = 0; i < (int) _NM_META_SETTING_TYPE_NUM; i++) {
        NMSetting *setting = priv->settings[nm_meta_setting_types_by_priority[i]];

        if (setting) {
            gs_free char *str = NULL;

            str = nm_setting_to_string(setting);
            g_print("%s\n", str);
        }
    }
}

/**
 * nm_connection_set_path:
 * @connection: the #NMConnection
 * @path: the D-Bus path of the connection as given by the settings service
 * which provides the connection
 *
 * Sets the D-Bus path of the connection.  This property is not serialized, and
 * is only for the reference of the caller.  Sets the #NMConnection:path
 * property.
 **/
void
nm_connection_set_path(NMConnection *connection, const char *path)
{
    g_return_if_fail(NM_IS_CONNECTION(connection));

    nm_ref_string_reset_str(&NM_CONNECTION_GET_PRIVATE(connection)->path, path);
}

void
_nm_connection_set_path_rstr(NMConnection *connection, NMRefString *path)
{
    nm_ref_string_reset(&NM_CONNECTION_GET_PRIVATE(connection)->path, path);
}

/**
 * nm_connection_get_path:
 * @connection: the #NMConnection
 *
 * Returns the connection's D-Bus path.
 *
 * Returns: the D-Bus path of the connection, previously set by a call to
 * nm_connection_set_path().
 **/
const char *
nm_connection_get_path(NMConnection *connection)
{
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);

    return nm_ref_string_get_str(NM_CONNECTION_GET_PRIVATE(connection)->path);
}

NMRefString *
_nm_connection_get_path_rstr(NMConnection *connection)
{
    return NM_CONNECTION_GET_PRIVATE(connection)->path;
}

/**
 * nm_connection_get_interface_name:
 * @connection: The #NMConnection
 *
 * Returns the interface name as stored in NMSettingConnection:interface_name.
 * If the connection contains no NMSettingConnection, it will return %NULL.
 *
 * For hardware devices and software devices created outside of NetworkManager,
 * this name is used to match the device. for software devices created by
 * NetworkManager, this is the name of the created interface.
 *
 * Returns: Name of the kernel interface or %NULL
 */
const char *
nm_connection_get_interface_name(NMConnection *connection)
{
    NMSettingConnection *s_con;

    s_con = nm_connection_get_setting_connection(connection);
    return s_con ? nm_setting_connection_get_interface_name(s_con) : NULL;
}

NMConnectionMultiConnect
_nm_connection_get_multi_connect(NMConnection *connection)
{
    NMSettingConnection           *s_con;
    NMConnectionMultiConnect       multi_connect;
    const NMConnectionMultiConnect DEFAULT = NM_CONNECTION_MULTI_CONNECT_SINGLE;

    /* connection.multi_connect property cannot be specified via regular
     * connection defaults in NetworkManager.conf, because those are per-device,
     * and we need to determine the multi_connect independent of a particular
     * device.
     *
     * There is however still a default-value, so theoretically, the default
     * value could be specified in NetworkManager.conf. Just not as [connection*]
     * and indepdented of a device. */

    s_con = nm_connection_get_setting_connection(connection);
    if (!s_con)
        return DEFAULT;

    multi_connect = nm_setting_connection_get_multi_connect(s_con);
    return multi_connect == NM_CONNECTION_MULTI_CONNECT_DEFAULT ? DEFAULT : multi_connect;
}

gboolean
_nm_connection_verify_required_interface_name(NMConnection *connection, GError **error)
{
    const char *interface_name;

    if (!connection)
        return TRUE;

    interface_name = nm_connection_get_interface_name(connection);
    if (interface_name)
        return TRUE;

    g_set_error_literal(error,
                        NM_CONNECTION_ERROR,
                        NM_CONNECTION_ERROR_MISSING_PROPERTY,
                        _("property is missing"));
    g_prefix_error(error,
                   "%s.%s: ",
                   NM_SETTING_CONNECTION_SETTING_NAME,
                   NM_SETTING_CONNECTION_INTERFACE_NAME);
    return FALSE;
}

/**
 * nm_connection_get_uuid:
 * @connection: the #NMConnection
 *
 * A shortcut to return the UUID from the connection's #NMSettingConnection.
 *
 * Returns: the UUID from the connection's 'connection' setting
 **/
const char *
nm_connection_get_uuid(NMConnection *connection)
{
    NMSettingConnection *s_con;

    s_con = nm_connection_get_setting_connection(connection);
    return s_con ? nm_setting_connection_get_uuid(s_con) : NULL;
}

/**
 * nm_connection_get_id:
 * @connection: the #NMConnection
 *
 * A shortcut to return the ID from the connection's #NMSettingConnection.
 *
 * Returns: the ID from the connection's 'connection' setting
 **/
const char *
nm_connection_get_id(NMConnection *connection)
{
    NMSettingConnection *s_con;

    s_con = nm_connection_get_setting_connection(connection);
    return s_con ? nm_setting_connection_get_id(s_con) : NULL;
}

/**
 * nm_connection_get_connection_type:
 * @connection: the #NMConnection
 *
 * A shortcut to return the type from the connection's #NMSettingConnection.
 *
 * Returns: the type from the connection's 'connection' setting
 **/
const char *
nm_connection_get_connection_type(NMConnection *connection)
{
    NMSettingConnection *s_con;

    s_con = nm_connection_get_setting_connection(connection);
    return s_con ? nm_setting_connection_get_connection_type(s_con) : NULL;
}

/**
 * nm_connection_is_virtual:
 * @connection: an #NMConnection
 *
 * Checks if @connection refers to a virtual device (and thus can potentially be
 * activated even if the device it refers to doesn't exist).
 *
 * Returns: whether @connection refers to a virtual device
 */
gboolean
nm_connection_is_virtual(NMConnection *connection)
{
    const char *type;

    type = nm_connection_get_connection_type(connection);
    if (!type)
        return FALSE;

    if (NM_IN_STRSET(type,
                     NM_SETTING_6LOWPAN_SETTING_NAME,
                     NM_SETTING_BOND_SETTING_NAME,
                     NM_SETTING_BRIDGE_SETTING_NAME,
                     NM_SETTING_DUMMY_SETTING_NAME,
                     NM_SETTING_HSR_SETTING_NAME,
                     NM_SETTING_IP_TUNNEL_SETTING_NAME,
                     NM_SETTING_IPVLAN_SETTING_NAME,
                     NM_SETTING_MACSEC_SETTING_NAME,
                     NM_SETTING_MACVLAN_SETTING_NAME,
                     NM_SETTING_OVS_BRIDGE_SETTING_NAME,
                     NM_SETTING_OVS_INTERFACE_SETTING_NAME,
                     NM_SETTING_OVS_PORT_SETTING_NAME,
                     NM_SETTING_TEAM_SETTING_NAME,
                     NM_SETTING_TUN_SETTING_NAME,
                     NM_SETTING_VETH_SETTING_NAME,
                     NM_SETTING_VLAN_SETTING_NAME,
                     NM_SETTING_VRF_SETTING_NAME,
                     NM_SETTING_VXLAN_SETTING_NAME,
                     NM_SETTING_WIREGUARD_SETTING_NAME))
        return TRUE;

    if (nm_streq(type, NM_SETTING_INFINIBAND_SETTING_NAME)) {
        NMSettingInfiniband *s_ib;

        s_ib = nm_connection_get_setting_infiniband(connection);
        return s_ib && nm_setting_infiniband_get_virtual_interface_name(s_ib);
    }

    if (nm_streq(type, NM_SETTING_BLUETOOTH_SETTING_NAME))
        return !!_nm_connection_get_setting_bluetooth_for_nap(connection);

    if (nm_streq(type, NM_SETTING_PPPOE_SETTING_NAME)) {
        NMSettingPppoe *s_pppoe;

        s_pppoe = nm_connection_get_setting_pppoe(connection);
        return !!nm_setting_pppoe_get_parent(s_pppoe);
    }

    if (nm_streq(type, NM_SETTING_GENERIC_SETTING_NAME)) {
        NMSettingGeneric *s_generic;

        s_generic = nm_connection_get_setting_generic(connection);
        return !!nm_setting_generic_get_device_handler(s_generic);
    }

    return FALSE;
}

/**
 * nm_connection_get_virtual_device_description:
 * @connection: an #NMConnection for a virtual device type
 *
 * Returns the name that nm_device_disambiguate_names() would
 * return for the virtual device that would be created for @connection.
 * Eg, "VLAN (eth1.1)".
 *
 * Returns: (transfer full): the name of @connection's device,
 *   or %NULL if @connection is not a virtual connection type
 */
char *
nm_connection_get_virtual_device_description(NMConnection *connection)
{
    const char *type;
    const char *iface = NULL, *display_type = NULL;

    type = nm_connection_get_connection_type(connection);
    if (!type)
        return NULL;

    iface = nm_connection_get_interface_name(connection);

    if (nm_streq(type, NM_SETTING_BOND_SETTING_NAME))
        display_type = _("Bond");
    else if (nm_streq(type, NM_SETTING_TEAM_SETTING_NAME))
        display_type = _("Team");
    else if (nm_streq(type, NM_SETTING_BRIDGE_SETTING_NAME))
        display_type = _("Bridge");
    else if (nm_streq(type, NM_SETTING_VLAN_SETTING_NAME))
        display_type = _("VLAN");
    else if (nm_streq(type, NM_SETTING_INFINIBAND_SETTING_NAME)) {
        display_type = _("InfiniBand");
        iface        = nm_setting_infiniband_get_virtual_interface_name(
            nm_connection_get_setting_infiniband(connection));
    } else if (nm_streq(type, NM_SETTING_IP_TUNNEL_SETTING_NAME))
        display_type = _("IP Tunnel");
    else if (nm_streq(type, NM_SETTING_WIREGUARD_SETTING_NAME))
        display_type = _("WireGuard");
    else if (nm_streq(type, NM_SETTING_TUN_SETTING_NAME))
        display_type = _("TUN/TAP");
    else if (nm_streq(type, NM_SETTING_VETH_SETTING_NAME))
        display_type = _("Veth");
    else if (nm_streq(type, NM_SETTING_LOOPBACK_SETTING_NAME))
        display_type = _("Loopback");

    if (!iface || !display_type)
        return NULL;

    return g_strdup_printf("%s (%s)", display_type, iface);
}

/*****************************************************************************/

/**
 * nm_connection_get_setting_802_1x:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSetting8021x the connection might contain.
 *
 * Returns: (transfer none): an #NMSetting8021x if the connection contains one, otherwise %NULL
 **/
NMSetting8021x *
nm_connection_get_setting_802_1x(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_802_1X);
}

/**
 * nm_connection_get_setting_bluetooth:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingBluetooth the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingBluetooth if the connection contains one, otherwise %NULL
 **/
NMSettingBluetooth *
nm_connection_get_setting_bluetooth(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_BLUETOOTH);
}

/**
 * nm_connection_get_setting_bond:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingBond the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingBond if the connection contains one, otherwise %NULL
 **/
NMSettingBond *
nm_connection_get_setting_bond(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_BOND);
}

/**
 * nm_connection_get_setting_team:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingTeam the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingTeam if the connection contains one, otherwise %NULL
 **/
NMSettingTeam *
nm_connection_get_setting_team(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_TEAM);
}

/**
 * nm_connection_get_setting_team_port:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingTeamPort the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingTeamPort if the connection contains one, otherwise %NULL
 **/
NMSettingTeamPort *
nm_connection_get_setting_team_port(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_TEAM_PORT);
}

/**
 * nm_connection_get_setting_bridge:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingBridge the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingBridge if the connection contains one, otherwise %NULL
 **/
NMSettingBridge *
nm_connection_get_setting_bridge(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_BRIDGE);
}

/**
 * nm_connection_get_setting_cdma:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingCdma the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingCdma if the connection contains one, otherwise %NULL
 **/
NMSettingCdma *
nm_connection_get_setting_cdma(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_CDMA);
}

/**
 * nm_connection_get_setting_connection:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingConnection the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingConnection if the connection contains one, otherwise %NULL
 **/
NMSettingConnection *
nm_connection_get_setting_connection(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_CONNECTION);
}

/**
 * nm_connection_get_setting_dcb:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingDcb the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingDcb if the connection contains one, otherwise NULL
 **/
NMSettingDcb *
nm_connection_get_setting_dcb(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_DCB);
}

/**
 * nm_connection_get_setting_dummy:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingDummy the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingDummy if the connection contains one, otherwise %NULL
 *
 * Since: 1.8
 **/
NMSettingDummy *
nm_connection_get_setting_dummy(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_DUMMY);
}

/**
 * nm_connection_get_setting_generic:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingGeneric the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingGeneric if the connection contains one, otherwise NULL
 **/
NMSettingGeneric *
nm_connection_get_setting_generic(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_GENERIC);
}

/**
 * nm_connection_get_setting_gsm:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingGsm the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingGsm if the connection contains one, otherwise %NULL
 **/
NMSettingGsm *
nm_connection_get_setting_gsm(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_GSM);
}

/**
 * nm_connection_get_setting_infiniband:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingInfiniband the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingInfiniband if the connection contains one, otherwise %NULL
 **/
NMSettingInfiniband *
nm_connection_get_setting_infiniband(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_INFINIBAND);
}

/**
 * nm_connection_get_setting_ip4_config:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingIP4Config the connection might contain.
 *
 * Note that it returns the value as type #NMSettingIPConfig, since the vast
 * majority of IPv4-setting-related methods are on that type, not
 * #NMSettingIP4Config.
 *
 * Returns: (type NMSettingIP4Config) (transfer none): an #NMSettingIP4Config if the
 * connection contains one, otherwise %NULL
 **/
NMSettingIPConfig *
nm_connection_get_setting_ip4_config(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_IP4_CONFIG);
}

/**
 * nm_connection_get_setting_ip_tunnel:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingIPTunnel the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingIPTunnel if the connection contains one, otherwise %NULL
 *
 * Since: 1.2
 **/
NMSettingIPTunnel *
nm_connection_get_setting_ip_tunnel(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_IP_TUNNEL);
}

/**
 * nm_connection_get_setting_ip6_config:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingIP6Config the connection might contain.
 *
 * Note that it returns the value as type #NMSettingIPConfig, since the vast
 * majority of IPv6-setting-related methods are on that type, not
 * #NMSettingIP6Config.
 *
 * Returns: (type NMSettingIP6Config) (transfer none): an #NMSettingIP6Config if the
 * connection contains one, otherwise %NULL
 **/
NMSettingIPConfig *
nm_connection_get_setting_ip6_config(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_IP6_CONFIG);
}

/**
 * nm_connection_get_setting_macsec:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingMacsec the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingMacsec if the connection contains one, otherwise %NULL
 *
 * Since: 1.6
 **/
NMSettingMacsec *
nm_connection_get_setting_macsec(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_MACSEC);
}

/**
 * nm_connection_get_setting_macvlan:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingMacvlan the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingMacvlan if the connection contains one, otherwise %NULL
 *
 * Since: 1.2
 **/
NMSettingMacvlan *
nm_connection_get_setting_macvlan(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_MACVLAN);
}

/**
 * nm_connection_get_setting_olpc_mesh:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingOlpcMesh the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingOlpcMesh if the connection contains one, otherwise %NULL
 **/
NMSettingOlpcMesh *
nm_connection_get_setting_olpc_mesh(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_OLPC_MESH);
}

/**
 * nm_connection_get_setting_ovs_bridge:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingOvsBridge the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingOvsBridge if the connection contains one, otherwise %NULL
 *
 * Since: 1.14
 **/
NMSettingOvsBridge *
nm_connection_get_setting_ovs_bridge(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_OVS_BRIDGE);
}

/**
 * nm_connection_get_setting_ovs_interface:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingOvsInterface the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingOvsInterface if the connection contains one, otherwise %NULL
 *
 * Since: 1.14
 **/
NMSettingOvsInterface *
nm_connection_get_setting_ovs_interface(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_OVS_INTERFACE);
}

/**
 * nm_connection_get_setting_ovs_patch:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingOvsPatch the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingOvsPatch if the connection contains one, otherwise %NULL
 *
 * Since: 1.14
 **/
NMSettingOvsPatch *
nm_connection_get_setting_ovs_patch(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_OVS_PATCH);
}

/**
 * nm_connection_get_setting_ovs_port:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingOvsPort the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingOvsPort if the connection contains one, otherwise %NULL
 *
 * Since: 1.14
 **/
NMSettingOvsPort *
nm_connection_get_setting_ovs_port(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_OVS_PORT);
}

/**
 * nm_connection_get_setting_ppp:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingPpp the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingPpp if the connection contains one, otherwise %NULL
 **/
NMSettingPpp *
nm_connection_get_setting_ppp(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_PPP);
}

/**
 * nm_connection_get_setting_pppoe:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingPppoe the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingPppoe if the connection contains one, otherwise %NULL
 **/
NMSettingPppoe *
nm_connection_get_setting_pppoe(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_PPPOE);
}

/**
 * nm_connection_get_setting_proxy:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingProxy the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingProxy if the connection contains one, otherwise %NULL
 *
 * Since: 1.6
 **/
NMSettingProxy *
nm_connection_get_setting_proxy(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_PROXY);
}

/**
 * nm_connection_get_setting_serial:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingSerial the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingSerial if the connection contains one, otherwise %NULL
 **/
NMSettingSerial *
nm_connection_get_setting_serial(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_SERIAL);
}

/**
 * nm_connection_get_setting_tc_config:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingTCConfig the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingTCConfig if the connection contains one, otherwise %NULL
 *
 * Since: 1.12
 **/
NMSettingTCConfig *
nm_connection_get_setting_tc_config(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_TC_CONFIG);
}

/**
 * nm_connection_get_setting_tun:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingTun the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingTun if the connection contains one, otherwise %NULL
 *
 * Since: 1.14
 **/
NMSettingTun *
nm_connection_get_setting_tun(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_TUN);
}

/**
 * nm_connection_get_setting_vpn:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingVpn the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingVpn if the connection contains one, otherwise %NULL
 **/
NMSettingVpn *
nm_connection_get_setting_vpn(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_VPN);
}

/**
 * nm_connection_get_setting_vxlan:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingVxlan the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingVxlan if the connection contains one, otherwise %NULL
 *
 * Since: 1.2
 **/
NMSettingVxlan *
nm_connection_get_setting_vxlan(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_VXLAN);
}

/**
 * nm_connection_get_setting_wimax:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingWimax the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingWimax if the connection contains one, otherwise %NULL
 **/
NMSettingWimax *
nm_connection_get_setting_wimax(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_WIMAX);
}

/**
 * nm_connection_get_setting_wired:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingWired the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingWired if the connection contains one, otherwise %NULL
 **/
NMSettingWired *
nm_connection_get_setting_wired(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_WIRED);
}

/**
 * nm_connection_get_setting_adsl:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingAdsl the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingAdsl if the connection contains one, otherwise %NULL
 **/
NMSettingAdsl *
nm_connection_get_setting_adsl(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_ADSL);
}

/**
 * nm_connection_get_setting_wireless:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingWireless the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingWireless if the connection contains one, otherwise %NULL
 **/
NMSettingWireless *
nm_connection_get_setting_wireless(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_WIRELESS);
}

/**
 * nm_connection_get_setting_wireless_security:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingWirelessSecurity the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingWirelessSecurity if the connection contains one, otherwise %NULL
 **/
NMSettingWirelessSecurity *
nm_connection_get_setting_wireless_security(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection,
                                                  NM_META_SETTING_TYPE_WIRELESS_SECURITY);
}

/**
 * nm_connection_get_setting_bridge_port:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingBridgePort the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingBridgePort if the connection contains one, otherwise %NULL
 **/
NMSettingBridgePort *
nm_connection_get_setting_bridge_port(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_BRIDGE_PORT);
}

/**
 * nm_connection_get_setting_vlan:
 * @connection: the #NMConnection
 *
 * A shortcut to return any #NMSettingVlan the connection might contain.
 *
 * Returns: (transfer none): an #NMSettingVlan if the connection contains one, otherwise %NULL
 **/
NMSettingVlan *
nm_connection_get_setting_vlan(NMConnection *connection)
{
    return _nm_connection_get_setting_by_metatype(connection, NM_META_SETTING_TYPE_VLAN);
}

NMSettingBluetooth *
_nm_connection_get_setting_bluetooth_for_nap(NMConnection *connection)
{
    NMSettingBluetooth *s_bt = nm_connection_get_setting_bluetooth(connection);

    if (s_bt
        && nm_streq0(nm_setting_bluetooth_get_connection_type(s_bt), NM_SETTING_BLUETOOTH_TYPE_NAP))
        return s_bt;
    return NULL;
}

/*****************************************************************************/

static void
nm_connection_default_init(NMConnectionInterface *iface)
{
    /**
     * NMConnection::secrets-updated:
     * @connection: the object on which the signal is emitted
     * @setting_name: the setting name of the #NMSetting for which secrets were
     * updated
     *
     * The ::secrets-updated signal is emitted when the secrets of a setting
     * have been changed.
     */
    signals[SECRETS_UPDATED] = g_signal_new(NM_CONNECTION_SECRETS_UPDATED,
                                            NM_TYPE_CONNECTION,
                                            G_SIGNAL_RUN_FIRST,
                                            G_STRUCT_OFFSET(NMConnectionInterface, secrets_updated),
                                            NULL,
                                            NULL,
                                            g_cclosure_marshal_VOID__STRING,
                                            G_TYPE_NONE,
                                            1,
                                            G_TYPE_STRING);

    /**
     * NMConnection::secrets-cleared:
     * @connection: the object on which the signal is emitted
     *
     * The ::secrets-cleared signal is emitted when the secrets of a connection
     * are cleared.
     */
    signals[SECRETS_CLEARED] = g_signal_new(NM_CONNECTION_SECRETS_CLEARED,
                                            NM_TYPE_CONNECTION,
                                            G_SIGNAL_RUN_FIRST,
                                            G_STRUCT_OFFSET(NMConnectionInterface, secrets_cleared),
                                            NULL,
                                            NULL,
                                            g_cclosure_marshal_VOID__VOID,
                                            G_TYPE_NONE,
                                            0);

    /**
     * NMConnection::changed:
     * @connection: the object on which the signal is emitted
     *
     * The ::changed signal is emitted when any property (including secrets)
     * of any setting of the connection is modified, or when settings are
     * added or removed.
     */
    signals[CHANGED] = g_signal_new(NM_CONNECTION_CHANGED,
                                    NM_TYPE_CONNECTION,
                                    G_SIGNAL_RUN_FIRST,
                                    G_STRUCT_OFFSET(NMConnectionInterface, changed),
                                    NULL,
                                    NULL,
                                    g_cclosure_marshal_VOID__VOID,
                                    G_TYPE_NONE,
                                    0);
}
