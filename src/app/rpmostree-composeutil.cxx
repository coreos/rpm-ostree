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

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <libdnf/dnf-repo.h>
#include <libdnf/libdnf.h>
#include <libglnx.h>
#include <linux/magic.h>
#include <rpm/rpmmacro.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

#include "libglnx.h"
#include "rpmostree-composeutil.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"

gboolean
rpmostree_composeutil_checksum (HyGoal goal, OstreeRepo *repo, const rpmostreecxx::Treefile &tf,
                                JsonObject *treefile, char **out_checksum, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Computing compose checksum", error);
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  /* Hash in the treefile inputs (this includes all externals like postprocess, add-files,
   * etc... and the final flattened treefile -- see treefile.rs for more details). */
  CXX_TRY_VAR (tf_checksum, tf.get_checksum (*repo), error);
  g_checksum_update (checksum, (const guint8 *)tf_checksum.data (), tf_checksum.size ());

  /* Hash in each package */
  if (!rpmostree_dnf_add_checksum_goal (checksum, goal, NULL, error))
    return FALSE;

  *out_checksum = g_strdup (g_checksum_get_string (checksum));
  return TRUE;
}

gboolean
rpmostree_composeutil_read_json_metadata (JsonNode *root, GHashTable *metadata, GError **error)
{
  g_autoptr (GVariant) jsonmetav = json_gvariant_deserialize (root, "a{sv}", error);
  if (!jsonmetav)
    return FALSE;

  GVariantIter viter;
  g_variant_iter_init (&viter, jsonmetav);
  {
    char *key;
    GVariant *value;
    while (g_variant_iter_loop (&viter, "{sv}", &key, &value))
      g_hash_table_replace (metadata, g_strdup (key), g_variant_ref (value));
  }

  return TRUE;
}

/* compose tree accepts JSON metadata via file; convert it
 * to a hash table of a{sv}; suitable for further extension.
 */
gboolean
rpmostree_composeutil_read_json_metadata_from_file (const char *path, GHashTable *metadata,
                                                    GError **error)
{
  const char *errprefix = glnx_strjoina ("While parsing JSON file ", path);
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);

  glnx_unref_object JsonParser *jparser = json_parser_new ();
  if (!json_parser_load_from_file (jparser, path, error))
    return FALSE;

  JsonNode *metarootval = json_parser_get_root (jparser);
  return rpmostree_composeutil_read_json_metadata (metarootval, metadata, error);
}

static GVariantBuilder *
metadata_conversion_start (GHashTable *metadata)
{
  GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  GLNX_HASH_TABLE_FOREACH_KV (metadata, const char *, strkey, GVariant *, v)
    g_variant_builder_add (builder, "{sv}", strkey, v);

  return builder;
}

static GVariant *
metadata_conversion_end (GVariantBuilder *builder)
{
  g_autoptr (GVariant) ret = g_variant_ref_sink (g_variant_builder_end (builder));
  /* Canonicalize to big endian, like OSTree does. Without this, any numbers
   * we place in the metadata will be unreadable since clients won't know
   * their endianness.
   */
  rpmostree_variant_native_to_be (&ret);

  return util::move_nullify (ret);
}

/* Convert hash table of metadata into finalized GVariant */
GVariant *
rpmostree_composeutil_finalize_metadata (GHashTable *metadata, int rootfs_dfd, GError **error)
{
  g_autoptr (GVariantBuilder) builder = metadata_conversion_start (metadata);

  /* include list of packages in rpmdb; this is used client-side for easily previewing
   * pending updates. once we only support unified core composes, this can easily be much
   * more readily injected during assembly */
  g_autoptr (GVariant) rpmdb_v = NULL;
  if (!rpmostree_create_rpmdb_pkglist_variant (rootfs_dfd, ".", &rpmdb_v, NULL, error))
    return FALSE;
  g_variant_builder_add (builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb_v);

  return metadata_conversion_end (builder);
}

/* Convert hash table of metadata into finalized GVariant */
GVariant *
rpmostree_composeutil_finalize_detached_metadata (GHashTable *detached_metadata)
{
  /* canonicalize empty detached metadata to NULL */
  if (g_hash_table_size (detached_metadata) == 0)
    return NULL;

  g_autoptr (GVariantBuilder) builder = metadata_conversion_start (detached_metadata);
  return metadata_conversion_end (builder);
}

/* Implements --write-composejson-to, and also prints values.
 * If `path` is NULL, we'll just print some data.
 */
gboolean
rpmostree_composeutil_write_composejson (OstreeRepo *repo, const char *path,
                                         const OstreeRepoTransactionStats *stats,
                                         const char *new_revision, GVariant *new_commit,
                                         const char *new_ref, GCancellable *cancellable,
                                         GError **error)
{
  g_autoptr (GVariant) new_commit_inline_meta = g_variant_get_child_value (new_commit, 0);

  g_auto (GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (new_ref != NULL)
    g_variant_builder_add (&builder, "{sv}", "ref", g_variant_new_string (new_ref));

  /* Lift transaction statistics into compose properties, if any */
  if (stats)
    {
      g_variant_builder_add (&builder, "{sv}", "ostree-n-metadata-total",
                             g_variant_new_uint32 (stats->metadata_objects_total));
      g_variant_builder_add (&builder, "{sv}", "ostree-n-metadata-written",
                             g_variant_new_uint32 (stats->metadata_objects_written));
      g_variant_builder_add (&builder, "{sv}", "ostree-n-content-total",
                             g_variant_new_uint32 (stats->content_objects_total));
      g_variant_builder_add (&builder, "{sv}", "ostree-n-content-written",
                             g_variant_new_uint32 (stats->content_objects_written));
      g_variant_builder_add (&builder, "{sv}", "ostree-n-cache-hits",
                             g_variant_new_uint32 (stats->devino_cache_hits));
      g_variant_builder_add (&builder, "{sv}", "ostree-content-bytes-written",
                             g_variant_new_uint64 (stats->content_bytes_written));
    }

  g_variant_builder_add (&builder, "{sv}", "ostree-commit", g_variant_new_string (new_revision));

  g_autofree char *content_checksum = ostree_commit_get_content_checksum (new_commit);
  g_variant_builder_add (&builder, "{sv}", "ostree-content-checksum",
                         g_variant_new_string (content_checksum));

  const char *commit_version = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, OSTREE_COMMIT_META_KEY_VERSION, "&s",
                          &commit_version);
  if (commit_version)
    g_variant_builder_add (&builder, "{sv}", "ostree-version",
                           g_variant_new_string (commit_version));

  /* Since JavaScript doesn't have 64 bit integers and hence neither does JSON,
   * store this as a string:
   * https://stackoverflow.com/questions/10286204/the-right-json-date-format
   * */
  {
    guint64 commit_ts = ostree_commit_get_timestamp (new_commit);
    g_autofree char *commit_ts_iso_8601 = rpmostree_timestamp_str_from_unix_utc (commit_ts);
    g_variant_builder_add (&builder, "{sv}", "ostree-timestamp",
                           g_variant_new_string (commit_ts_iso_8601));
  }

  const char *inputhash = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, "rpmostree.inputhash", "&s", &inputhash);
  /* We may not have the inputhash in the split-up installroot case */
  if (inputhash)
    g_variant_builder_add (&builder, "{sv}", "rpm-ostree-inputhash",
                           g_variant_new_string (inputhash));

  g_autofree char *parent_revision = ostree_commit_get_parent (new_commit);
  if (path && parent_revision)
    {
      /* don't error if the parent doesn't exist */
      gboolean parent_exists;
      if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, parent_revision, &parent_exists,
                                   cancellable, error))
        return FALSE;

      if (parent_exists)
        {
          g_autoptr (GVariant) diffv = NULL;
          if (!rpm_ostree_db_diff_variant (repo, parent_revision, new_revision, TRUE, &diffv,
                                           cancellable, error))
            return FALSE;
          g_variant_builder_add (&builder, "{sv}", "pkgdiff", diffv);
        }
    }

  if (path)
    {
      g_autoptr (GVariant) composemeta_v = g_variant_builder_end (&builder);
      JsonNode *composemeta_node = json_gvariant_serialize (composemeta_v);
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_root (generator, composemeta_node);

      char *dnbuf = strdupa (path);
      const char *dn = dirname (dnbuf);
      g_auto (GLnxTmpfile) tmpf = {
        0,
      };
      if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dn, O_WRONLY | O_CLOEXEC, &tmpf, error))
        return FALSE;
      g_autoptr (GOutputStream) out = g_unix_output_stream_new (tmpf.fd, FALSE);
      /* See also similar code in status.c */
      if (json_generator_to_stream (generator, out, cancellable, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;

      /* World readable to match --write-commitid-to which uses umask */
      if (!glnx_fchmod (tmpf.fd, 0644, error))
        return FALSE;

      if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_REPLACE, AT_FDCWD, path, error))
        return FALSE;
    }

  return TRUE;
}
