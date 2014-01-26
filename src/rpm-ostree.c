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

#include <ostree.h>
#include "libgsystem.h"

static char **opt_enable_repos;
static char *opt_workdir;
static char **opt_bootstrap_packages;

static GOptionEntry option_entries[] = {
  { "bootstrap-package", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bootstrap_packages, "Install this package first", "PACKAGE" },
  { "enablerepo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_repos, "Repositories to enable", "REPO" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "REPO" },
  { NULL }
};

static char *
c_stringify (const guint8   *buf,
             gsize           len)
{
  GString *ret = g_string_new ("");
  gsize i;

  for (i = 0; i < len; i++)
    {
      guint8 c = buf[i];
      if (c == '\\')
        g_string_append (ret, "\\\\");
      else if (g_ascii_isprint (c))
        g_string_append_c (ret, c);
      else
        g_string_append_printf (ret, "\\x%02x", c);
    }

  return g_string_free (ret, FALSE);
}

static gboolean
replace_nsswitch (GFile         *target_usretc,
                  GCancellable  *cancellable,
                  GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *nsswitch_conf =
    g_file_get_child (target_usretc, "nsswitch.conf");
  gs_free char *nsswitch_contents = NULL;
  gs_free char *new_nsswitch_contents = NULL;

  static gsize regex_initialized;
  static GRegex *passwd_regex;

  if (g_once_init_enter (&regex_initialized))
    {
      passwd_regex = g_regex_new ("^(passwd|group):\\s+files(.*)$",
                                  G_REGEX_MULTILINE, 0, NULL);
      g_assert (passwd_regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  nsswitch_contents = gs_file_load_contents_utf8 (nsswitch_conf, cancellable, error);
  if (!nsswitch_contents)
    goto out;

  new_nsswitch_contents = g_regex_replace (passwd_regex,
                                           nsswitch_contents, -1, 0,
                                           "\\1: files altfiles\\2",
                                           0, error);
  if (!new_nsswitch_contents)
    goto out;

  if (!g_file_replace_contents (nsswitch_conf, new_nsswitch_contents,
                                strlen (new_nsswitch_contents),
                                NULL, FALSE, 0, NULL,
                                cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  GSSubprocess *process;
  GFile *reposdir_path;
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

  if (yumctx->tmp_reposdir_path)
    {
      if (!gs_file_rename (yumctx->tmp_reposdir_path, yumctx->reposdir_path,
                           cancellable, error))
        goto out;
      g_clear_object (&yumctx->reposdir_path);
      g_clear_object (&yumctx->tmp_reposdir_path);
    }
  
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

static void
append_repo_opts (GPtrArray  *args)
{
  char **strviter;

  if (g_getenv ("RPM_OSTREE_OFFLINE"))
    g_ptr_array_add (args, g_strdup ("-C"));

  g_ptr_array_add (args, g_strdup ("--disablerepo=*"));
  for (strviter = opt_enable_repos; strviter && *strviter; strviter++)
    g_ptr_array_add (args, g_strconcat ("--enablerepo=", *strviter, NULL));
}

static YumContext *
yum_context_new (GFile          *yumroot,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean success = FALSE;
  YumContext *yumctx = NULL;
  GPtrArray *yum_argv = g_ptr_array_new_with_free_func (g_free);
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *yum_process = NULL;
  gs_unref_object GFile *reposdir_path = NULL;

  g_ptr_array_add (yum_argv, g_strdup ("yum"));
  g_ptr_array_add (yum_argv, g_strdup ("-y"));
  g_ptr_array_add (yum_argv, g_strdup ("--setopt=keepcache=1"));
  g_ptr_array_add (yum_argv, g_strconcat ("--installroot=",
                                          gs_file_get_path_cached (yumroot),
                                          NULL));

  append_repo_opts (yum_argv);
  
  g_ptr_array_add (yum_argv, g_strdup ("shell"));

  g_ptr_array_add (yum_argv, NULL);

  context = gs_subprocess_context_new ((char**)yum_argv->pdata);
  {
    gs_strfreev char **duped_environ = g_get_environ ();

    duped_environ = g_environ_setenv (duped_environ, "OSTREE_KERNEL_INSTALL_NOOP", "1", TRUE);

    gs_subprocess_context_set_environment (context, duped_environ);
  }

  reposdir_path = g_file_resolve_relative_path (yumroot, "etc/yum.repos.d");
  /* Hideous workaround for the fact that as soon as yum.repos.d
     exists in the install root, yum will prefer it. */
  if (g_file_query_exists (reposdir_path, NULL))
    {
      yumctx->reposdir_path = g_object_ref (reposdir_path);
      yumctx->tmp_reposdir_path = g_file_resolve_relative_path (yumroot, "etc/yum.repos.d.tmp");
      if (!gs_file_rename (reposdir_path, yumctx->tmp_reposdir_path,
                           cancellable, error))
        goto out;
    }

  gs_subprocess_context_set_stdin_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE);
  /* gs_subprocess_context_set_stdout_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE); */

  yumctx = g_new0 (YumContext, 1);

  g_print ("Starting yum...\n");
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

  /*
  do
    {
      const guint8 *buf;
      gsize len;
      GBytes *bytes;
      const guint8 *nl;

      bytes = g_input_stream_read_bytes ((GInputStream*)yumctx->stdout, 8192,
                                         cancellable, error);
      if (!bytes)
        goto out;
      buf = g_bytes_get_data (bytes, &len);
      if (g_bytes_get_size (bytes) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Received unexpected EOF from yum!");
          goto out;
        }

      while ((nl = memchr (buf, '\n', len)) != NULL)
        {
          gsize linelen = nl - buf;
          char *line;
          if (!g_utf8_validate ((char*)buf, linelen, NULL))
            {
              gs_free char *cstr = c_stringify (buf, linelen);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid UTF-8 from yum: %s", cstr);
              goto out;
            }
          line = g_strndup ((char*)buf, linelen);
          g_print ("yum< %s\n", line);
          g_ptr_array_add (lines, line);
          len -= (linelen+1);
          buf = nl + 1;
        }

      if (len < 2 || memcmp (buf, "> ", 2) != 0)
        {
          gs_free char *cstr = c_stringify (buf, len);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unexpected end of data '%s'", cstr);
          goto out;
        }
    }
  while (TRUE);
  */

  ret = TRUE;
  gs_transfer_out_value (out_lines, &lines);
 out:
  return ret;
}
                  
static gboolean
yuminstall (GFile           *yumroot,
            char           **packages,
            GCancellable    *cancellable,
            GError         **error)
{
  gboolean ret = FALSE;
  char **strviter;
  YumContext *yumctx;

  yumctx = yum_context_new (yumroot, cancellable, error);
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

int
main (int     argc,
      char  **argv)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  GOptionContext *context = g_option_context_new ("- Run yum and commit the result to an OSTree repository");
  const char *cmd;
  const char *ref;
  gs_free char *ref_unix = NULL;
  gs_unref_object GFile *cachedir = NULL;
  gs_unref_object GFile *yumroot = NULL;
  gs_unref_object GFile *targetroot = NULL;
  gs_unref_object GFile *yumcachedir = NULL;
  gs_unref_object GFile *yumcache_lookaside = NULL;
  gs_unref_object GFile *yumroot_varcache = NULL;
  gs_unref_ptrarray GPtrArray *all_packages = NULL;
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 4)
    {
      g_printerr ("usage: %s create REFNAME PACKAGE [PACKAGE...]\n", argv[0]);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Option processing failed");
      goto out;
    }
  
  cmd = argv[1];
  if (strcmp (cmd, "create") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown command '%s'", cmd);
      goto out;
    }
  ref = argv[2];

  if (opt_workdir)
    {
      if (chdir (opt_workdir) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to chdir to '%s': %s",
                       opt_workdir, strerror (errno));
          goto out;
        }
    }

  cachedir = g_file_new_for_path ("cache");
  if (!gs_file_ensure_directory (cachedir, TRUE, cancellable, error))
    goto out;

  yumroot = g_file_get_child (cachedir, "yum");
  targetroot = g_file_resolve_relative_path (cachedir, "rootfs");
  yumcachedir = g_file_resolve_relative_path (yumroot, "var/cache/yum");
  yumcache_lookaside = g_file_resolve_relative_path (cachedir, "yum-cache");
  
  if (!gs_shutil_rm_rf (yumroot, cancellable, error))
    goto out;
  yumroot_varcache = g_file_resolve_relative_path (yumroot, "var/cache");
  if (g_file_query_exists (yumcache_lookaside, NULL))
    {
      g_print ("Reusing cache: %s\n", gs_file_get_path_cached (yumroot_varcache));
      if (!gs_file_ensure_directory (yumroot_varcache, TRUE, cancellable, error))
        goto out;
      if (!gs_shutil_cp_al_or_fallback (yumcache_lookaside, yumcachedir,
                                        cancellable, error))
        goto out;
    }
  else
    {
      g_print ("No cache found at: %s\n", gs_file_get_path_cached (yumroot_varcache));
    }

  ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

  all_packages = g_ptr_array_new ();
  if (opt_bootstrap_packages)
    {
      char **strviter;
      for (strviter = opt_bootstrap_packages; strviter && *strviter; strviter++)
        g_ptr_array_add (all_packages, *strviter);
    }

  {
    guint i;
    for (i = 3; i < argc; i++)
      g_ptr_array_add (all_packages, argv[i]);
  }
    
  g_ptr_array_add (all_packages, NULL);

  {
    gs_free char *cached_packageset_name =
      g_strconcat ("packageset-", ref_unix, ".txt", NULL);
    gs_unref_object GFile *rpmtextlist_path = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name);
    gs_free char *cached_packageset_name_new =
      g_strconcat (cached_packageset_name, ".new", NULL);
    gs_unref_object GFile *rpmtextlist_path_new = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name_new);
    GPtrArray *repoquery_argv = g_ptr_array_new_with_free_func (g_free);
    gs_unref_object GSSubprocessContext *repoquery_proc_ctx = NULL;
    gs_unref_object GSSubprocess *repoquery_proc = NULL;
    guint i;

    g_ptr_array_add (repoquery_argv, g_strdup (PKGLIBDIR "/repoquery-sorted"));
    append_repo_opts (repoquery_argv);
    g_ptr_array_add (repoquery_argv, g_strdup ("--recursive"));
    g_ptr_array_add (repoquery_argv, g_strdup ("--requires"));
    g_ptr_array_add (repoquery_argv, g_strdup ("--resolve"));

    for (i = 0; i < all_packages->len; i++)
      {
        const char *package = all_packages->pdata[i];
        if (!package)
          continue;

        g_ptr_array_add (repoquery_argv, g_strdup (package));
      }

    g_ptr_array_add (repoquery_argv, NULL);
      
    repoquery_proc_ctx = gs_subprocess_context_new ((char**)repoquery_argv->pdata);
    gs_subprocess_context_set_stdout_file_path (repoquery_proc_ctx, gs_file_get_path_cached (rpmtextlist_path_new));
    gs_subprocess_context_set_stderr_disposition (repoquery_proc_ctx, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT);
    g_print ("Resolving dependencies...\n");
    repoquery_proc = gs_subprocess_new (repoquery_proc_ctx, cancellable, error);
    if (!repoquery_proc)
      goto out;

    if (!gs_subprocess_wait_sync_check (repoquery_proc, cancellable, error))
      goto out;
    
    if (g_file_query_exists (rpmtextlist_path, NULL))
      {
        GError *temp_error = NULL;
        gs_unref_object GSSubprocess *diff_proc = NULL;
        gboolean differs = FALSE;

        g_print ("Comparing diff of previous tree\n");
        diff_proc =
          gs_subprocess_new_simple_argl (GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         cancellable,
                                         error,
                                         "diff", "-u",
                                         gs_file_get_path_cached (rpmtextlist_path),
                                         gs_file_get_path_cached (rpmtextlist_path_new),
                                         NULL);

        if (!gs_subprocess_wait_sync_check (diff_proc, cancellable, &temp_error))
          {
            int ecode;
            if (temp_error->domain != G_SPAWN_EXIT_ERROR)
              {
                g_propagate_error (error, temp_error);
                goto out;
              }
            ecode = temp_error->code;
            g_assert (ecode != 0);
            if (ecode == 1)
              differs = TRUE;
            else
              {
                g_propagate_error (error, temp_error);
                goto out;
              }
          }

        if (!differs)
          {
            g_print ("No changes in package set\n");
            if (!gs_file_unlink (rpmtextlist_path_new, cancellable, error))
              goto out;
            goto out;
          }
      }
    else
      {
        g_print ("No previous diff file found at '%s'\n",
                 gs_file_get_path_cached (rpmtextlist_path));
      }

    /* Ensure we have enough to modify NSS */
    if (opt_bootstrap_packages)
      {
        if (!yuminstall (yumroot, opt_bootstrap_packages, cancellable, error))
          goto out;
      }

    /* Prepare NSS configuration; this needs to be done
       before any invocations of "useradd" in %post */

    {
      gs_unref_object GFile *yumroot_passwd =
        g_file_resolve_relative_path (yumroot, "usr/lib/passwd");
      gs_unref_object GFile *yumroot_group =
        g_file_resolve_relative_path (yumroot, "usr/lib/group");
      gs_unref_object GFile *yumroot_etc = 
        g_file_resolve_relative_path (yumroot, "etc");

      if (!g_file_replace_contents (yumroot_passwd, "", 0, NULL, FALSE, 0,
                                    NULL, cancellable, error))
        goto out;
      if (!g_file_replace_contents (yumroot_group, "", 0, NULL, FALSE, 0,
                                    NULL, cancellable, error))
        goto out;

      if (!replace_nsswitch (yumroot_etc, cancellable, error))
        goto out;
    }

    {
      if (!yuminstall (yumroot, (char**)all_packages->pdata,
                       cancellable, error))
        goto out;
    }

    ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

    /* Attempt to cache stuff between runs */
    if (!gs_shutil_rm_rf (yumcache_lookaside, cancellable, error))
      goto out;

    g_print ("Saving yum cache %s\n", gs_file_get_path_cached (yumcache_lookaside));
    if (!gs_file_rename (yumcachedir, yumcache_lookaside,
                         cancellable, error))
      goto out;

    {
    }
  
    {
      /* OSTree commit messages aren't really useful. */
      const char *commit_message = "";
      if (!gs_subprocess_simple_run_sync (NULL,
                                          GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                          cancellable,
                                          error,
                                          "rpm-ostree-postprocess-and-commit",
                                          "--repo=repo",
                                          "-m", commit_message,
                                          gs_file_get_path_cached (yumroot),
                                          ref,
                                          NULL))
        goto out;
    }

    if (!gs_file_rename (rpmtextlist_path_new, rpmtextlist_path,
                         cancellable, error))
      goto out;
  }

  g_print ("Complete\n");
  
 out:
  if (local_error != NULL)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_printerr ("%serror: %s%s\n", prefix, suffix, local_error->message);
      g_error_free (local_error);
      return 2;
    }
  else
    return 0;
}
