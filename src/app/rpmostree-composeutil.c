/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2018 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <libdnf/dnf-repo.h>
#include <sys/mount.h>
#include <stdio.h>
#include <linux/magic.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-composeutil.h"
#include "rpmostree-util.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

gboolean
rpmostree_composeutil_checksum (GBytes            *serialized_treefile,
                                HyGoal             goal,
                                GFile             *contextdir,
                                JsonArray         *add_files,
                                char             **out_checksum,
                                GError           **error)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  /* Hash in the raw treefile; this means reordering the input packages
   * or adding a comment will cause a recompose, but let's be conservative
   * here.
   */
  { gsize len;
    const guint8* buf = g_bytes_get_data (serialized_treefile, &len);

    g_checksum_update (checksum, buf, len);
  }

  if (add_files)
    {
      guint i, len = json_array_get_length (add_files);
      for (i = 0; i < len; i++)
        {
          g_autoptr(GFile) srcfile = NULL;
          const char *src, *dest;
          JsonArray *add_el = json_array_get_array_element (add_files, i);

          if (!add_el)
            return glnx_throw (error, "Element in add-files is not an array");

          src = _rpmostree_jsonutil_array_require_string_element (add_el, 0, error);
          if (!src)
            return FALSE;

          dest = _rpmostree_jsonutil_array_require_string_element (add_el, 1, error);
          if (!dest)
            return FALSE;

          srcfile = g_file_resolve_relative_path (contextdir, src);

          if (!_rpmostree_util_update_checksum_from_file (checksum,
                                                          AT_FDCWD,
                                                          gs_file_get_path_cached (srcfile),
                                                          NULL,
                                                          error))
            return FALSE;

          g_checksum_update (checksum, (const guint8 *) dest, strlen (dest));
        }

    }

  /* FIXME; we should also hash the post script */

  /* Hash in each package */
  if (!rpmostree_dnf_add_checksum_goal (checksum, goal, NULL, error))
    return FALSE;

  *out_checksum = g_strdup (g_checksum_get_string (checksum));
  return TRUE;
}

/* Prepare /dev in the target root with the API devices.  TODO:
 * Delete this when we implement https://github.com/projectatomic/rpm-ostree/issues/729
 */
gboolean
rpmostree_composeutil_legacy_prep_dev (int         rootfs_dfd,
                                       GError    **error)
{

  glnx_autofd int src_fd = openat (AT_FDCWD, "/dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (src_fd == -1)
    return glnx_throw_errno (error);

  if (mkdirat (rootfs_dfd, "dev", 0755) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno (error);
    }

  glnx_autofd int dest_fd = openat (rootfs_dfd, "dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dest_fd == -1)
    return glnx_throw_errno (error);

  static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
  for (guint i = 0; i < G_N_ELEMENTS (devnodes); i++)
    {
      const char *nodename = devnodes[i];
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (src_fd, nodename, &stbuf, 0, error))
        return FALSE;
      if (errno == ENOENT)
        continue;

      if (mknodat (dest_fd, nodename, stbuf.st_mode, stbuf.st_rdev) != 0)
        return glnx_throw_errno (error);
      if (fchmodat (dest_fd, nodename, stbuf.st_mode, 0) != 0)
        return glnx_throw_errno (error);
    }

  return TRUE;
}

gboolean
rpmostree_composeutil_sanity_checks (JsonObject   *treedata,
                                     GFile        *contextdir,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  /* Check that postprocess-script is executable; https://github.com/projectatomic/rpm-ostree/issues/817 */
  const char *postprocess_script = NULL;

  if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "postprocess-script",
                                                              &postprocess_script, error))
    return FALSE;

  if (!postprocess_script)
    return TRUE;

  g_autofree char *src = NULL;
  if (g_path_is_absolute (postprocess_script))
    src = g_strdup (postprocess_script);
  else
    src = g_build_filename (gs_file_get_path_cached (contextdir), postprocess_script, NULL);

  struct stat stbuf;
  if (!glnx_fstatat (AT_FDCWD, src, &stbuf, 0, error))
    return glnx_prefix_error (error, "postprocess-script");

  if ((stbuf.st_mode & S_IXUSR) == 0)
    return glnx_throw (error, "postprocess-script (%s) must be executable", postprocess_script);

  /* Insert other sanity checks here */

  return TRUE;
}
