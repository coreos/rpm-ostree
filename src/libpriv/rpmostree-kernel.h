/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014,2017 Colin Walters <walters@verbum.org>
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

#include <ostree.h>

G_BEGIN_DECLS

typedef enum {
  RPMOSTREE_FINALIZE_KERNEL_AUTO,
  RPMOSTREE_FINALIZE_KERNEL_USRLIB_MODULES,
  RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT,
  RPMOSTREE_FINALIZE_KERNEL_SLASH_BOOT,
} RpmOstreeFinalizeKernelDestination;

GVariant *
rpmostree_find_kernel (int rootfs_dfd,
                       GCancellable *cancellable,
                       GError **error);

gboolean
rpmostree_kernel_remove (int rootfs_dfd,
                         GCancellable *cancellable,
                         GError **error);

gboolean
rpmostree_finalize_kernel (int rootfs_dfd,
                           const char *bootdir,
                           const char *kver,
                           const char *kernel_path,
                           GLnxTmpfile *initramfs_tmpf,
                           RpmOstreeFinalizeKernelDestination dest,
                           GCancellable *cancellable,
                           GError **error);

gboolean
rpmostree_run_dracut (int     rootfs_dfd,
                      const char *const* argv,
                      const char *kver,
                      const char *rebuild_from_initramfs,
                      gboolean     use_root_etc,
                      GLnxTmpDir  *dracut_host_tmpdir,
                      GLnxTmpfile *out_initramfs_tmpf,
                      GCancellable  *cancellable,
                      GError **error);

G_END_DECLS