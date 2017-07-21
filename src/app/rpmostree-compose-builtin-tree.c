/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <libdnf/dnf-repo.h>
#include <sys/mount.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static char *opt_cachedir;
static gboolean opt_force_nocache;
static gboolean opt_cache_only;
static char *opt_proxy;
static char *opt_output_repodata_dir;
static char **opt_metadata_strings;
static char *opt_metadata_json;
static char *opt_repo;
static char *opt_touch_if_changed;
static gboolean opt_dry_run;
static gboolean opt_print_only;
static char *opt_write_commitid_to;

static GOptionEntry option_entries[] = {
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "add-metadata-from-json", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_json, "Parse the given JSON file as object, convert to GVariant, append to OSTree commit", "JSON" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, 0, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "output-repodata-dir", 0, 0, G_OPTION_ARG_STRING, &opt_output_repodata_dir, "Save downloaded repodata in DIR", "DIR" },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "cache-only", 0, 0, G_OPTION_ARG_NONE, &opt_cache_only, "Assume cache is present, do not attempt to update it", NULL },
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "touch-if-changed", 0, 0, G_OPTION_ARG_STRING, &opt_touch_if_changed, "Update the modification time on FILE if a new commit was created", "FILE" },
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Just print the transaction and exit", NULL },
  { "print-only", 0, 0, G_OPTION_ARG_NONE, &opt_print_only, "Just expand any includes and print treefile", NULL },
  { "write-commitid-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_commitid_to, "File to write the composed commitid to instead of updating the ref", "FILE" },
  { NULL }
};

/* FIXME: This is a copy of ot_admin_checksum_version */
static char *
checksum_version (GVariant *checksum)
{
  g_autoptr(GVariant) metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

typedef struct {
  GPtrArray *treefile_context_dirs;

  GFile *workdir;
  int workdir_dfd;
  int cachedir_dfd;
  OstreeRepo *repo;
  char *ref;
  char *previous_checksum;

  GBytes *serialized_treefile;
} RpmOstreeTreeComposeContext;

static gboolean
compute_checksum_from_treefile_and_goal (RpmOstreeTreeComposeContext   *self,
                                         HyGoal                         goal,
                                         GFile                         *contextdir,
                                         JsonArray                     *add_files,
                                         char                        **out_checksum,
                                         GError                      **error)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  /* Hash in the raw treefile; this means reordering the input packages
   * or adding a comment will cause a recompose, but let's be conservative
   * here.
   */
  { gsize len;
    const guint8* buf = g_bytes_get_data (self->serialized_treefile, &len);

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
  rpmostree_dnf_add_checksum_goal (checksum, goal);

  *out_checksum = g_strdup (g_checksum_get_string (checksum));
  return TRUE;
}


static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
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

/* Prepare /dev in the target root with the API devices.  TODO:
 * Delete this when we implement https://github.com/projectatomic/rpm-ostree/issues/729
 */
static gboolean
libcontainer_prep_dev (int         rootfs_dfd,
                       GError    **error)
{

  glnx_fd_close int src_fd = openat (AT_FDCWD, "/dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (src_fd == -1)
    return glnx_throw_errno (error);

  if (mkdirat (rootfs_dfd, "dev", 0755) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno (error);
    }

  glnx_fd_close int dest_fd = openat (rootfs_dfd, "dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dest_fd == -1)
    return glnx_throw_errno (error);

  static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
  for (guint i = 0; i < G_N_ELEMENTS (devnodes); i++)
    {
      const char *nodename = devnodes[i];
      struct stat stbuf;
      if (fstatat (src_fd, nodename, &stbuf, 0) == -1)
        {
          if (errno == ENOENT)
            continue;
          return glnx_throw_errno (error);
        }

      if (mknodat (dest_fd, nodename, stbuf.st_mode, stbuf.st_rdev) != 0)
        return glnx_throw_errno (error);
      if (fchmodat (dest_fd, nodename, stbuf.st_mode, 0) != 0)
        return glnx_throw_errno (error);
    }

  return TRUE;
}

static gboolean
treefile_sanity_checks (JsonObject   *treedata,
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
  if (fstatat (AT_FDCWD, src, &stbuf, 0) < 0)
    return glnx_throw_errno_prefix (error, "postprocess-script: stat(%s)", postprocess_script);

  if ((stbuf.st_mode & S_IXUSR) == 0)
    return glnx_throw (error, "postprocess-script (%s) must be executable", postprocess_script);

  /* Insert other sanity checks here */

  return TRUE;
}

static gboolean
install_packages_in_root (RpmOstreeTreeComposeContext  *self,
                          RpmOstreeContext *ctx,
                          JsonObject      *treedata,
                          GFile           *yumroot,
                          int              rootfs_dfd,
                          char           **packages,
                          gboolean        *out_unmodified,
                          char           **out_new_inputhash,
                          GCancellable    *cancellable,
                          GError         **error)
{
  /* TODO - uncomment this once we have SELinux working */
#if 0
  g_autofree char *cache_repo_pathstr = glnx_fdrel_abspath (self->cachedir_dfd, "repo");
  g_autoptr(GFile) cache_repo_path = g_file_new_for_path (cache_repo_pathstr);
  glnx_unref_object OstreeRepo *ostreerepo = ostree_repo_new (cache_repo_path);

  if (!g_file_test (cache_repo_pathstr, G_FILE_TEST_EXISTS))
    {
      if (!ostree_repo_create (ostreerepo, OSTREE_REPO_MODE_BARE_USER, cancellable, error))
        goto out;
    }
#endif

  DnfContext *hifctx = rpmostree_context_get_hif (ctx);
  if (opt_proxy)
    dnf_context_set_http_proxy (hifctx, opt_proxy);

  /* Hack this here... see https://github.com/rpm-software-management/libhif/issues/53
   * but in the future we won't be using librpm at all for unpack/scripts, so it won't
   * matter.
   */
  { const char *debuglevel = getenv ("RPMOSTREE_RPM_VERBOSITY");
    if (!debuglevel)
      debuglevel = "info";
    dnf_context_set_rpm_verbosity (hifctx, debuglevel);
    rpmlogSetFile(NULL);
  }

  GFile *contextdir = self->treefile_context_dirs->pdata[0];
  dnf_context_set_repo_dir (hifctx, gs_file_get_path_cached (contextdir));

  /* By default, retain packages in addition to metadata with --cachedir */
  if (opt_cachedir)
    dnf_context_set_keep_cache (hifctx, TRUE);
  /* For compose, always try to refresh metadata; we're used in build servers
   * where fetching should be cheap. Otherwise, if --cache-only is set, it's
   * likely an offline developer laptop case, so never refresh.
   */
  if (!opt_cache_only)
    dnf_context_set_cache_age (hifctx, 0);
  else
    dnf_context_set_cache_age (hifctx, G_MAXUINT);

  g_autoptr(GKeyFile) treespec = g_key_file_new ();
  g_key_file_set_string (treespec, "tree", "ref", self->ref);
  g_key_file_set_string_list (treespec, "tree", "packages", (const char *const*)packages, g_strv_length (packages));
  { const char *releasever;
    if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "releasever",
                                                                &releasever, error))
      return FALSE;
    if (releasever)
      g_key_file_set_string (treespec, "tree", "releasever", releasever);
  }

  /* Some awful code to translate between JSON and GKeyFile */
  if (json_object_has_member (treedata, "install-langs"))
    {
      JsonArray *a = json_object_get_array_member (treedata, "install-langs");
      if (!set_keyfile_string_array_from_json (treespec, "tree", "instlangs", a, error))
        return FALSE;
    }

  /* Bind the json \"repos\" member to the hif state, which looks at the
   * enabled= member of the repos file.  By default we forcibly enable
   * only repos which are specified, ignoring the enabled= flag.
   */
  if (!json_object_has_member (treedata, "repos"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Treefile is missing required \"repos\" member");
      return FALSE;
    }

  JsonArray *enable_repos = json_object_get_array_member (treedata, "repos");

  if (!set_keyfile_string_array_from_json (treespec, "tree", "repos", enable_repos, error))
    return FALSE;

  { gboolean docs = TRUE;

    if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata,
                                                                 "documentation",
                                                                 &docs,
                                                                 error))
      return FALSE;

    if (!docs)
      g_key_file_set_boolean (treespec, "tree", "documentation", FALSE);
  }

  { g_autoptr(GError) tmp_error = NULL;
    g_autoptr(RpmOstreeTreespec) treespec_value = rpmostree_treespec_new_from_keyfile (treespec, &tmp_error);
    g_assert_no_error (tmp_error);

    if (!rpmostree_context_setup (ctx, gs_file_get_path_cached (yumroot), NULL, treespec_value,
                                  cancellable, error))
      return FALSE;
  }

  if (!rpmostree_context_prepare (ctx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (hifctx);

  JsonArray *add_files = NULL;
  if (json_object_has_member (treedata, "add-files"))
    add_files = json_object_get_array_member (treedata, "add-files");

  /* FIXME - just do a depsolve here before we compute download requirements */
  g_autofree char *ret_new_inputhash = NULL;
  if (!compute_checksum_from_treefile_and_goal (self, dnf_context_get_goal (hifctx),
                                                contextdir, add_files,
                                                &ret_new_inputhash, error))
    return FALSE;

  /* Only look for previous checksum if caller has passed *out_unmodified */
  if (self->previous_checksum && out_unmodified != NULL)
    {
      g_autoptr(GVariant) commit_v = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      const char *previous_inputhash = NULL;

      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     self->previous_checksum,
                                     &commit_v, error))
        return FALSE;

      commit_metadata = g_variant_get_child_value (commit_v, 0);
      if (g_variant_lookup (commit_metadata, "rpmostree.inputhash", "&s", &previous_inputhash))
        {
          if (strcmp (previous_inputhash, ret_new_inputhash) == 0)
            {
              *out_unmodified = TRUE;
              return TRUE; /* NB: early return */
            }
        }
      else
        g_print ("Previous commit found, but without rpmostree.inputhash metadata key\n");
    }

  if (opt_dry_run)
    return TRUE; /* NB: early return */

  if (!treefile_sanity_checks (treedata, self->treefile_context_dirs->pdata[0],
                               cancellable, error))
    return FALSE;

  /* --- Downloading packages --- */
  if (!rpmostree_context_download (ctx, cancellable, error))
    return FALSE;

  { g_auto(GLnxConsoleRef) console = { 0, };
    g_autoptr(DnfState) hifstate = dnf_state_new ();

    guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                             G_CALLBACK (on_hifstate_percentage_changed),
                                             "Installing packages:");

    glnx_console_lock (&console);

    if (!libcontainer_prep_dev (rootfs_dfd, error))
      return FALSE;

    if (!dnf_transaction_commit (dnf_context_get_transaction (hifctx),
                                 dnf_context_get_goal (hifctx),
                                 hifstate,
                                 error))
      return FALSE;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }

  if (out_unmodified)
    *out_unmodified = FALSE;
  *out_new_inputhash = g_steal_pointer (&ret_new_inputhash);
  return TRUE;
}

static gboolean
process_includes (RpmOstreeTreeComposeContext  *self,
                  GFile             *treefile_path,
                  guint              depth,
                  JsonObject        *root,
                  GCancellable      *cancellable,
                  GError           **error)
{
  const guint maxdepth = 50;
  if (depth > maxdepth)
    return glnx_throw (error, "Exceeded maximum include depth of %u", maxdepth);

  {
    g_autoptr(GFile) parent = g_file_get_parent (treefile_path);
    gboolean existed = FALSE;
    if (self->treefile_context_dirs->len > 0)
      {
        GFile *prev = self->treefile_context_dirs->pdata[self->treefile_context_dirs->len-1];
        if (g_file_equal (parent, prev))
          existed = TRUE;
      }
    if (!existed)
      {
        g_ptr_array_add (self->treefile_context_dirs, parent);
        parent = NULL; /* Transfer ownership */
      }
  }

  const char *include_path;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (root, "include", &include_path, error))
    return FALSE;

  if (include_path)
    {
      g_autoptr(GFile) treefile_dirpath = g_file_get_parent (treefile_path);
      g_autoptr(GFile) parent_path = g_file_resolve_relative_path (treefile_dirpath, include_path);
      glnx_unref_object JsonParser *parent_parser = json_parser_new ();
      JsonNode *parent_rootval;
      JsonObject *parent_root;
      GList *members;
      GList *iter;

      if (!json_parser_load_from_file (parent_parser,
                                       gs_file_get_path_cached (parent_path),
                                       error))
        return FALSE;

      parent_rootval = json_parser_get_root (parent_parser);
      if (!JSON_NODE_HOLDS_OBJECT (parent_rootval))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Treefile root is not an object");
          return FALSE;
        }
      parent_root = json_node_get_object (parent_rootval);

      if (!process_includes (self, parent_path, depth + 1, parent_root,
                             cancellable, error))
        return FALSE;

      members = json_object_get_members (parent_root);
      for (iter = members; iter; iter = iter->next)
        {
          const char *name = iter->data;
          JsonNode *parent_val = json_object_get_member (parent_root, name);
          JsonNode *val = json_object_get_member (root, name);

          g_assert (parent_val);

          if (!val)
            json_object_set_member (root, name, json_node_copy (parent_val));
          else
            {
              JsonNodeType parent_type =
                json_node_get_node_type (parent_val);
              JsonNodeType child_type =
                json_node_get_node_type (val);
              if (parent_type != child_type)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Conflicting element type of '%s'",
                               name);
                  return FALSE;
                }
              if (child_type == JSON_NODE_ARRAY)
                {
                  JsonArray *parent_array = json_node_get_array (parent_val);
                  JsonArray *child_array = json_node_get_array (val);
                  JsonArray *new_child = json_array_new ();
                  guint i, len;

                  len = json_array_get_length (parent_array);
                  for (i = 0; i < len; i++)
                    json_array_add_element (new_child, json_node_copy (json_array_get_element (parent_array, i)));
                  len = json_array_get_length (child_array);
                  for (i = 0; i < len; i++)
                    json_array_add_element (new_child, json_node_copy (json_array_get_element (child_array, i)));

                  json_object_set_array_member (root, name, new_child);
                }
            }
        }

      json_object_remove_member (root, "include");
    }

  return TRUE;
}

static gboolean
parse_metadata_keyvalue_strings (char             **strings,
                                 GHashTable        *metadata_hash,
                                 GError           **error)
{
  char **iter;

  for (iter = strings; *iter; iter++)
    {
      const char *s;
      const char *eq;

      s = *iter;

      eq = strchr (s, '=');
      if (!eq)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Missing '=' in KEY=VALUE metadata '%s'", s);
          return FALSE;
        }

      g_hash_table_insert (metadata_hash, g_strndup (s, eq - s),
                           g_variant_ref_sink (g_variant_new_string (eq + 1)));
    }

  return TRUE;
}

static gboolean
process_touch_if_changed (GError **error)
{
  glnx_fd_close int fd = -1;

  if (!opt_touch_if_changed)
    return TRUE;

  fd = open (opt_touch_if_changed, O_CREAT|O_WRONLY|O_NOCTTY, 0644);
  if (fd == -1)
    {
      glnx_set_prefix_error_from_errno (error, "Updating '%s': ", opt_touch_if_changed);
      return FALSE;
    }
  if (futimens (fd, NULL) == -1)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

int
rpmostree_compose_builtin_tree (int             argc,
                                char          **argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable   *cancellable,
                                GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GError *temp_error = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE - Install packages and commit the result to an OSTree repository");
  RpmOstreeTreeComposeContext selfdata = { NULL, };
  RpmOstreeTreeComposeContext *self = &selfdata;
  JsonNode *treefile_rootval = NULL;
  JsonObject *treefile = NULL;
  g_autofree char *new_inputhash = NULL;
  g_autoptr(GFile) previous_root = NULL;
  g_autofree char *previous_checksum = NULL;
  const char *rootfs_name = "rootfs.tmp";
  g_autoptr(GFile) yumroot = NULL;
  glnx_fd_close int rootfs_fd = -1;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GPtrArray) packages = NULL;
  g_autoptr(GFile) treefile_path = NULL;
  g_autoptr(GFile) treefile_dirpath = NULL;
  g_autoptr(GFile) repo_path = NULL;
  glnx_unref_object JsonParser *treefile_parser = NULL;
  g_autoptr(GHashTable) metadata_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  g_autoptr(RpmOstreeContext) corectx = NULL;
  g_autoptr(GHashTable) varsubsts = NULL;
  gboolean workdir_is_tmp = FALSE;
  g_autofree char *next_version = NULL;
  g_autofree char *new_revision = NULL;
  g_autoptr(GVariant) metadata = NULL;

  self->treefile_context_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL,
                                       error))
    goto out;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "TREEFILE must be specified", error);
      goto out;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      goto out;
    }

  /* Test whether or not bwrap is going to work - we will fail inside e.g. a Docker
   * container without --privileged or userns exposed.
   */
  if (!rpmostree_bwrap_selftest (error))
    goto out;

  repo_path = g_file_new_for_path (opt_repo);
  repo = self->repo = ostree_repo_new (repo_path);
  if (!ostree_repo_open (repo, cancellable, error))
    goto out;

  treefile_path = g_file_new_for_path (argv[1]);

  if (opt_workdir)
    {
      self->workdir = g_file_new_for_path (opt_workdir);
    }
  else
    {
      g_autofree char *tmpd = NULL;

      if (!rpmostree_mkdtemp ("/var/tmp/rpm-ostree.XXXXXX", &tmpd, NULL, error))
        goto out;

      self->workdir = g_file_new_for_path (tmpd);
      workdir_is_tmp = TRUE;

      if (opt_workdir_tmpfs)
        {
          if (mount ("tmpfs", tmpd, "tmpfs", 0, (const void*)"mode=755") != 0)
            {
              glnx_set_prefix_error_from_errno (error, "%s", "mount(tmpfs)");
              goto out;
            }
        }
    }

  if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->workdir),
                       FALSE, &self->workdir_dfd, error))
    goto out;

  if (opt_cachedir)
    {
      if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
        {
          g_prefix_error (error, "Opening cachedir '%s': ", opt_cachedir);
          goto out;
        }
    }
  else
    {
      self->cachedir_dfd = fcntl (self->workdir_dfd, F_DUPFD_CLOEXEC, 3);
      if (self->cachedir_dfd < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (opt_metadata_json)
    {
      glnx_unref_object JsonParser *jparser = json_parser_new ();
      JsonNode *metarootval = NULL; /* unowned */
      g_autoptr(GVariant) jsonmetav = NULL;
      GVariantIter viter;

      if (!json_parser_load_from_file (jparser, opt_metadata_json, error))
        goto out;

      metarootval = json_parser_get_root (jparser);

      jsonmetav = json_gvariant_deserialize (metarootval, "a{sv}", error);
      if (!jsonmetav)
        {
          g_prefix_error (error, "Parsing %s: ", opt_metadata_json);
          goto out;
        }

      g_variant_iter_init (&viter, jsonmetav);
      { char *key;
        GVariant *value;
        while (g_variant_iter_loop (&viter, "{sv}", &key, &value))
          g_hash_table_replace (metadata_hash, g_strdup (key), g_variant_ref (value));
      }
    }

  if (opt_metadata_strings)
    {
      if (!parse_metadata_keyvalue_strings (opt_metadata_strings, metadata_hash, error))
        goto out;
    }

  if (fchdir (self->workdir_dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  corectx = rpmostree_context_new_compose (self->cachedir_dfd, cancellable, error);
  if (!corectx)
    goto out;

  varsubsts = rpmostree_dnfcontext_get_varsubsts (rpmostree_context_get_hif (corectx));

  treefile_parser = json_parser_new ();
  if (!json_parser_load_from_file (treefile_parser,
                                   gs_file_get_path_cached (treefile_path),
                                   error))
    goto out;

  treefile_rootval = json_parser_get_root (treefile_parser);
  if (!JSON_NODE_HOLDS_OBJECT (treefile_rootval))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Treefile root is not an object");
      goto out;
    }
  treefile = json_node_get_object (treefile_rootval);

  if (!process_includes (self, treefile_path, 0, treefile,
                         cancellable, error))
    goto out;

  if (opt_print_only)
    {
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      g_autoptr(GOutputStream) stdout = g_unix_output_stream_new (1, FALSE);

      json_generator_set_pretty (generator, TRUE);
      json_generator_set_root (generator, treefile_rootval);
      (void) json_generator_to_stream (generator, stdout, NULL, NULL);

      exit_status = EXIT_SUCCESS;
      goto out;
    }

  { const char *input_ref = _rpmostree_jsonutil_object_require_string_member (treefile, "ref", error);
    if (!input_ref)
      goto out;
    self->ref = _rpmostree_varsubst_string (input_ref, varsubsts, error);
    if (!self->ref)
      goto out;
  }

  if (!ostree_repo_read_commit (repo, self->ref, &previous_root, &previous_checksum,
                                cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          g_print ("No previous commit for %s\n", self->ref);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    g_print ("Previous commit: %s\n", previous_checksum);

  self->previous_checksum = previous_checksum;

  yumroot = g_file_get_child (self->workdir, rootfs_name);
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
    goto out;
  if (mkdirat (self->workdir_dfd, rootfs_name, 0755) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (!glnx_opendirat (self->workdir_dfd, rootfs_name, TRUE,
                       &rootfs_fd, error))
    goto out;

  if (json_object_has_member (treefile, "automatic_version_prefix") &&
      /* let --add-metadata-string=version=... take precedence */
      !g_hash_table_contains (metadata_hash, "version"))
    {
      g_autoptr(GVariant) variant = NULL;
      g_autofree char *last_version = NULL;
      const char *ver_prefix;

      ver_prefix = _rpmostree_jsonutil_object_require_string_member (treefile,
                                                                     "automatic_version_prefix",
                                                                     error);
      if (!ver_prefix)
          goto out;

      if (previous_checksum)
        {
          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         previous_checksum, &variant, error))
            goto out;

          last_version = checksum_version (variant);
        }

      next_version = _rpmostree_util_next_version (ver_prefix, last_version);
      g_hash_table_insert (metadata_hash, g_strdup ("version"),
                           g_variant_ref_sink (g_variant_new_string (next_version)));
    }
  else
    {
      GVariant *v = g_hash_table_lookup (metadata_hash, "version");
      if (v)
        {
          g_assert (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING));
          next_version = g_variant_dup_string (v, NULL);
        }
    }

  packages = g_ptr_array_new_with_free_func (g_free);

  if (json_object_has_member (treefile, "bootstrap_packages"))
    {
      if (!_rpmostree_jsonutil_append_string_array_to (treefile, "bootstrap_packages", packages, error))
        goto out;
    }
  if (!_rpmostree_jsonutil_append_string_array_to (treefile, "packages", packages, error))
    goto out;

  { g_autofree char *thisarch_packages = g_strconcat ("packages-", dnf_context_get_base_arch (rpmostree_context_get_hif (corectx)), NULL);

    if (json_object_has_member (treefile, thisarch_packages))
      {
        if (!_rpmostree_jsonutil_append_string_array_to (treefile, thisarch_packages, packages, error))
          goto out;
      }
  }
  g_ptr_array_add (packages, NULL);

  { glnx_unref_object JsonGenerator *generator = json_generator_new ();
    char *treefile_buf = NULL;
    gsize len;

    json_generator_set_root (generator, treefile_rootval);
    json_generator_set_pretty (generator, TRUE);
    treefile_buf = json_generator_to_data (generator, &len);

    self->serialized_treefile = g_bytes_new_take (treefile_buf, len);
  }

  treefile_dirpath = g_file_get_parent (treefile_path);
  if (TRUE)
    {
      gboolean generate_from_previous = TRUE;

      if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                                   "preserve-passwd",
                                                                   &generate_from_previous,
                                                                   error))
        goto out;

      if (generate_from_previous)
        {
          if (!rpmostree_generate_passwd_from_previous (repo, rootfs_fd,
                                                        treefile_dirpath,
                                                        previous_root, treefile,
                                                        cancellable, error))
            goto out;
        }
    }

  { gboolean unmodified = FALSE;

    if (!install_packages_in_root (self, corectx, treefile, yumroot, rootfs_fd,
                                   (char**)packages->pdata,
                                   opt_force_nocache ? NULL : &unmodified,
                                   &new_inputhash,
                                   cancellable, error))
      goto out;

    if (unmodified)
      {
        g_print ("No apparent changes since previous commit; use --force-nocache to override\n");
        exit_status = EXIT_SUCCESS;
        goto out;
      }
    else if (opt_dry_run)
      {
        g_print ("--dry-run complete");
        if (opt_touch_if_changed)
          g_print (", updating --touch-if-changed=%s", opt_touch_if_changed);
        g_print ("; exiting\n");
        if (!process_touch_if_changed (error))
          goto out;
        exit_status = EXIT_SUCCESS;
        goto out;
      }
  }

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    goto out;

  if (!rpmostree_treefile_postprocessing (rootfs_fd, self->treefile_context_dirs->pdata[0],
                                          self->serialized_treefile, treefile,
                                          next_version, cancellable, error))
    {
      g_prefix_error (error, "Postprocessing: ");
      goto out;
    }

  if (!rpmostree_prepare_rootfs_for_commit (self->workdir_dfd, &rootfs_fd, rootfs_name,
                                            treefile,
                                            cancellable, error))
    {
      g_prefix_error (error, "Preparing rootfs for commit: ");
      goto out;
    }

  if (!rpmostree_copy_additional_files (yumroot, self->treefile_context_dirs->pdata[0], treefile, cancellable, error))
    goto out;

  if (!rpmostree_check_passwd (repo, yumroot, treefile_dirpath, treefile,
                               previous_checksum,
                               cancellable, error))
    {
      g_prefix_error (error, "Handling passwd db: ");
      goto out;
    }

  if (!rpmostree_check_groups (repo, yumroot, treefile_dirpath, treefile,
                               previous_checksum,
                               cancellable, error))
    {
      g_prefix_error (error, "Handling group db: ");
      goto out;
    }

  /* Insert our input hash */
  g_hash_table_replace (metadata_hash, g_strdup ("rpmostree.inputhash"),
                        g_variant_ref_sink (g_variant_new_string (new_inputhash)));

  const char *gpgkey = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "gpg_key", &gpgkey, error))
    goto out;

  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile, "selinux", &selinux, error))
    goto out;

  /* Convert metadata hash to GVariant */
  { g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    GLNX_HASH_TABLE_FOREACH_KV (metadata_hash, const char*, strkey, GVariant*, v)
      g_variant_builder_add (metadata_builder, "{sv}", strkey, v);

    metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
    /* Canonicalize to big endian, like OSTree does. Without this, any numbers
     * we place in the metadata will be unreadable since clients won't know
     * their endianness.
     */
    if (G_BYTE_ORDER != G_BIG_ENDIAN)
      {
        GVariant *swapped = g_variant_byteswap (metadata);
        GVariant *orig = metadata;
        metadata = swapped;
        g_variant_unref (orig);
      }
  }

  if (!rpmostree_commit (rootfs_fd, repo, self->ref, opt_write_commitid_to, metadata, gpgkey, selinux, NULL,
                         &new_revision,
                         cancellable, error))
    goto out;

  g_print ("%s => %s\n", self->ref, new_revision);

  if (!process_touch_if_changed (error))
    goto out;

  exit_status = EXIT_SUCCESS;

 out:
  /* Explicitly close this one now as it may have references to files
   * we delete below.
   */
  g_clear_object (&corectx);

  g_free (self->ref);

  /* Move back out of the workding directory and close all fds pointing
   * to it ensure unmount works */
  (void )chdir ("/");
  if (self->workdir_dfd != -1)
    (void) close (self->workdir_dfd);
  if (rootfs_fd != -1)
    (void) close (rootfs_fd);

  if (workdir_is_tmp)
    {
      if (opt_workdir_tmpfs)
        if (umount (gs_file_get_path_cached (self->workdir)) != 0)
          {
            fprintf (stderr, "warning: umount failed: %m\n");
          }
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (self->workdir), NULL, NULL);
    }
  if (self)
    {
      g_clear_object (&self->workdir);
      g_clear_pointer (&self->serialized_treefile, g_bytes_unref);
      g_ptr_array_unref (self->treefile_context_dirs);
    }

  return exit_status;
}
