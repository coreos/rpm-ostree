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
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libgsystem.h"

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static char *opt_cachedir;
static gboolean opt_force_nocache;
static char *opt_proxy;
static char *opt_output_repodata_dir;
static char **opt_metadata_strings;
static char *opt_repo;
static char **opt_override_pkg_repos;
static char *opt_touch_if_changed;
static gboolean opt_dry_run;
static gboolean opt_print_only;

static GOptionEntry option_entries[] = {
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, 0, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "output-repodata-dir", 0, 0, G_OPTION_ARG_STRING, &opt_output_repodata_dir, "Save downloaded repodata in DIR", "DIR" },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { "add-override-pkg-repo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_override_pkg_repos, "Include an additional package repository from DIRECTORY", "DIRECTORY" },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "touch-if-changed", 0, 0, G_OPTION_ARG_STRING, &opt_touch_if_changed, "Update the modification time on FILE if a new commit was created", "FILE" },
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Just print the transaction and exit", NULL },
  { "print-only", 0, 0, G_OPTION_ARG_NONE, &opt_print_only, "Just expand any includes and print treefile", NULL },
  { NULL }
};

/* FIXME: This is a copy of ot_admin_checksum_version */
static char *
checksum_version (GVariant *checksum)
{
  gs_unref_variant GVariant *metadata = NULL;
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
  gboolean ret = FALSE;
  gs_free char *ret_checksum = NULL;
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
  
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
          gs_unref_object GFile *srcfile = NULL;
          const char *src, *dest;
          JsonArray *add_el = json_array_get_array_element (add_files, i);
          gs_unref_object GFile *child = NULL;

          if (!add_el)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Element in add-files is not an array");
              goto out;
            }
          src = _rpmostree_jsonutil_array_require_string_element (add_el, 0, error);
          if (!src)
            goto out;

          dest = _rpmostree_jsonutil_array_require_string_element (add_el, 1, error);
          if (!dest)
            goto out;

          srcfile = g_file_resolve_relative_path (contextdir, src);

          if (!_rpmostree_util_update_checksum_from_file (checksum,
                                                          srcfile,
                                                          NULL,
                                                          error))
            goto out;

          g_checksum_update (checksum, (const guint8 *) dest, strlen (dest));
        }

    }

  /* FIXME; we should also hash the post script */

  /* Hash in each package */
  rpmostree_hif_add_checksum_goal (checksum, goal);

  ret_checksum = g_strdup (g_checksum_get_string (checksum));

  ret = TRUE;
 out:
  gs_transfer_out_value (out_checksum, &ret_checksum);
  if (checksum) g_checksum_free (checksum);
  return ret;
}


static void
on_hifstate_percentage_changed (HifState   *hifstate,
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
  gboolean ret = FALSE;
  guint len = json_array_get_length (a);
  guint i;
  g_autoptr(GPtrArray) instlangs_v = g_ptr_array_new ();
  
  for (i = 0; i < len; i++)
    {
      const char *elt = _rpmostree_jsonutil_array_require_string_element (a, i, error);

      if (!elt)
        goto out;

      g_ptr_array_add (instlangs_v, (char*)elt);
    }
  
  g_key_file_set_string_list (keyfile, keyfile_group, keyfile_key,
                              (const char*const*)instlangs_v->pdata, instlangs_v->len);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
install_packages_in_root (RpmOstreeTreeComposeContext  *self,
                          RpmOstreeContext *ctx,
                          JsonObject      *treedata,
                          GFile           *yumroot,
                          char           **packages,
                          gboolean        *out_unmodified,
                          char           **out_new_inputhash,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  GFile *contextdir = self->treefile_context_dirs->pdata[0];
  g_autoptr(RpmOstreeInstall) hifinstall = { 0, };
  HifContext *hifctx;
  gs_free char *ret_new_inputhash = NULL;
  g_autoptr(GKeyFile) treespec = g_key_file_new ();
  JsonArray *enable_repos = NULL;
  JsonArray *add_files = NULL;

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

  hifctx = rpmostree_context_get_hif (ctx);
  if (opt_proxy)
    hif_context_set_http_proxy (hifctx, opt_proxy);

  hif_context_set_repo_dir (hifctx, gs_file_get_path_cached (contextdir));

  g_key_file_set_string (treespec, "tree", "ref", self->ref);
  g_key_file_set_string_list (treespec, "tree", "packages", (const char *const*)packages, g_strv_length (packages));

  /* Some awful code to translate between JSON and GKeyFile */
  if (json_object_has_member (treedata, "install-langs"))
    {
      JsonArray *a = json_object_get_array_member (treedata, "install-langs");
      if (!set_keyfile_string_array_from_json (treespec, "tree", "install-langs", a, error))
        goto out;
    }

  /* Bind the json \"repos\" member to the hif state, which looks at the
   * enabled= member of the repos file.  By default we forcibly enable
   * only repos which are specified, ignoring the enabled= flag.
   */
  if (!json_object_has_member (treedata, "repos"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Treefile is missing required \"repos\" member");
      goto out;
    }

  enable_repos = json_object_get_array_member (treedata, "repos");

  if (!set_keyfile_string_array_from_json (treespec, "tree", "repos", enable_repos, error))
    goto out;

  { gboolean docs = TRUE;

    if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata,
                                                                 "documentation",
                                                                 &docs,
                                                                 error))
      goto out;

    if (!docs)
      g_key_file_set_boolean (treespec, "tree", "documentation", FALSE);
  }

  { g_autoptr(GError) tmp_error = NULL;
    g_autoptr(RpmOstreeTreespec) treespec_value = rpmostree_treespec_new_from_keyfile (treespec, &tmp_error);
    g_assert_no_error (tmp_error);
    
    if (!rpmostree_context_setup (ctx, gs_file_get_path_cached (yumroot), treespec_value,
                                  cancellable, error))
      goto out;
  }

  /* --- Downloading metadata --- */
  if (!rpmostree_context_download_metadata (ctx, cancellable, error))
    goto out;

  if (!rpmostree_context_prepare_install (ctx, &hifinstall, cancellable, error))
    goto out;

  if (json_object_has_member (treedata, "add-files"))
    add_files = json_object_get_array_member (treedata, "add-files");

  /* FIXME - just do a depsolve here before we compute download requirements */
  if (!compute_checksum_from_treefile_and_goal (self, hif_context_get_goal (hifctx),
                                                contextdir, add_files,
                                                &ret_new_inputhash, error))
    goto out;

  /* Only look for previous checksum if caller has passed *out_unmodified */
  if (self->previous_checksum && out_unmodified != NULL)
    {
      gs_unref_variant GVariant *commit_v = NULL;
      gs_unref_variant GVariant *commit_metadata = NULL;
      const char *previous_inputhash = NULL;
      
      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     self->previous_checksum,
                                     &commit_v, error))
        goto out;

      commit_metadata = g_variant_get_child_value (commit_v, 0);
      if (g_variant_lookup (commit_metadata, "rpmostree.inputhash", "&s", &previous_inputhash))
        {
          if (strcmp (previous_inputhash, ret_new_inputhash) == 0)
            {
              *out_unmodified = TRUE;
              ret = TRUE;
              goto out;
            }
        }
      else
        g_print ("Previous commit found, but without rpmostree.inputhash metadata key\n");
    }

  if (opt_dry_run)
    {
      ret = TRUE;
      goto out;
    }

  /* --- Downloading packages --- */
  if (!rpmostree_context_download_rpms (ctx, -1, hifinstall, cancellable, error))
    goto out;
  
  { g_auto(GLnxConsoleRef) console = { 0, };
    gs_unref_object HifState *hifstate = hif_state_new ();

    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Installing packages:");

    glnx_console_lock (&console);

    if (!hif_transaction_commit (hif_context_get_transaction (hifctx),
                                 hif_context_get_goal (hifctx),
                                 hifstate,
                                 error))
      goto out;

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }
      
  ret = TRUE;
  if (out_unmodified)
    *out_unmodified = FALSE;
  gs_transfer_out_value (out_new_inputhash, &ret_new_inputhash);
 out:
  return ret;
}

static gboolean
process_includes (RpmOstreeTreeComposeContext  *self,
                  GFile             *treefile_path,
                  guint              depth,
                  JsonObject        *root,
                  GCancellable      *cancellable,
                  GError           **error)
{
  gboolean ret = FALSE;
  const char *include_path;
  const guint maxdepth = 50;

  if (depth > maxdepth)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum include depth of %u", maxdepth);
      goto out;
    }

  {
    gs_unref_object GFile *parent = g_file_get_parent (treefile_path);
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

  if (!_rpmostree_jsonutil_object_get_optional_string_member (root, "include", &include_path, error))
    goto out;
                                          
  if (include_path)
    {
      gs_unref_object GFile *treefile_dirpath = g_file_get_parent (treefile_path);
      gs_unref_object GFile *parent_path = g_file_resolve_relative_path (treefile_dirpath, include_path);
      gs_unref_object JsonParser *parent_parser = json_parser_new ();
      JsonNode *parent_rootval;
      JsonObject *parent_root;
      GList *members;
      GList *iter;

      if (!json_parser_load_from_file (parent_parser,
                                       gs_file_get_path_cached (parent_path),
                                       error))
        goto out;

      parent_rootval = json_parser_get_root (parent_parser);
      if (!JSON_NODE_HOLDS_OBJECT (parent_rootval))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Treefile root is not an object");
          goto out;
        }
      parent_root = json_node_get_object (parent_rootval);
      
      if (!process_includes (self, parent_path, depth + 1, parent_root,
                             cancellable, error))
        goto out;
                             
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
                  goto out;
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

  ret = TRUE;
 out:
  return ret;
}

static gboolean
parse_keyvalue_strings (char             **strings,
                        GVariantBuilder   *builder,
                        GError           **error)
{
  gboolean ret = FALSE;
  char **iter;

  for (iter = strings; *iter; iter++)
    {
      const char *s;
      const char *eq;
      gs_free char *key = NULL;

      s = *iter;

      eq = strchr (s, '=');
      if (!eq)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Missing '=' in KEY=VALUE metadata '%s'", s);
          goto out;
        }
          
      key = g_strndup (s, eq - s);
      g_variant_builder_add (builder, "{sv}", key,
                             g_variant_new_string (eq + 1));
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
compose_strv_contains_prefix (gchar **strv,
                              const gchar  *prefix)
{
  if (!strv)
    return FALSE;

  while (*strv)
    {
      if (g_str_has_prefix (*strv, prefix))
        return TRUE;
      ++strv;
    }

  return FALSE;
}


int
rpmostree_compose_builtin_tree (int             argc,
                                char          **argv,
                                GCancellable   *cancellable,
                                GError        **error)
{
  int exit_status = EXIT_FAILURE;
  GError *temp_error = NULL;
  GOptionContext *context = g_option_context_new ("TREEFILE - Run yum and commit the result to an OSTree repository");
  RpmOstreeTreeComposeContext selfdata = { NULL, };
  RpmOstreeTreeComposeContext *self = &selfdata;
  JsonNode *treefile_rootval = NULL;
  JsonObject *treefile = NULL;
  gs_free char *cachekey = NULL;
  gs_free char *new_inputhash = NULL;
  gs_unref_object GFile *previous_root = NULL;
  gs_free char *previous_checksum = NULL;
  gs_unref_object GFile *yumroot = NULL;
  gs_unref_object GFile *yumroot_varcache = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *bootstrap_packages = NULL;
  gs_unref_ptrarray GPtrArray *packages = NULL;
  gs_unref_object GFile *treefile_path = NULL;
  gs_unref_object GFile *treefile_dirpath = NULL;
  gs_unref_object GFile *repo_path = NULL;
  gs_unref_object JsonParser *treefile_parser = NULL;
  gs_unref_variant_builder GVariantBuilder *metadata_builder = 
    g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  g_autoptr(RpmOstreeContext) corectx = NULL;
  g_autoptr(GHashTable) varsubsts = NULL;
  gboolean workdir_is_tmp = FALSE;

  self->treefile_context_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  
  if (!rpmostree_option_context_parse (context,
                                       option_entries,
                                       &argc, &argv,
                                       RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
                                       cancellable,
                                       NULL,
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

  if (getuid () != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "compose tree must presently be run as uid 0 (root)");
      goto out;
    }

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
      gs_free char *tmpd = NULL;

      if (!rpmostree_mkdtemp ("/var/tmp/rpm-ostree.XXXXXX", &tmpd, NULL, error))
        goto out;

      self->workdir = g_file_new_for_path (tmpd);
      workdir_is_tmp = TRUE;

      if (opt_workdir_tmpfs)
        {
          if (mount ("tmpfs", tmpd, "tmpfs", 0, (const void*)"mode=755") != 0)
            {
              _rpmostree_set_prefix_error_from_errno (error, errno,
                                                      "mount(tmpfs): ");
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

  if (opt_metadata_strings)
    {
      if (!parse_keyvalue_strings (opt_metadata_strings,
                                   metadata_builder, error))
        goto out;
    }

  if (fchdir (self->workdir_dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  corectx = rpmostree_context_new_unprivileged (self->cachedir_dfd, cancellable, error);
  if (!corectx)
    goto out;

  varsubsts = rpmostree_context_get_varsubsts (corectx);

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
      gs_unref_object JsonGenerator *generator = json_generator_new ();
      gs_unref_object GOutputStream *stdout = g_unix_output_stream_new (1, FALSE);

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

  yumroot = g_file_get_child (self->workdir, "rootfs.tmp");
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, "rootfs.tmp", cancellable, error))
    goto out;

  if (json_object_has_member (treefile, "automatic_version_prefix") &&
      !compose_strv_contains_prefix (opt_metadata_strings, "version="))
    {
      gs_unref_variant GVariant *variant = NULL;
      gs_free char *last_version = NULL;
      gs_free char *next_version = NULL;
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
      g_variant_builder_add (metadata_builder, "{sv}", "version",
                             g_variant_new_string (next_version));
    }

  bootstrap_packages = g_ptr_array_new ();
  packages = g_ptr_array_new ();

  if (json_object_has_member (treefile, "bootstrap_packages"))
    {
      if (!_rpmostree_jsonutil_append_string_array_to (treefile, "bootstrap_packages", packages, error))
        goto out;
    }
  if (!_rpmostree_jsonutil_append_string_array_to (treefile, "packages", packages, error))
    goto out;
  g_ptr_array_add (packages, NULL);

  { gs_unref_object JsonGenerator *generator = json_generator_new ();
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
          if (!rpmostree_generate_passwd_from_previous (repo, yumroot,
                                                        treefile_dirpath,
                                                        previous_root, treefile,
                                                        cancellable, error))
            goto out;
        }
    }

  { gboolean unmodified = FALSE;

    if (!install_packages_in_root (self, corectx, treefile, yumroot,
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
        g_print ("--dry-run complete, exiting\n");
        exit_status = EXIT_SUCCESS;
        goto out;
      }
  }

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    goto out;

  if (!rpmostree_treefile_postprocessing (yumroot, self->treefile_context_dirs->pdata[0],
                                          self->serialized_treefile, treefile,
                                          cancellable, error))
    goto out;

  if (!rpmostree_prepare_rootfs_for_commit (yumroot, treefile, cancellable, error))
    goto out;

  if (!rpmostree_copy_additional_files (yumroot, self->treefile_context_dirs->pdata[0], treefile, cancellable, error))
    goto out;

  if (!rpmostree_check_passwd (repo, yumroot, treefile_dirpath, treefile,
                               previous_checksum,
                               cancellable, error))
    goto out;

  if (!rpmostree_check_groups (repo, yumroot, treefile_dirpath, treefile,
                               previous_checksum,
                               cancellable, error))
    goto out;

  {
    const char *gpgkey;
    gboolean selinux = TRUE;
    gs_unref_variant GVariant *metadata = NULL;

    g_variant_builder_add (metadata_builder, "{sv}",
                           "rpmostree.inputhash",
                           g_variant_new_string (new_inputhash));

    metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));

    if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "gpg_key", &gpgkey, error))
      goto out;

    if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                                 "selinux",
                                                                 &selinux,
                                                                 error))
      goto out;

    { g_autofree char *new_revision = NULL;
      glnx_fd_close int rootfs_fd = -1;

      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (yumroot), TRUE,
                           &rootfs_fd, error))
        goto out;

      g_print ("Committing...\n");
      
      if (!rpmostree_commit (rootfs_fd, repo, self->ref, metadata, gpgkey, selinux, NULL,
                             &new_revision,
                             cancellable, error))
        goto out;

      g_print ("%s => %s\n", self->ref, new_revision);

      if (!g_getenv ("RPM_OSTREE_PRESERVE_ROOTFS"))
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (yumroot), cancellable, NULL);
      else
        g_print ("Preserved %s\n", gs_file_get_path_cached (yumroot));
    }
  }

  if (opt_touch_if_changed)
    {
      gs_fd_close int fd = open (opt_touch_if_changed, O_CREAT|O_WRONLY|O_NOCTTY, 0644);
      if (fd == -1)
        {
          gs_set_error_from_errno (error, errno);
          g_prefix_error (error, "Updating '%s': ", opt_touch_if_changed);
          goto out;
        }
      if (futimens (fd, NULL) == -1)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }
    }

  exit_status = EXIT_SUCCESS;

 out:
  /* Move back out of the workding directory to ensure unmount works */
  (void )chdir ("/");

  if (self->workdir_dfd != -1)
    (void) close (self->workdir_dfd);

  if (workdir_is_tmp)
    {
      if (opt_workdir_tmpfs)
        if (umount (gs_file_get_path_cached (self->workdir)) != 0)
          {
            fprintf (stderr, "warning: umount failed: %m\n");
          }
      (void) gs_shutil_rm_rf (self->workdir, NULL, NULL);
    }
  if (self)
    {
      g_clear_object (&self->workdir);
      g_clear_pointer (&self->serialized_treefile, g_bytes_unref);
      g_ptr_array_unref (self->treefile_context_dirs);
    }

  return exit_status;
}
