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
#include <sys/mount.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-postprocess.h"

#include "libgsystem.h"

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static char *opt_cachedir;
static char *opt_proxy;
static char *opt_repo;
static gboolean opt_print_only;

static GOptionEntry option_entries[] = {
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, 0, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "print-only", 0, 0, G_OPTION_ARG_NONE, &opt_print_only, "Just expand any includes and print treefile", NULL },
  { NULL }
};

typedef struct {
  GPtrArray *treefile_context_dirs;
} RpmOstreeTreeComposeContext;

static char *
subprocess_context_print_args (GSSubprocessContext   *ctx)
{
  GString *ret = g_string_new ("");
  gs_strfreev char **argv = NULL;
  char **strviter;

  g_object_get ((GObject*)ctx, "argv", &argv, NULL);
  for (strviter = argv; strviter && *strviter; strviter++)
    {
      gs_free char *quoted = g_shell_quote (*strviter);
      g_string_append_c (ret, ' ');
      g_string_append (ret, quoted);
    }

  return g_string_free (ret, FALSE);
}

static gboolean
object_get_optional_string_member (JsonObject     *object,
                                   const char     *member_name,
                                   const char    **out_value,
                                   GError        **error)
{
  gboolean ret = FALSE;
  JsonNode *node = json_object_get_member (object, member_name);

  if (node != NULL)
    {
      *out_value = json_node_get_string (node);
      if (!*out_value)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Member '%s' is not a string", member_name);
          goto out;
        }
    }
  else
    *out_value = NULL;

  ret = TRUE;
 out:
  return ret;
}

static const char *
object_require_string_member (JsonObject     *object,
                              const char     *member_name,
                              GError        **error)
{
  const char *ret;
  if (!object_get_optional_string_member (object, member_name, &ret, error))
    return NULL;
  if (!ret)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Member '%s' not found", member_name);
      return NULL;
    }
  return ret;
}

static const char *
array_require_string_element (JsonArray      *array,
                              guint           i,
                              GError        **error)
{
  const char *ret = json_array_get_string_element (array, i);
  if (!ret)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Element at index %u is not a string", i);
      return NULL;
    }
  return ret;
}

static gboolean
append_string_array_to (JsonObject   *object,
                        const char   *member_name,
                        GPtrArray    *array,
                        GError      **error)
{
  JsonArray *jarray = json_object_get_array_member (object, member_name);
  guint i, len;

  if (!jarray)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No member '%s' found", member_name);
      return FALSE;
    }

  len = json_array_get_length (jarray);
  for (i = 0; i < len; i++)
    {
      const char *v = array_require_string_element (jarray, i, error);
      if (!v)
        return FALSE;
      g_ptr_array_add (array, g_strdup (v));
    }

  return TRUE;
}

typedef struct {
  GSSubprocess *process;
  GFile *tmp_reposdir_path;
  GDataOutputStream *stdin;
  /* GDataInputStream *stdout; */
} YumContext;

static gboolean
yum_context_close (YumContext   *yumctx,
                   GCancellable *cancellable,
                   GError      **error)
{
  gboolean ret = FALSE;

  if (!yumctx)
    return TRUE;

  if (yumctx->process)
    {
      if (yumctx->stdin)
        {
          if (!g_output_stream_close ((GOutputStream*)yumctx->stdin, cancellable, error))
            goto out;
          g_clear_object (&yumctx->stdin);
        }
      /*
      if (yumctx->stdout)
        {
          if (!g_input_stream_close ((GInputStream*)yumctx->stdout, cancellable, error))
            goto out;
          g_clear_object (&yumctx->stdout);
        }
      */
      
      g_print ("Waiting for yum...\n");
      if (!gs_subprocess_wait_sync_check (yumctx->process, cancellable, error))
        goto out;
      g_print ("Waiting for yum [OK]\n");
      g_clear_object (&yumctx->process);
    }

  ret = TRUE;
 out:
  return ret;
}

static void
yum_context_free (YumContext  *yumctx)
{
  if (!yumctx)
    return;
  (void) yum_context_close (yumctx, NULL, NULL);
  g_free (yumctx);
}

static inline
void cleanup_keyfile_unref (void *loc)
{
  GKeyFile *locp = *((GKeyFile**)loc);
  if (locp)
    g_key_file_unref (locp);
}

static gboolean
append_repo_and_cache_opts (RpmOstreeTreeComposeContext *self,
                            JsonObject *treedata,
                            GFile      *workdir,
                            GPtrArray  *args,
                            GCancellable *cancellable,
                            GError    **error)
{
  gboolean ret = FALSE;
  JsonArray *enable_repos = NULL;
  JsonArray *repos_data = NULL;
  guint i;
  gs_unref_object GFile *yumcache_lookaside = NULL;
  gs_unref_object GFile *repos_tmpdir = NULL;

  yumcache_lookaside = g_file_resolve_relative_path (workdir, "yum-cache");
  if (!gs_file_ensure_directory (yumcache_lookaside, TRUE, cancellable, error))
    goto out;

  repos_tmpdir = g_file_resolve_relative_path (workdir, "tmp-repos");
  if (!gs_shutil_rm_rf (repos_tmpdir, cancellable, error))
    goto out;
  if (!gs_file_ensure_directory (repos_tmpdir, TRUE, cancellable, error))
    goto out;

  if (g_getenv ("RPM_OSTREE_OFFLINE"))
    g_ptr_array_add (args, g_strdup ("-C"));

  {
    const char *proxy;
    if (opt_proxy)
      proxy = opt_proxy;
    else
      proxy = g_getenv ("http_proxy");
    if (proxy)
      g_ptr_array_add (args, g_strconcat ("--setopt=proxy=", proxy, NULL));
  }

  {
    GString *reposdir_value = g_string_new ("--setopt=reposdir=");
    gboolean first = TRUE;
    for (i = 0; i < self->treefile_context_dirs->len; i++)
      {
        GFile *contextdir = self->treefile_context_dirs->pdata[i];
        if (first)
          first = FALSE;
        else
          g_string_append_c (reposdir_value, ',');
        g_string_append (reposdir_value, gs_file_get_path_cached (contextdir));
      }
    g_ptr_array_add (args, g_string_free (reposdir_value, FALSE));
  }

  g_ptr_array_add (args, g_strdup ("--disablerepo=*"));

  if (json_object_has_member (treedata, "repos"))
    enable_repos = json_object_get_array_member (treedata, "repos");
  if (enable_repos)
    {
      guint i;
      guint n = json_array_get_length (enable_repos);
      for (i = 0; i < n; i++)
        {
          const char *reponame = array_require_string_element (enable_repos, i, error);
          if (!reponame)
            goto out;
          g_ptr_array_add (args, g_strconcat ("--enablerepo=", reponame, NULL));
        }
    }

  if (json_object_has_member (treedata, "repos_data"))
    repos_data = json_object_get_array_member (treedata, "repos_data");
  else
    repos_data = NULL;
  if (repos_data)
    {
      guint i;
      guint n = json_array_get_length (repos_data);

      if (n > 0)
        g_ptr_array_add (args, g_strconcat ("--setopt=reposdir=/etc/yum.repos.d,",
                                            gs_file_get_path_cached (repos_tmpdir),
                                            NULL));

      for (i = 0; i < n; i++)
        {
          const char *repodata = array_require_string_element (repos_data, i, error);
          __attribute__ ((cleanup(cleanup_keyfile_unref))) GKeyFile *keyfile = NULL;
          gs_strfreev char **groups = NULL;
          const char *reponame;
          gs_free char *rpmostree_reponame = NULL;
          gs_free char *rpmostree_repo_filename = NULL;
          gs_unref_object GFile *repo_tmp_file = NULL;
          gsize len;

          if (!repodata)
            goto out;

          keyfile = g_key_file_new ();
          if (!g_key_file_load_from_data (keyfile, repodata, -1, 0, error))
            {
              g_prefix_error (error, "Parsing keyfile data in repos_data: ");
              goto out;
            }

          groups = g_key_file_get_groups (keyfile, &len);
          if (len == 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No groups found in keyfile data in repos_data");
              goto out;
            }

          reponame = groups[0];
          g_assert (strchr (reponame, '/') == NULL);
          rpmostree_reponame = g_strconcat (reponame, NULL);
          rpmostree_repo_filename = g_strconcat (rpmostree_reponame, ".repo", NULL);
          repo_tmp_file = g_file_get_child (repos_tmpdir, rpmostree_repo_filename);

          if (!g_file_replace_contents (repo_tmp_file, repodata, strlen (repodata),
                                        NULL, FALSE, 0,
                                        NULL,
                                        cancellable, error))
            goto out;
          
          g_ptr_array_add (args, g_strconcat ("--enablerepo=", rpmostree_reponame, NULL));
        }
    }

  g_ptr_array_add (args, g_strdup ("--setopt=keepcache=0"));
  g_ptr_array_add (args, g_strconcat ("--setopt=cachedir=",
                                      gs_file_get_path_cached (yumcache_lookaside),
                                      NULL));

  ret = TRUE;
 out:
  return ret;
}

static YumContext *
yum_context_new (RpmOstreeTreeComposeContext  *self,
                 JsonObject     *treedata,
                 GFile          *yumroot,
                 GFile          *workdir,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean success = FALSE;
  YumContext *yumctx = NULL;
  GPtrArray *yum_argv = g_ptr_array_new_with_free_func (g_free);
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *yum_process = NULL;

  g_ptr_array_add (yum_argv, g_strdup ("yum"));
  g_ptr_array_add (yum_argv, g_strdup ("-y"));

  if (!append_repo_and_cache_opts (self, treedata, workdir, yum_argv,
                                   cancellable, error))
    goto out;

  g_ptr_array_add (yum_argv, g_strconcat ("--installroot=",
                                          gs_file_get_path_cached (yumroot),
                                          NULL));
  
  g_ptr_array_add (yum_argv, g_strdup ("shell"));

  g_ptr_array_add (yum_argv, NULL);

  context = gs_subprocess_context_new ((char**)yum_argv->pdata);
  {
    gs_strfreev char **duped_environ = g_get_environ ();

    duped_environ = g_environ_setenv (duped_environ, "OSTREE_KERNEL_INSTALL_NOOP", "1", TRUE);
    /* See fedora's kernel.spec */
    duped_environ = g_environ_setenv (duped_environ, "HARDLINK", "no", TRUE);

    gs_subprocess_context_set_environment (context, duped_environ);
  }

  gs_subprocess_context_set_stdin_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE);
  /* gs_subprocess_context_set_stdout_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE); */

  yumctx = g_new0 (YumContext, 1);

  {
    gs_free char *cmdline = subprocess_context_print_args (context);
    g_print ("Starting %s\n", cmdline);
  }
  yumctx->process = gs_subprocess_new (context, cancellable, error);
  if (!yumctx->process)
    goto out;

  yumctx->stdin = (GDataOutputStream*)g_data_output_stream_new (gs_subprocess_get_stdin_pipe (yumctx->process));
  /* yumctx->stdout = (GDataInputStream*)g_data_input_stream_new (gs_subprocess_get_stdout_pipe (yumctx->process)); */

  success = TRUE;
 out:
  if (!success)
    {
      yum_context_free (yumctx);
      return NULL;
    }
  return yumctx;
}

static gboolean
yum_context_command (YumContext   *yumctx,
                     const char   *cmd,
                     GPtrArray   **out_lines,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  gs_unref_ptrarray GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  gs_free char *cmd_nl = g_strconcat (cmd, "\n", NULL);

  g_print ("yum> %s", cmd_nl);
  if (!g_output_stream_write_all ((GOutputStream*)yumctx->stdin,
                                  cmd_nl, strlen (cmd_nl), &bytes_written,
                                  cancellable, error))
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_lines, &lines);
 out:
  return ret;
}
                  
static gboolean
yuminstall (RpmOstreeTreeComposeContext  *self,
            JsonObject      *treedata,
            GFile           *yumroot,
            GFile           *workdir,
            char           **packages,
            GCancellable    *cancellable,
            GError         **error)
{
  gboolean ret = FALSE;
  char **strviter;
  YumContext *yumctx;

  yumctx = yum_context_new (self, treedata, yumroot, workdir, cancellable, error);
  if (!yumctx)
    goto out;

  for (strviter = packages; strviter && *strviter; strviter++)
    {
      gs_free char *cmd = NULL;
      const char *package = *strviter;
      gs_unref_ptrarray GPtrArray *lines = NULL;

      if (g_str_has_prefix (package, "@"))
        cmd = g_strconcat ("group install ", package, NULL);
      else
        cmd = g_strconcat ("install ", package, NULL);
        
      if (!yum_context_command (yumctx, cmd, &lines,
                                cancellable, error))
        goto out;
    }

  {
    gs_unref_ptrarray GPtrArray *lines = NULL;
    if (!yum_context_command (yumctx, "run", &lines,
                              cancellable, error))
      goto out;
  }

  if (!yum_context_close (yumctx, cancellable, error))
    goto out;

  ret = TRUE;
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

  if (!object_get_optional_string_member (root, "include", &include_path, error))
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
    }

  ret = TRUE;
 out:
  return ret;
}

static char *
cachedir_fssafe_key (const char  *primary_key)
{
  GString *ret = g_string_new ("");
  
  for (; *primary_key; primary_key++)
    {
      const char c = *primary_key;
      if (!g_ascii_isprint (c) || c == '-')
        g_string_append_printf (ret, "\\%02x", c);
      else if (c == '/')
        g_string_append_c (ret, '-');
      else
        g_string_append_c (ret, c);
    }

  return g_string_free (ret, FALSE);
}

static GFile *
cachedir_keypath (GFile         *cachedir,
                  const char    *primary_key)
{
  gs_free char *fssafe_key = cachedir_fssafe_key (primary_key);
  return g_file_get_child (cachedir, fssafe_key);
}

static gboolean
cachedir_lookup_string (GFile             *cachedir,
                        const char        *key,
                        char             **out_value,
                        GCancellable      *cancellable,
                        GError           **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_value = NULL;

  if (cachedir)
    {
      gs_unref_object GFile *keypath = cachedir_keypath (cachedir, key);
      if (!_rpmostree_file_load_contents_utf8_allow_noent (keypath, &ret_value,
                                                           cancellable, error))
        goto out;
    }
  
  ret = TRUE;
  gs_transfer_out_value (out_value, &ret_value);
 out:
  return ret;
}

static gboolean
cachedir_set_string (GFile             *cachedir,
                     const char        *key,
                     const char        *value,
                     GCancellable      *cancellable,
                     GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *keypath = NULL;

  if (!cachedir)
    return TRUE;

  keypath = cachedir_keypath (cachedir, key);
  if (!g_file_replace_contents (keypath, value, strlen (value), NULL,
                                FALSE, 0, NULL,
                                cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
compute_checksum_for_compose (JsonObject   *treefile_rootval,
                              GFile        *yumroot,
                              char        **out_checksum,
                              GCancellable *cancellable,
                              GError      **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_checksum = NULL;
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
  
  {
    JsonNode *treefile_rootnode = json_node_new (JSON_NODE_OBJECT);
    gs_unref_object JsonGenerator *generator = json_generator_new ();
    gs_free char *treefile_buf = NULL;
    gsize len;

    json_node_set_object (treefile_rootnode, treefile_rootval);
    json_generator_set_root (generator, treefile_rootnode);
    treefile_buf = json_generator_to_data (generator, &len);
    
    g_checksum_update (checksum, (guint8*)treefile_buf, len);

    json_node_free (treefile_rootnode);
  }

  /* Query the generated rpmdb, to see if anything has changed. */
  {
    int estatus;
    gs_free char *yumroot_var_lib_rpm =
      g_build_filename (gs_file_get_path_cached (yumroot),
                        "var/lib/rpm",
                        NULL);
    const char *rpmqa_argv[] = { PKGLIBDIR "/rpmqa-sorted-and-clean",
                                 yumroot_var_lib_rpm,
                                 NULL };
    gs_free char *rpmqa_result = NULL;

    if (!g_spawn_sync (NULL, (char**)rpmqa_argv, NULL,
                       G_SPAWN_SEARCH_PATH, NULL, NULL,
                       &rpmqa_result, NULL, &estatus, error))
      goto out;
    if (!g_spawn_check_exit_status (estatus, error))
      {
        g_prefix_error (error, "Executing %s: ",
                        rpmqa_argv[0]);
        goto out;
      }

    if (!*rpmqa_result)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Empty result from %s", rpmqa_argv[0]);
        goto out;
      }
    
    g_checksum_update (checksum, (guint8*)rpmqa_result, strlen (rpmqa_result));
  }

  ret_checksum = g_strdup (g_checksum_get_string (checksum));

  ret = TRUE;
  gs_transfer_out_value (out_checksum, &ret_checksum);
 out:
  if (checksum) g_checksum_free (checksum);
  return ret;
}

gboolean
rpmostree_compose_builtin_tree (int             argc,
                                char          **argv,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Run yum and commit the result to an OSTree repository");
  const char *ref;
  RpmOstreeTreeComposeContext selfdata = { NULL, };
  RpmOstreeTreeComposeContext *self = &selfdata;
  JsonNode *treefile_rootval = NULL;
  JsonObject *treefile = NULL;
  JsonArray *units = NULL;
  guint len;
  guint i;
  gs_free char *ref_unix = NULL;
  gs_free char *cachekey = NULL;
  gs_free char *cached_compose_checksum = NULL;
  gs_free char *new_compose_checksum = NULL;
  gs_unref_object GFile *workdir = NULL;
  gs_unref_object GFile *cachedir = NULL;
  gs_unref_object GFile *yumroot = NULL;
  gs_unref_object GFile *targetroot = NULL;
  gs_unref_object GFile *yumroot_varcache = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *bootstrap_packages = NULL;
  gs_unref_ptrarray GPtrArray *packages = NULL;
  gs_unref_object GFile *treefile_path = NULL;
  gs_unref_object GFile *repo_path = NULL;
  gs_unref_object JsonParser *treefile_parser = NULL;
  gboolean workdir_is_tmp = FALSE;

  self->treefile_context_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_printerr ("usage: rpm-ostree compose tree TREEFILE\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Option processing failed");
      goto out;
    }
  
  if (!opt_repo)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "--repo must be specified");
      goto out;
    }

  repo_path = g_file_new_for_path (opt_repo);
  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_open (repo, cancellable, error))
    goto out;
  
  treefile_path = g_file_new_for_path (argv[1]);

  if (opt_workdir)
    {
      workdir = g_file_new_for_path (opt_workdir);
    }
  else
    {
      gs_free char *tmpd = g_mkdtemp (g_strdup ("/var/tmp/rpm-ostree.XXXXXX"));
      workdir = g_file_new_for_path (tmpd);
      workdir_is_tmp = TRUE;

      if (opt_workdir_tmpfs)
        {
          /* Use a private mount namespace to avoid polluting the global
           * namespace, and to ensure the mount gets cleaned up if we exit
           * unexpectedly.
           */
          if (unshare (CLONE_NEWNS) != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "unshare(CLONE_NEWNS): %s", g_strerror (errno));
              goto out;
            }
          if (mount (NULL, "/", "none", MS_PRIVATE | MS_REC, NULL) == -1)
            {
              int errsv = errno;
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "mount(/, MS_PRIVATE | MS_REC): %s",
                           g_strerror (errsv));
              goto out;
            }
      
          if (mount ("tmpfs", tmpd, "tmpfs", 0, (const void*)"mode=755") != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "mount(tmpfs): %s", g_strerror (errno));
              goto out;
            }
        }
    }

  if (opt_cachedir)
    {
      cachedir = g_file_new_for_path (opt_cachedir);
      if (!gs_file_ensure_directory (cachedir, FALSE, cancellable, error))
        goto out;
    }

  if (chdir (gs_file_get_path_cached (workdir)) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to chdir to '%s': %s",
                   opt_workdir, strerror (errno));
      goto out;
    }

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
      
      ret = TRUE;
      goto out;
    }

  yumroot = g_file_get_child (workdir, "rootfs.tmp");
  if (!gs_shutil_rm_rf (yumroot, cancellable, error))
    goto out;
  targetroot = g_file_get_child (workdir, "rootfs");

  ref = object_require_string_member (treefile, "ref", error);
  if (!ref)
    goto out;

  ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

  bootstrap_packages = g_ptr_array_new ();
  packages = g_ptr_array_new ();

  if (!append_string_array_to (treefile, "bootstrap_packages", packages, error))
    goto out;
  if (!append_string_array_to (treefile, "packages", packages, error))
    goto out;
  g_ptr_array_add (packages, NULL);

  if (!yuminstall (self, treefile, yumroot, workdir,
                   (char**)packages->pdata,
                   cancellable, error))
    goto out;

  cachekey = g_strconcat ("treecompose/", ref, NULL);
  if (!cachedir_lookup_string (cachedir, cachekey,
                               &cached_compose_checksum,
                               cancellable, error))
    goto out;
  
  if (!compute_checksum_for_compose (treefile, yumroot,
                                     &new_compose_checksum,
                                     cancellable, error))
    goto out;

  if (g_strcmp0 (cached_compose_checksum, new_compose_checksum) == 0)
    {
      g_print ("No changes to input, reusing cached commit\n");
      ret = TRUE;
      goto out;
    }

  ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    goto out;

  if (!rpmostree_postprocess (yumroot, cancellable, error))
    goto out;

  if (json_object_has_member (treefile, "units"))
    units = json_object_get_array_member (treefile, "units");

  if (units)
    len = json_array_get_length (units);
  else
    len = 0;

  {
    gs_unref_object GFile *multiuser_wants_dir =
      g_file_resolve_relative_path (yumroot, "usr/etc/systemd/system/multi-user.target.wants");

    if (!gs_file_ensure_directory (multiuser_wants_dir, TRUE, cancellable, error))
      goto out;

    for (i = 0; i < len; i++)
      {
        const char *unitname = array_require_string_element (units, i, error);
        gs_unref_object GFile *unit_link_target = NULL;
        gs_free char *symlink_target = NULL;

        if (!unitname)
          goto out;

        symlink_target = g_strconcat ("/usr/lib/systemd/system/", unitname, NULL);
        unit_link_target = g_file_get_child (multiuser_wants_dir, unitname);

        if (g_file_query_file_type (unit_link_target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
          continue;
          
        g_print ("Adding %s to multi-user.target.wants\n", unitname);

        if (!g_file_make_symbolic_link (unit_link_target, symlink_target,
                                        cancellable, error))
          goto out;
      }
  }

  {
    gs_unref_object GFile *target_treefile_dir_path =
      g_file_resolve_relative_path (yumroot, "usr/share/rpm-ostree");
    gs_unref_object GFile *target_treefile_path =
      g_file_get_child (target_treefile_dir_path, "treefile.json");
      
    if (!gs_file_ensure_directory (target_treefile_dir_path, TRUE,
                                   cancellable, error))
      goto out;
                                     
    g_print ("Copying '%s' to '%s'\n",
             gs_file_get_path_cached (treefile_path),
             gs_file_get_path_cached (target_treefile_path));
    if (!g_file_copy (treefile_path, target_treefile_path,
                      G_FILE_COPY_TARGET_DEFAULT_PERMS,
                      cancellable, NULL, NULL, error))
      goto out;
  }

  {
    const char *default_target = NULL;
      
    if (!object_get_optional_string_member (treefile, "default_target",
                                            &default_target, error))
      goto out;

    if (default_target != NULL)
      {
        gs_unref_object GFile *default_target_path =
          g_file_resolve_relative_path (yumroot, "usr/etc/systemd/system/default.target");

        (void) gs_file_unlink (default_target_path, NULL, NULL);
        
        if (!g_file_make_symbolic_link (default_target_path, default_target,
                                        cancellable, error))
          goto out;
      }
  }
    
  {
    const char *gpgkey;
    if (!object_get_optional_string_member (treefile, "gpg_key", &gpgkey, error))
      goto out;

    if (!rpmostree_commit (yumroot, repo, ref, gpgkey,
                           json_object_get_boolean_member (treefile, "selinux"),
                           cancellable, error))
      goto out;
  }

  if (!cachedir_set_string (cachedir, cachekey,
                            new_compose_checksum,
                            cancellable, error))
    goto out;

  g_print ("Complete\n");
  
 out:

  if (workdir_is_tmp)
    {
      if (opt_workdir_tmpfs)
        (void) umount (gs_file_get_path_cached (workdir));
      (void) gs_shutil_rm_rf (workdir, NULL, NULL);
    }
  if (self)
    {
      g_ptr_array_unref (self->treefile_context_dirs);
    }
  return ret;
}
