/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2014 Red Hat, Inc.
 */

#ifndef __NM_CONNECTION_PRIVATE_H__
#define __NM_CONNECTION_PRIVATE_H__

#if !((NETWORKMANAGER_COMPILATION) & NM_NETWORKMANAGER_COMPILATION_WITH_LIBNM_CORE_PRIVATE)
#error Cannot use this header.
#endif

#include "nm-setting.h"
#include "nm-connection.h"

NMSetting *_nm_connection_find_base_type_setting(NMConnection *connection);

const char *_nm_connection_detect_port_type(NMConnection *connection, NMSetting **out_s_port);

gboolean _nm_connection_detect_port_type_full(NMSettingConnection *s_con,
                                              NMConnection        *connection,
                                              const char         **out_port_type,
                                              const char         **out_normerr_port_setting_type,
                                              const char         **out_normerr_missing_port_type,
                                              const char **out_normerr_missing_port_type_port,
                                              GError     **error);

const char *_nm_connection_detect_bluetooth_type(NMConnection *self);

gboolean _nm_setting_connection_verify_secondaries(GArray *secondaries, GError **error);

gboolean _nm_setting_connection_verify_no_duplicate_addresses(GArray *secondaries, GError **error);

int _get_ip_address_family(const char *ip_address);

gboolean _nm_connection_verify_required_interface_name(NMConnection *connection, GError **error);

int _nm_setting_ovs_interface_verify_interface_type(NMSettingOvsInterface *self,
                                                    const char            *type,
                                                    NMConnection          *connection,
                                                    gboolean               normalize,
                                                    gboolean              *out_modified,
                                                    const char           **out_normalized_type,
                                                    GError               **error);

#endif /* __NM_CONNECTION_PRIVATE_H__ */
