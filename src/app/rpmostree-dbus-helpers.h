/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "rpm-ostreed-generated.h"
#include "rpmostree-builtin-types.h"

#include <stdint.h>
#include <string.h>
#include <ostree.h>

#define BUS_NAME "org.projectatomic.rpmostree1"

#define _cleanup_peer_ __attribute__((cleanup(rpmostree_cleanup_peer)))

void
rpmostree_cleanup_peer                       (GPid *peer_pid);

gboolean
rpmostree_load_sysroot                       (gchar *sysroot,
                                              gboolean force_peer,
                                              GCancellable *cancellable,
                                              RPMOSTreeSysroot **out_sysroot_proxy,
                                              GPid *out_peer_pid,
                                              GError **error);

gboolean
rpmostree_load_os_proxy                      (RPMOSTreeSysroot *sysroot_proxy,
                                              gchar *opt_osname,
                                              GCancellable *cancellable,
                                              RPMOSTreeOS **out_os_proxy,
                                              GError **error);

gboolean
rpmostree_load_os_proxies                    (RPMOSTreeSysroot *sysroot_proxy,
                                              gchar *opt_osname,
                                              GCancellable *cancellable,
                                              RPMOSTreeOS **out_os_proxy,
                                              RPMOSTreeOSExperimental **out_osexperimental_proxy,
                                              GError **error);

gboolean
rpmostree_transaction_connect_active         (RPMOSTreeSysroot *sysroot_proxy,
                                              char                 **out_path,
                                              RPMOSTreeTransaction **out_txn,
                                              GCancellable *cancellable,
                                              GError      **error);

gboolean
rpmostree_transaction_get_response_sync      (RPMOSTreeSysroot *sysroot_proxy,
                                              const char *transaction_address,
                                              GCancellable *cancellable,
                                              GError **error);

gboolean
rpmostree_transaction_client_run             (RpmOstreeCommandInvocation *invocation,
                                              RPMOSTreeSysroot *sysroot_proxy,
                                              RPMOSTreeOS      *os_proxy,
                                              GVariant         *options,
                                              gboolean          exit_unchanged_77,
                                              const char *transaction_address,
                                              GVariant         *previous_deployment,
                                              GCancellable *cancellable,
                                              GError **error);

void
rpmostree_print_signatures                   (GVariant *variant,
                                              const gchar *sep,
                                              gboolean verbose);

void
rpmostree_print_package_diffs                (GVariant *variant);

gboolean
rpmostree_sort_pkgs_strv (const char *const* pkgs,
                          GUnixFDList  *fd_list,
                          GPtrArray   **out_repo_pkgs,
                          GVariant    **out_fd_idxs,
                          GError      **error);

GVariant*
rpmostree_get_options_variant (gboolean reboot,
                               gboolean allow_downgrade,
                               gboolean cache_only,
                               gboolean download_only,
                               gboolean skip_purge,
                               gboolean no_pull_base,
                               gboolean dry_run,
                               gboolean no_overrides);

gboolean
rpmostree_update_deployment (RPMOSTreeOS  *os_proxy,
                             const char   *set_refspec,
                             const char   *set_revision,
                             const char *const* install_pkgs,
                             const char *const* uninstall_pkgs,
                             const char *const* override_replace_pkgs,
                             const char *const* override_remove_pkgs,
                             const char *const* override_reset_pkgs,
                             GVariant     *options,
                             char        **out_transaction_address,
                             GCancellable *cancellable,
                             GError      **error);
