/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "rpmostree-util.h"
#include "rpmostreed-utils.h"
#include "rpmostreed-errors.h"
#include "libglnx.h"
#include <systemd/sd-journal.h>
#include <stdint.h>

#include <libglnx.h>

static void
append_to_object_path (GString *str,
                       const gchar *s)
{
  guint n;

  for (n = 0; s[n] != '\0'; n++)
    {
      gint c = s[n];
      /* D-Bus spec sez:
       *
       * Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_-"
       */
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
        {
          g_string_append_c (str, c);
        }
      else if (c == '-' || c == '/')
        {
          /* Swap / or - for _ to keep names easier to read */
          g_string_append_c (str, '_');
        }
      else
        {
          /* Escape bytes not in [A-Z][a-z][0-9] as _<hex-with-two-digits> */
          g_string_append_printf (str, "_%02x", c & 0xFF);
        }
    }
}

/**
 * rpmostreed_generate_object_path:
 * @base: The base object path (without trailing '/').
 * @part...: UTF-8 strings.
 *
 * Appends @s to @base in a way such that only characters that can be
 * used in a D-Bus object path will be used. E.g. a character not in
 * <literal>[A-Z][a-z][0-9]_</literal> will be escaped as _HEX where
 * HEX is a two-digit hexadecimal number.
 *
 * Note that his mapping is not bijective - e.g. you cannot go back
 * to the original string.
 *
 * Returns: An allocated string that must be freed with g_free ().
 */
gchar *
rpmostreed_generate_object_path (const gchar *base,
                                 const gchar *part,
                                 ...)
{
  gchar *result;
  va_list va;

  va_start (va, part);
  result = rpmostreed_generate_object_path_from_va (base, part, va);
  va_end (va);

  return result;
}

gchar *
rpmostreed_generate_object_path_from_va (const gchar *base,
                                         const gchar *part,
                                         va_list va)
{
  GString *path;

  g_return_val_if_fail (base != NULL, NULL);
  g_return_val_if_fail (g_variant_is_object_path (base), NULL);
  g_return_val_if_fail (!g_str_has_suffix (base, "/"), NULL);

  path = g_string_new (base);

  while (part != NULL)
    {
      if (!g_utf8_validate (part, -1, NULL))
        {
          g_string_free (path, TRUE);
          return NULL;
        }
      else
        {
          g_string_append_c (path, '/');
          append_to_object_path (path, part);
          part = va_arg (va, const gchar *);
        }
    }

  return g_string_free (path, FALSE);
}

/**
 * rpmostreed_refspec_parse_partial:
 * @new_provided_refspec: The provided refspec
 * @base_refspec: The refspec string to base on.
 * @out_refspec: Pointer to the new refspec
 * @error: Pointer to an error pointer.
 *
 * Takes a refspec string and adds any missing bits based on the
 * base_refspec argument. Errors if a full valid refspec can't
 * be derived.
 *
 * Returns: True on success.
 */
gboolean
rpmostreed_refspec_parse_partial (const gchar *new_provided_refspec,
                                  const gchar *base_refspec,
                                  gchar **out_refspec,
                                  GError **error)
{
  g_autofree gchar *ref = NULL;
  g_autofree gchar *remote = NULL;
  gboolean infer_remote = TRUE;

  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
    {
      remote = g_strndup (new_provided_refspec, strlen(new_provided_refspec)-1);
    }
  /* Allow switching to a local branch */
  else if (g_str_has_prefix (new_provided_refspec, ":"))
    {
      infer_remote = FALSE;
      ref = g_strdup (new_provided_refspec + 1);
    }
  else
    {
      g_autoptr(GError) parse_error = NULL;
      if (!ostree_parse_refspec (new_provided_refspec, &remote,
                                 &ref, &parse_error))
        {
          g_set_error_literal (error, RPM_OSTREED_ERROR,
                               RPM_OSTREED_ERROR_INVALID_REFSPEC,
                               parse_error->message);
          return FALSE;
        }
    }

  g_autofree gchar *origin_ref = NULL;
  g_autofree gchar *origin_remote = NULL;
  if (base_refspec != NULL)
    {
      g_autoptr(GError) parse_error = NULL;
      if (!ostree_parse_refspec (base_refspec, &origin_remote,
                                 &origin_ref, &parse_error))
        {
          g_set_error_literal (error, RPM_OSTREED_ERROR,
                               RPM_OSTREED_ERROR_INVALID_REFSPEC,
                               parse_error->message);
          return FALSE;
        }
    }

  if (ref == NULL)
    {
      if (origin_ref)
        {
          ref = g_strdup (origin_ref);
        }
      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                      "Could not determine default ref to pull.");
          return FALSE;
        }

    }
  else if (infer_remote && remote == NULL)
    {
      if (origin_remote)
        {
          remote = g_strdup (origin_remote);
        }
    }

  if (remote == NULL)
      *out_refspec = util::move_nullify (ref);
  else
      *out_refspec = g_strconcat (remote, ":", ref, NULL);

  return TRUE;
}

void
rpmostreed_reboot (GCancellable *cancellable, GError **error)
{
  const char *child_argv[] = { "systemctl", "reboot", NULL };
  (void) g_spawn_sync (NULL, (char**)child_argv, NULL, (GSpawnFlags)(G_SPAWN_CHILD_INHERITS_STDIN | G_SPAWN_SEARCH_PATH),
                       NULL, NULL, NULL, NULL, NULL, NULL);
}

/**
 * rpmostreed_repo_pull_ancestry:
 * @repo: Repo
 * @refspec: Repository branch
 * @visitor: (allow-none): Visitor function to call on each commit
 * @visitor_data: (allow-none): User data for @visitor
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Downloads an ancestry of commit objects starting from @refspec.
 *
 * If a @visitor function pointer is given, commit objects are downloaded
 * in batches and the @visitor function is called for each commit object.
 * The @visitor function can stop the recursion, such as when looking for
 * a particular commit.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
rpmostreed_repo_pull_ancestry (OstreeRepo               *repo,
                               const char               *refspec,
                               RpmostreedCommitVisitor   visitor,
                               gpointer                  visitor_data,
                               OstreeAsyncProgress      *progress,
                               GCancellable             *cancellable,
                               GError                  **error)
{
  OstreeRepoPullFlags flags;
  GVariantDict options;
  GVariant *refs_value;
  const char *refs_array[] = { NULL, NULL };
  g_autofree char *remote = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *checksum = NULL;
  int depth, ii;
  gboolean ret = FALSE;

  /* Only fetch the HEAD on the first pass. See also:
   * https://github.com/projectatomic/rpm-ostree/pull/557 */
  gboolean first_pass = TRUE;

  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);
  g_return_val_if_fail (refspec != NULL, FALSE);

  if (!ostree_parse_refspec (refspec, &remote, &ref, error))
    goto out;

  /* If no visitor function was provided then we won't be short-circuiting
   * the recursion, so pull everything in one shot. Otherwise pull commits
   * in increasingly large batches. */
  depth = (visitor != NULL) ? 10 : -1;

  flags = OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY;

  /* It's important to use the ref name instead of a checksum on the first
   * pass because we want to search from the latest available commit on the
   * remote server, which is not necessarily what the ref name is currently
   * pointing at in our local repo. */
  refs_array[0] = ref;

  while (TRUE)
    {
      if (remote != NULL)
        {
          /* Floating reference, transferred to dictionary. */
          refs_value =
            g_variant_new_strv ((const char * const *) refs_array, -1);

          g_variant_dict_init (&options, NULL);
          if (!first_pass)
            g_variant_dict_insert (&options, "depth", "i", depth);
          g_variant_dict_insert (&options, "flags", "i", flags);
          g_variant_dict_insert_value (&options, "refs", refs_value);

          if (!ostree_repo_pull_with_options (repo, remote,
                                              g_variant_dict_end (&options),
                                              progress, cancellable, error))
            goto out;

          if (progress)
            ostree_async_progress_finish (progress);
        }

      /* First pass only.  Now we can resolve the ref to a checksum. */
      if (checksum == NULL)
        {
          if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &checksum, error))
            goto out;
        }

      if (visitor != NULL)
        {
          for (ii = 0; ii < (first_pass ? 1 : depth) && checksum != NULL; ii++)
            {
              g_autoptr(GVariant) commit = NULL;
              gboolean stop = FALSE;

              if (!ostree_repo_load_commit (repo, checksum, &commit,
                                            NULL, error))
                goto out;

              if (!visitor (repo, checksum, commit, visitor_data, &stop, error))
                goto out;

              g_clear_pointer (&checksum, g_free);

              if (!stop)
                checksum = ostree_commit_get_parent (commit);
            }
        }

      /* Break if no visitor, or visitor told us to stop. */
      if (visitor == NULL || checksum == NULL)
        break;

      /* Pull the next batch of commits, twice as many. */
      refs_array[0] = checksum;

      if (!first_pass)
        depth = depth * 2;
      first_pass = FALSE;
    }

  ret = TRUE;

out:
  return ret;
}

typedef struct {
  const char *version;
  char *checksum;
} VersionVisitorClosure;

static gboolean
version_visitor (OstreeRepo  *repo,
                 const char  *checksum,
                 GVariant    *commit,
                 gpointer     user_data,
                 gboolean    *out_stop,
                 GError     **error)
{
  auto closure = static_cast<VersionVisitorClosure *>(user_data);
  g_autoptr(GVariant) metadict = NULL;
  const char *version = NULL;

  metadict = g_variant_get_child_value (commit, 0);
  if (g_variant_lookup (metadict, "version", "&s", &version))
    {
      if (g_str_equal (version, closure->version))
        {
          closure->checksum = g_strdup (checksum);
          *out_stop = TRUE;
        }
    }

  return TRUE;
}

/**
 * rpmostreed_repo_lookup_version:
 * @repo: Repo
 * @refspec: Repository branch
 * @version: Version to look for
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @out_checksum: (out) (allow-none): Commit checksum, or %NULL
 * @error: Error
 *
 * Tries to determine the commit checksum for @version on @refspec.
 * This may require pulling commit objects from a remote repository.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
rpmostreed_repo_lookup_version (OstreeRepo           *repo,
                                const char           *refspec,
                                const char           *version,
                                OstreeAsyncProgress  *progress,
                                GCancellable         *cancellable,
                                char                **out_checksum,
                                GError              **error)
{
  VersionVisitorClosure closure = { version, NULL };

  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);
  g_return_val_if_fail (refspec != NULL, FALSE);
  g_return_val_if_fail (version != NULL, FALSE);

  if (!rpmostreed_repo_pull_ancestry (repo, refspec,
                                      version_visitor, &closure,
                                      progress, cancellable, error))
    return FALSE;

  g_autofree char *checksum = util::move_nullify (closure.checksum);
  if (checksum == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Version %s not found in %s", version, refspec);
      return FALSE;
    }

  if (out_checksum != NULL)
    *out_checksum = util::move_nullify (checksum);
  return TRUE;
}

typedef struct {
  const char *wanted_checksum;
  gboolean found;
} ChecksumVisitorClosure;

static gboolean
checksum_visitor (OstreeRepo  *repo,
                  const char  *checksum,
                  GVariant    *commit,
                  gpointer     user_data,
                  gboolean    *out_stop,
                  GError     **error)
{
  auto closure = static_cast<ChecksumVisitorClosure *>(user_data);
  *out_stop = closure->found = g_str_equal (checksum, closure->wanted_checksum);
  return TRUE;
}

/**
 * rpmostreed_repo_lookup_checksum:
 * @repo: Repo
 * @refspec: Repository branch
 * @checksum: Checksum to look for
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Tries to determine if the checksum given belongs on the remote and branch
 * given by @refspec. This may require pulling commit objects from a remote
 * repository.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
rpmostreed_repo_lookup_checksum (OstreeRepo           *repo,
                                 const char           *refspec,
                                 const char           *checksum,
                                 OstreeAsyncProgress  *progress,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  ChecksumVisitorClosure closure = { checksum, FALSE };

  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);
  g_return_val_if_fail (refspec != NULL, FALSE);
  g_return_val_if_fail (checksum != NULL, FALSE);

  if (!rpmostreed_repo_pull_ancestry (repo, refspec,
                                      checksum_visitor, &closure,
                                      progress, cancellable, error))
    return FALSE;

  if (!closure.found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Checksum %s not found in %s", checksum, refspec);
      return FALSE;
    }

  return TRUE;
}

/**
 * rpmostreed_repo_lookup_cached_version:
 * @repo: Repo
 * @refspec: Repository branch
 * @version: Version to look for
 * @cancellable: Cancellable
 * @out_checksum: (out) (allow-none): Commit checksum, or %NULL
 * @error: Error
 *
 * Similar to rpmostreed_repo_lookup_version(), except without pulling
 * from a remote repository.  It traverses whatever commits are available
 * locally in @repo.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
rpmostreed_repo_lookup_cached_version (OstreeRepo    *repo,
                                       const char    *refspec,
                                       const char    *version,
                                       GCancellable  *cancellable,
                                       char         **out_checksum,
                                       GError       **error)
{
  VersionVisitorClosure closure = { version, NULL };
  g_autofree char *checksum = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);
  g_return_val_if_fail (refspec != NULL, FALSE);
  g_return_val_if_fail (version != NULL, FALSE);

  if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &checksum, error))
    goto out;

  while (checksum != NULL)
    {
      g_autoptr(GVariant) commit = NULL;
      gboolean stop = FALSE;

      if (!ostree_repo_load_commit (repo, checksum, &commit, NULL, error))
        goto out;

      if (!version_visitor (repo, checksum, commit, &closure, &stop, error))
        goto out;

      g_clear_pointer (&checksum, g_free);

      if (!stop)
        checksum = ostree_commit_get_parent (commit);
    }

  if (closure.checksum == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Version %s not cached in %s", version, refspec);
      goto out;
    }

  if (out_checksum != NULL)
    *out_checksum = util::move_nullify (closure.checksum);

  g_free (closure.checksum);

  ret = TRUE;

out:
  return ret;
}

/**
 * rpmostreed_parse_revision:
 * @revision: Revision string
 * @out_checksum: (out) (allow-none): Commit checksum, or %NULL
 * @out_version: (out) (allow-none): Version value, or %NULL
 * @error: Error
 *
 * Determines @revision to either be a SHA256 checksum or a version metadata
 * value, sets one of @out_checksum or @out_version appropriately, and then
 * returns %TRUE.
 *
 * The @revision string may have a "revision=" prefix to denote a SHA256
 * checksum, or a "version=" prefix to denote a version metadata value.  If
 * the @revision string lacks either prefix, the function attempts to infer
 * the type of revision.  The prefixes are case-insensitive.
 *
 * The only possible error is if a "revision=" prefix is given, but the
 * rest of the string is not a valid SHA256 checksum.  In that case the
 * function sets @error and returns %FALSE.
 *
 * Returns: %TRUE on success, %FALSE of failure
 */
gboolean
rpmostreed_parse_revision (const char  *revision,
                           char       **out_checksum,
                           char       **out_version,
                           GError     **error)
{
  const char *checksum = NULL;
  const char *version = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (revision != NULL, FALSE);

  if (g_ascii_strncasecmp (revision, "revision=", 9) == 0)
    {
      checksum = revision + 9;

      /* Since this claims to be a checksum, fail if it isn't. */
      if (!ostree_validate_checksum_string (checksum, error))
        goto out;
    }
  else if (g_ascii_strncasecmp (revision, "version=", 8) == 0)
    {
      version = revision + 8;
    }
  else if (ostree_validate_checksum_string (revision, NULL))
    {
      /* If it looks like a checksum, assume it is. */
      checksum = revision;
    }
  else
    {
      /* Treat anything else as a version metadata value. */
      version = revision;
    }

  if (out_checksum != NULL)
    *out_checksum = g_strdup (checksum);

  if (out_version != NULL)
    *out_version = g_strdup (version);

  ret = TRUE;

out:
  return ret;
}
