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
#include "rpmostree-rpm-util.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

gboolean
rpmostree_composeutil_checksum (HyGoal             goal,
                                RORTreefile       *tf,
                                JsonObject        *treefile,
                                char             **out_checksum,
                                GError           **error)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  /* Hash in the raw treefile; this means reordering the input packages
   * or adding a comment will cause a recompose, but let's be conservative
   * here.
   */
  g_checksum_update (checksum, (guint8*)ror_treefile_get_json_string (tf), -1);

  if (json_object_has_member (treefile, "add-files"))
    {
      JsonArray *add_files = json_object_get_array_member (treefile, "add-files");
      guint i, len = json_array_get_length (add_files);
      for (i = 0; i < len; i++)
        {
          JsonArray *add_el = json_array_get_array_element (add_files, i);
          if (!add_el)
            return glnx_throw (error, "Element in add-files is not an array");
          const char *src = _rpmostree_jsonutil_array_require_string_element (add_el, 0, error);
          if (!src)
            return FALSE;

          int src_fd = ror_treefile_get_add_file_fd (tf, src);
          g_assert_cmpint (src_fd, !=, -1);
          g_autoptr(GBytes) bytes = glnx_fd_readall_bytes (src_fd, NULL, FALSE);
          gsize len;
          const guint8* buf = g_bytes_get_data (bytes, &len);
          g_checksum_update (checksum, (const guint8 *) buf, len);
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

  { GLNX_AUTO_PREFIX_ERROR ("Testing /dev/null in target root (is nodev set?)", error);
    glnx_autofd int devnull_fd = -1;
    if (!glnx_openat_rdonly (dest_fd, "null", TRUE, &devnull_fd, error))
      return FALSE;
    char buf[1];
    ssize_t s = read (devnull_fd, buf, sizeof (buf));
    if (s < 0)
      return glnx_throw_errno_prefix (error, "read");
  }

  return TRUE;
}


gboolean
rpmostree_composeutil_sanity_checks (RORTreefile  *tf,
                                     JsonObject   *treefile,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  int fd = ror_treefile_get_postprocess_script_fd (tf);
  if (fd != -1)
    {
      /* Check that postprocess-script is executable; https://github.com/projectatomic/rpm-ostree/issues/817 */
      struct stat stbuf;
      if (!glnx_fstat (fd, &stbuf, error))
        return glnx_prefix_error (error, "postprocess-script");

      if ((stbuf.st_mode & S_IXUSR) == 0)
        return glnx_throw (error, "postprocess-script must be executable");
    }

  /* Insert other sanity checks here */

  return TRUE;
}

static gboolean
set_keyfile_string_array_from_json (GKeyFile    *keyfile,
                                    const char  *keyfile_group,
                                    const char  *keyfile_key,
                                    JsonArray   *a,
                                    GError     **error)
{
  g_autoptr(GPtrArray) instlangs_v = g_ptr_array_new ();

  guint len = json_array_get_length (a);
  for (guint i = 0; i < len; i++)
    {
      const char *elt = _rpmostree_jsonutil_array_require_string_element (a, i, error);

      if (!elt)
        return FALSE;

      g_ptr_array_add (instlangs_v, (char*)elt);
    }

  g_key_file_set_string_list (keyfile, keyfile_group, keyfile_key,
                              (const char*const*)instlangs_v->pdata, instlangs_v->len);

  return TRUE;
}

static gboolean
treespec_bind_array (JsonObject *treedata,
                     GKeyFile   *ts,
                     const char *src_name,
                     const char *dest_name,
                     gboolean    required,
                     GError    **error)
{
  if (!json_object_has_member (treedata, src_name))
    {
      if (required)
        return glnx_throw (error, "Treefile is missing required \"%s\" member", src_name);
      return TRUE;
    }
  JsonArray *a = json_object_get_array_member (treedata, src_name);
  g_assert (a);
  return set_keyfile_string_array_from_json (ts, "tree", dest_name ?: src_name, a, error);
}

/* Given a boolean value in JSON, add it to treespec
 * if it's not the default.
 */
static gboolean
treespec_bind_bool (JsonObject *treedata,
                    GKeyFile   *ts,
                    const char *name,
                    gboolean    default_value,
                    GError    **error)
{
  gboolean v = default_value;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata, name, &v, error))
    return FALSE;

  if (v != default_value)
    g_key_file_set_boolean (ts, "tree", name, v);

  return TRUE;
}

/* Convert a treefile into a "treespec" understood by the core.
 */
RpmOstreeTreespec *
rpmostree_composeutil_get_treespec (RpmOstreeContext  *ctx,
                                    RORTreefile *treefile_rs,
                                    JsonObject  *treedata,
                                    GError     **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Parsing treefile", error);
  g_autoptr(GHashTable) varsubsts = rpmostree_dnfcontext_get_varsubsts (rpmostree_context_get_dnf (ctx));
  g_autoptr(GKeyFile) treespec = g_key_file_new ();

  if (!treespec_bind_array (treedata, treespec, "packages", NULL, TRUE, error))
    return FALSE;
  if (!treespec_bind_array (treedata, treespec, "repos", NULL, TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "documentation", TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "recommends", TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "selinux", TRUE, error))
    return FALSE;
  if (!treespec_bind_array (treedata, treespec, "install-langs", "instlangs", FALSE, error))
    return FALSE;
  { const char *releasever;
    if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "releasever",
                                                                &releasever, error))
      return FALSE;
    if (releasever)
      g_key_file_set_string (treespec, "tree", "releasever", releasever);
  }

  const char *input_ref = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "ref", &input_ref, error))
    return FALSE;
  if (input_ref)
    {
      g_autofree char *ref = _rpmostree_varsubst_string (input_ref, varsubsts, error);
      if (!ref)
        return FALSE;
      g_key_file_set_string (treespec, "tree", "ref", ref);
    }

  return rpmostree_treespec_new_from_keyfile (treespec, error);
}

/* compose tree accepts JSON metadata via file; convert it
 * to a hash table of a{sv}; suitable for further extension.
 */
GHashTable *
rpmostree_composeutil_read_json_metadata (const char *path,
                                          GError    **error)
{
  g_autoptr(GHashTable) metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  if (path)
    {
      glnx_unref_object JsonParser *jparser = json_parser_new ();
      if (!json_parser_load_from_file (jparser, path, error))
        return FALSE;

      JsonNode *metarootval = json_parser_get_root (jparser);
      g_autoptr(GVariant) jsonmetav = json_gvariant_deserialize (metarootval, "a{sv}", error);
      if (!jsonmetav)
        {
          g_prefix_error (error, "Parsing %s: ", path);
          return FALSE;
        }

      GVariantIter viter;
      g_variant_iter_init (&viter, jsonmetav);
      { char *key;
        GVariant *value;
        while (g_variant_iter_loop (&viter, "{sv}", &key, &value))
          g_hash_table_replace (metadata, g_strdup (key), g_variant_ref (value));
      }
    }

  return g_steal_pointer (&metadata);
}

/* Convert hash table of metadata into finalized GVariant */
GVariant *
rpmostree_composeutil_finalize_metadata (GHashTable *metadata,
                                         int         rootfs_dfd,
                                         GError    **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  GLNX_HASH_TABLE_FOREACH_KV (metadata, const char*, strkey, GVariant*, v)
    g_variant_builder_add (metadata_builder, "{sv}", strkey, v);

  /* include list of packages in rpmdb; this is used client-side for easily previewing
   * pending updates. once we only support unified core composes, this can easily be much
   * more readily injected during assembly */
  g_autoptr(GVariant) rpmdb_v = NULL;
  if (!rpmostree_create_rpmdb_pkglist_variant (rootfs_dfd, ".", &rpmdb_v,
                                               NULL, error))
    return FALSE;
  g_variant_builder_add (metadata_builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb_v);

  g_autoptr(GVariant) ret = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
  /* Canonicalize to big endian, like OSTree does. Without this, any numbers
   * we place in the metadata will be unreadable since clients won't know
   * their endianness.
   */
  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      GVariant *swapped = g_variant_byteswap (ret);
      GVariant *orig = ret;
      ret = swapped;
      g_variant_unref (orig);
    }

  return g_steal_pointer (&ret);
}
