/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Red Hat, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-audit-manager.h"

#if HAVE_LIBAUDIT
#include <libaudit.h>
#endif

#define NM_VALUE_TYPE_DEFINE_FUNCTIONS

#include "libnm-core-aux-intern/nm-auth-subject.h"
#include "libnm-glib-aux/nm-str-buf.h"
#include "libnm-glib-aux/nm-value-type.h"
#include "nm-config.h"
#include "nm-dbus-manager.h"
#include "settings/nm-settings-connection.h"

/*****************************************************************************/

typedef enum _nm_packed {
    BACKEND_LOG    = (1 << 0),
    BACKEND_AUDITD = (1 << 1),
    _BACKEND_LAST,
    BACKEND_ALL = ((_BACKEND_LAST - 1) << 1) - 1,
} AuditBackend;

typedef struct {
    const char     *name;
    AuditBackend    backends;
    bool            need_encoding;
    NMValueType     value_type;
    NMValueTypUnion value;
} AuditField;

/*****************************************************************************/

typedef struct {
    NMConfig *config;
    int       auditd_fd;
} NMAuditManagerPrivate;

struct _NMAuditManager {
    GObject parent;
#if HAVE_LIBAUDIT
    NMAuditManagerPrivate _priv;
#endif
};

struct _NMAuditManagerClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMAuditManager, nm_audit_manager, G_TYPE_OBJECT)

#define NM_AUDIT_MANAGER_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMAuditManager, NM_IS_AUDIT_MANAGER)

/*****************************************************************************/

#define AUDIT_LOG_LEVEL LOGL_INFO

#define _NMLOG_PREFIX_NAME "audit"
#define _NMLOG(level, domain, ...)                                         \
    G_STMT_START                                                           \
    {                                                                      \
        nm_log((level),                                                    \
               (domain),                                                   \
               NULL,                                                       \
               NULL,                                                       \
               "%s" _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                    \
               _NMLOG_PREFIX_NAME ": " _NM_UTILS_MACRO_REST(__VA_ARGS__)); \
    }                                                                      \
    G_STMT_END

/*****************************************************************************/

NM_DEFINE_SINGLETON_GETTER(NMAuditManager, nm_audit_manager_get, NM_TYPE_AUDIT_MANAGER);

/*****************************************************************************/

static void
_audit_field_init_string(AuditField  *field,
                         const char  *name,
                         const char  *str,
                         gboolean     need_encoding,
                         AuditBackend backends)
{
    *field = (AuditField) {
        .name           = name,
        .need_encoding  = need_encoding,
        .backends       = backends,
        .value_type     = NM_VALUE_TYPE_STRING,
        .value.v_string = str,
    };
}

static void
_audit_field_init_uint64(AuditField *field, const char *name, guint64 val, AuditBackend backends)
{
    *field = (AuditField) {
        .name           = name,
        .backends       = backends,
        .value_type     = NM_VALUE_TYPE_UINT64,
        .value.v_uint64 = val,
    };
}

static const char *
build_message(NMStrBuf *strbuf, AuditBackend backend, GPtrArray *fields)
{
    guint i;

    if (strbuf->len == 0) {
        /* preallocate a large buffer... */
        nm_str_buf_maybe_expand(strbuf, NM_UTILS_GET_NEXT_REALLOC_SIZE_232, FALSE);
    } else
        nm_str_buf_reset(strbuf);

    for (i = 0; i < fields->len; i++) {
        const AuditField *field = fields->pdata[i];

        if (!NM_FLAGS_ANY(field->backends, backend))
            continue;

        nm_str_buf_append_required_delimiter(strbuf, ' ');

        if (field->value_type == NM_VALUE_TYPE_STRING) {
            const char *str = field->value.v_string;

#if HAVE_LIBAUDIT
            if (backend == BACKEND_AUDITD) {
                if (field->need_encoding) {
                    nm_auto_free char *value = NULL;

                    value = audit_encode_nv_string(field->name, str, 0);
                    if (value)
                        nm_str_buf_append(strbuf, value);
                    else
                        nm_str_buf_append_printf(strbuf, "%s=???", field->name);
                } else
                    nm_str_buf_append_printf(strbuf, "%s=%s", field->name, str);
                continue;
            }
#endif /* HAVE_LIBAUDIT */

            nm_str_buf_append_printf(strbuf, "%s=\"%s\"", field->name, str);
            continue;
        }

        if (field->value_type == NM_VALUE_TYPE_UINT64) {
            nm_str_buf_append_printf(strbuf,
                                     "%s=%" G_GUINT64_FORMAT,
                                     field->name,
                                     field->value.v_uint64);
            continue;
        }

        g_return_val_if_reached(NULL);
    }

    return nm_str_buf_get_str(strbuf);
}

static void
nm_audit_log(NMAuditManager *self,
             GPtrArray      *fields,
             const char     *file,
             guint           line,
             const char     *func,
             gboolean        success)
{
    nm_auto_str_buf NMStrBuf strbuf = NM_STR_BUF_INIT(0, FALSE);
#if HAVE_LIBAUDIT
    NMAuditManagerPrivate *priv;
#endif

    g_return_if_fail(NM_IS_AUDIT_MANAGER(self));

#if HAVE_LIBAUDIT
    priv = NM_AUDIT_MANAGER_GET_PRIVATE(self);

    if (priv->auditd_fd >= 0) {
        int r;

        r = audit_log_user_message(priv->auditd_fd,
                                   AUDIT_USYS_CONFIG,
                                   build_message(&strbuf, BACKEND_AUDITD, fields),
                                   NULL,
                                   NULL,
                                   NULL,
                                   success);
        (void) r;
    }
#endif

    if (nm_logging_enabled(AUDIT_LOG_LEVEL, LOGD_AUDIT)) {
        _nm_log_full(file,
                     line,
                     func,
                     !(NM_THREAD_SAFE_ON_MAIN_THREAD),
                     AUDIT_LOG_LEVEL,
                     LOGD_AUDIT,
                     0,
                     NULL,
                     NULL,
                     "%s%s",
                     _NMLOG_PREFIX_NAME ": ",
                     build_message(&strbuf, BACKEND_LOG, fields));
    }
}

static void
_audit_log_helper(NMAuditManager *self,
                  GPtrArray      *fields,
                  const char     *file,
                  guint           line,
                  const char     *func,
                  const char     *op,
                  gboolean        result,
                  gpointer        subject_context,
                  const char     *reason)
{
    AuditField                     op_field;
    AuditField                     pid_field;
    AuditField                     uid_field;
    AuditField                     result_field;
    AuditField                     reason_field;
    gulong                         pid;
    gulong                         uid;
    NMAuthSubject                 *subject      = NULL;
    gs_unref_object NMAuthSubject *subject_free = NULL;

    _audit_field_init_string(&op_field, "op", op, FALSE, BACKEND_ALL);
    g_ptr_array_insert(fields, 0, &op_field);

    if (subject_context) {
        if (NM_IS_AUTH_SUBJECT(subject_context))
            subject = subject_context;
        else if (G_IS_DBUS_METHOD_INVOCATION(subject_context)) {
            GDBusMethodInvocation *context = subject_context;

            subject = subject_free = nm_dbus_manager_new_auth_subject_from_context(context);
        } else
            g_warn_if_reached();
    }
    if (subject && nm_auth_subject_get_subject_type(subject) == NM_AUTH_SUBJECT_TYPE_UNIX_PROCESS) {
        pid = nm_auth_subject_get_unix_process_pid(subject);
        uid = nm_auth_subject_get_unix_process_uid(subject);
        if (pid != G_MAXULONG) {
            _audit_field_init_uint64(&pid_field, "pid", pid, BACKEND_ALL);
            g_ptr_array_add(fields, &pid_field);
        }
        if (uid != G_MAXULONG) {
            _audit_field_init_uint64(&uid_field, "uid", uid, BACKEND_ALL);
            g_ptr_array_add(fields, &uid_field);
        }
    }

    _audit_field_init_string(&result_field,
                             "result",
                             result ? "success" : "fail",
                             FALSE,
                             BACKEND_ALL);
    g_ptr_array_add(fields, &result_field);

    if (reason) {
        _audit_field_init_string(&reason_field, "reason", reason, FALSE, BACKEND_LOG);
        g_ptr_array_add(fields, &reason_field);
    }

    nm_audit_log(self, fields, file, line, func, result);
}

gboolean
nm_audit_manager_audit_enabled(NMAuditManager *self)
{
#if HAVE_LIBAUDIT
    NMAuditManagerPrivate *priv = NM_AUDIT_MANAGER_GET_PRIVATE(self);

    if (priv->auditd_fd >= 0)
        return TRUE;
#endif

    return nm_logging_enabled(AUDIT_LOG_LEVEL, LOGD_AUDIT);
}

void
_nm_audit_manager_log_connection_op(NMAuditManager       *self,
                                    const char           *file,
                                    guint                 line,
                                    const char           *func,
                                    const char           *op,
                                    NMSettingsConnection *connection,
                                    gboolean              result,
                                    const char           *args,
                                    gpointer              subject_context,
                                    const char           *reason)
{
    gs_unref_ptrarray GPtrArray *fields = NULL;
    AuditField                   uuid_field;
    AuditField                   name_field;
    AuditField                   args_field;

    g_return_if_fail(op);

    fields = g_ptr_array_new();

    if (connection) {
        _audit_field_init_string(&uuid_field,
                                 "uuid",
                                 nm_settings_connection_get_uuid(connection),
                                 FALSE,
                                 BACKEND_ALL);
        g_ptr_array_add(fields, &uuid_field);

        _audit_field_init_string(&name_field,
                                 "name",
                                 nm_settings_connection_get_id(connection),
                                 TRUE,
                                 BACKEND_ALL);
        g_ptr_array_add(fields, &name_field);
    }

    if (args) {
        _audit_field_init_string(&args_field, "args", args, FALSE, BACKEND_ALL);
        g_ptr_array_add(fields, &args_field);
    }

    _audit_log_helper(self, fields, file, line, func, op, result, subject_context, reason);
}

void
_nm_audit_manager_log_generic_op(NMAuditManager *self,
                                 const char     *file,
                                 guint           line,
                                 const char     *func,
                                 const char     *op,
                                 const char     *arg,
                                 gboolean        result,
                                 gpointer        subject_context,
                                 const char     *reason)
{
    gs_unref_ptrarray GPtrArray *fields = NULL;
    AuditField                   arg_field;

    g_return_if_fail(op);
    g_return_if_fail(arg);

    fields = g_ptr_array_new();

    _audit_field_init_string(&arg_field, "arg", arg, TRUE, BACKEND_ALL);
    g_ptr_array_add(fields, &arg_field);

    _audit_log_helper(self, fields, file, line, func, op, result, subject_context, reason);
}

void
_nm_audit_manager_log_device_op(NMAuditManager *self,
                                const char     *file,
                                guint           line,
                                const char     *func,
                                const char     *op,
                                NMDevice       *device,
                                gboolean        result,
                                const char     *args,
                                gpointer        subject_context,
                                const char     *reason)
{
    gs_unref_ptrarray GPtrArray *fields = NULL;
    AuditField                   interface_field;
    AuditField                   ifindex_field;
    AuditField                   args_field;
    int                          ifindex;

    g_return_if_fail(op);
    g_return_if_fail(device);

    fields = g_ptr_array_new();

    _audit_field_init_string(&interface_field,
                             "interface",
                             nm_device_get_ip_iface(device),
                             TRUE,
                             BACKEND_ALL);
    g_ptr_array_add(fields, &interface_field);

    ifindex = nm_device_get_ip_ifindex(device);
    if (ifindex > 0) {
        _audit_field_init_uint64(&ifindex_field, "ifindex", ifindex, BACKEND_ALL);
        g_ptr_array_add(fields, &ifindex_field);
    }

    if (args) {
        _audit_field_init_string(&args_field, "args", args, FALSE, BACKEND_ALL);
        g_ptr_array_add(fields, &args_field);
    }

    _audit_log_helper(self, fields, file, line, func, op, result, subject_context, reason);
}

#if HAVE_LIBAUDIT
static void
init_auditd(NMAuditManager *self)
{
    NMAuditManagerPrivate *priv = NM_AUDIT_MANAGER_GET_PRIVATE(self);
    NMConfigData          *data = nm_config_get_data(priv->config);
    int                    errsv;

    if (nm_config_data_get_value_boolean(data,
                                         NM_CONFIG_KEYFILE_GROUP_LOGGING,
                                         NM_CONFIG_KEYFILE_KEY_LOGGING_AUDIT,
                                         NM_CONFIG_DEFAULT_LOGGING_AUDIT_BOOL)) {
        if (priv->auditd_fd < 0) {
            priv->auditd_fd = audit_open();
            if (priv->auditd_fd < 0) {
                errsv = errno;
                _LOGE(LOGD_CORE, "failed to open auditd socket: %s", nm_strerror_native(errsv));
            } else
                _LOGD(LOGD_CORE, "socket created");
        }
    } else {
        if (priv->auditd_fd >= 0) {
            audit_close(priv->auditd_fd);
            priv->auditd_fd = -1;
            _LOGD(LOGD_CORE, "socket closed");
        }
    }
}

static void
config_changed_cb(NMConfig           *config,
                  NMConfigData       *config_data,
                  NMConfigChangeFlags changes,
                  NMConfigData       *old_data,
                  NMAuditManager     *self)
{
    if (NM_FLAGS_HAS(changes, NM_CONFIG_CHANGE_VALUES))
        init_auditd(self);
}
#endif

/*****************************************************************************/

static void
nm_audit_manager_init(NMAuditManager *self)
{
#if HAVE_LIBAUDIT
    NMAuditManagerPrivate *priv = NM_AUDIT_MANAGER_GET_PRIVATE(self);

    priv->config = g_object_ref(nm_config_get());
    g_signal_connect(G_OBJECT(priv->config),
                     NM_CONFIG_SIGNAL_CONFIG_CHANGED,
                     G_CALLBACK(config_changed_cb),
                     self);
    priv->auditd_fd = -1;

    init_auditd(self);
#endif
}

static void
dispose(GObject *object)
{
#if HAVE_LIBAUDIT
    NMAuditManager        *self = NM_AUDIT_MANAGER(object);
    NMAuditManagerPrivate *priv = NM_AUDIT_MANAGER_GET_PRIVATE(self);

    if (priv->config) {
        g_signal_handlers_disconnect_by_func(priv->config, config_changed_cb, self);
        g_clear_object(&priv->config);
    }

    if (priv->auditd_fd >= 0) {
        audit_close(priv->auditd_fd);
        priv->auditd_fd = -1;
    }
#endif

    G_OBJECT_CLASS(nm_audit_manager_parent_class)->dispose(object);
}

static void
nm_audit_manager_class_init(NMAuditManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = dispose;
}
