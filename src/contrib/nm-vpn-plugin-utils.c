/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2016, 2018 Red Hat, Inc.
 */

#include "libnm-client-aux-extern/nm-default-client.h"

#include "nm-vpn-plugin-utils.h"

#include <dlfcn.h>

/*****************************************************************************/

char *
nm_vpn_plugin_utils_get_editor_module_path(const char *module_name, GError **error)
{
    gs_free char *module_path = NULL;
    gs_free char *dirname     = NULL;
    Dl_info       plugin_info;

    g_return_val_if_fail(module_name, NULL);
    g_return_val_if_fail(!error || !*error, NULL);

    /*
     * Look for the editor from the same directory this plugin is in.
     * Ideally, we'd get our .so name from the NMVpnEditorPlugin if it
     * would just have a property with it...
     */
    if (!dladdr(nm_vpn_plugin_utils_load_editor, &plugin_info)) {
        /* Really a "can not happen" scenario. */
        g_set_error(error,
                    NM_VPN_PLUGIN_ERROR,
                    NM_VPN_PLUGIN_ERROR_FAILED,
                    _("unable to get editor plugin name: %s"),
                    dlerror());
    }

    dirname     = g_path_get_dirname(plugin_info.dli_fname);
    module_path = g_build_filename(dirname, module_name, NULL);

    if (!g_file_test(module_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_NOENT,
                    _("missing plugin file \"%s\""),
                    module_path);
        return NULL;
    }

    return g_steal_pointer(&module_path);
}

NMVpnEditor *
nm_vpn_plugin_utils_load_editor(const char                   *module_path,
                                const char                   *factory_name,
                                NMVpnPluginUtilsEditorFactory editor_factory,
                                NMVpnEditorPlugin            *editor_plugin,
                                NMConnection                 *connection,
                                gpointer                      user_data,
                                GError                      **error)

{
    gs_free char *compat_module_path = NULL;
    static struct {
        gpointer factory;
        void    *dl_module;
        char    *module_path;
        char    *factory_name;
    } cached = {0};
    NMVpnEditor *editor;

    g_return_val_if_fail(module_path, NULL);
    g_return_val_if_fail(factory_name && factory_name[0], NULL);
    g_return_val_if_fail(editor_factory, NULL);
    g_return_val_if_fail(NM_IS_VPN_EDITOR_PLUGIN(editor_plugin), NULL);
    g_return_val_if_fail(NM_IS_CONNECTION(connection), NULL);
    g_return_val_if_fail(!error || !*error, NULL);

    if (!g_path_is_absolute(module_path)) {
        /* This presumably means the VPN plugin factory() didn't verify that the plugin is there.
         * Now it might be too late to do so. */
        g_warning("VPN plugin bug: load_editor() argument not an absolute path. Continuing...");
        compat_module_path = nm_vpn_plugin_utils_get_editor_module_path(module_path, error);
        if (compat_module_path == NULL)
            return NULL;
        else
            module_path = compat_module_path;
    }

    /* we really expect this function to be called with unchanging @module_path
     * and @factory_name. And we only want to load the module once, hence it would
     * be more complicated to accept changing @module_path/@factory_name arguments.
     *
     * The reason for only loading once is that due to glib types, we cannot create a
     * certain type-name more then once, so loading the same module or another version
     * of the same module will fail horribly as both try to create a GType with the same
     * name.
     *
     * Only support loading once, any future calls will reuse the handle. To simplify
     * that, we enforce that the @factory_name and @module_path is the same. */
    if (cached.factory) {
        g_return_val_if_fail(cached.dl_module, NULL);
        g_return_val_if_fail(cached.factory_name && nm_streq0(cached.factory_name, factory_name),
                             NULL);
        g_return_val_if_fail(cached.module_path && nm_streq0(cached.module_path, module_path),
                             NULL);
    } else {
        gpointer factory;
        void    *dl_module;

        dl_module = dlopen(module_path, RTLD_LAZY | RTLD_LOCAL);
        if (!dl_module) {
            g_set_error(error,
                        NM_VPN_PLUGIN_ERROR,
                        NM_VPN_PLUGIN_ERROR_FAILED,
                        _("cannot load editor plugin: %s"),
                        dlerror());
            return NULL;
        }

        factory = dlsym(dl_module, factory_name);
        if (!factory) {
            g_set_error(error,
                        NM_VPN_PLUGIN_ERROR,
                        NM_VPN_PLUGIN_ERROR_FAILED,
                        _("cannot load factory %s from plugin: %s"),
                        factory_name,
                        dlerror());
            dlclose(dl_module);
            return NULL;
        }

        /* we cannot ever unload the module because it creates glib types, which
         * cannot be unregistered.
         *
         * Thus we just leak the dl_module handle indefinitely. */
        cached.factory      = factory;
        cached.dl_module    = dl_module;
        cached.module_path  = g_strdup(module_path);
        cached.factory_name = g_strdup(factory_name);
    }

    editor = editor_factory(cached.factory, editor_plugin, connection, user_data, error);
    if (!editor) {
        if (error && !*error) {
            g_set_error_literal(error,
                                NM_VPN_PLUGIN_ERROR,
                                NM_VPN_PLUGIN_ERROR_FAILED,
                                _("unknown error creating editor instance"));
            g_return_val_if_reached(NULL);
        }
        return NULL;
    }

    g_return_val_if_fail(NM_IS_VPN_EDITOR(editor), NULL);
    return editor;
}

char *
nm_vpn_plugin_utils_get_cert_path(const char *plugin)
{
    const char *path;

    g_return_val_if_fail(plugin, NULL);

    /* Users can set NM_CERT_PATH=~/.cert to be compatible with the certificate
     * directory used in the past. */
    path = g_getenv("NM_CERT_PATH");
    if (path)
        return g_build_filename(path, plugin, NULL);

    /* Otherwise use XDG_DATA_HOME. We use subdirectory "networkmanagement/certificates"
     * because the SELinux policy already has rules to set the correct labels in that
     * directory. */
    path = g_getenv("XDG_DATA_HOME");
    if (path)
        return g_build_filename(path, "networkmanagement", "certificates", plugin, NULL);

    /* Use the default value for XDG_DATA_HOME */
    return g_build_filename(g_get_home_dir(),
                            ".local",
                            "share",
                            "networkmanagement",
                            "certificates",
                            plugin,
                            NULL);
}
