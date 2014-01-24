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

static GOptionEntry option_entries[] = {
  { "enablerepo", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_repos, "Repositories to enable", "REPO" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "REPO" },
  { NULL }
};

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

static char *
strconcat_push_malloced (GPtrArray *malloced,
                         ...) G_GNUC_NULL_TERMINATED;

static char *
strconcat_push_malloced (GPtrArray *malloced,
                         ...)
{
  va_list args;
  GString *buf = g_string_new ("");
  const char *p;
  char *ret;

  va_start (args, malloced);

  while ((p = va_arg (args, const char *)) != NULL)
    g_string_append (buf, p);

  va_end (args);

  ret = g_string_free (buf, FALSE);
  g_ptr_array_add (malloced, buf);
  return ret;
}
    
static gboolean
run_yum (GFile          *yumroot,
         char          **argv,
         const char     *stdin_str,
         GCancellable   *cancellable,
         GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *malloced_argv = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *yum_argv = g_ptr_array_new ();
  char **strviter;
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *yum_process = NULL;
  gs_unref_object GFile *reposdir_path = NULL;
  gs_unref_object GFile *tmp_reposdir_path = NULL;
  GOutputStream *stdin_pipe = NULL;
  GMemoryInputStream *stdin_data = NULL;

  g_ptr_array_add (yum_argv, "yum");
  g_ptr_array_add (yum_argv, "-y");
  g_ptr_array_add (yum_argv, "--setopt=keepcache=1");
  g_ptr_array_add (yum_argv, strconcat_push_malloced (malloced_argv,
                                                      "--installroot=",
                                                      gs_file_get_path_cached (yumroot),
                                                      NULL));
  g_ptr_array_add (yum_argv, "--disablerepo=*");
  
  for (strviter = opt_enable_repos; strviter && *strviter; strviter++)
    {
      g_ptr_array_add (yum_argv, strconcat_push_malloced (malloced_argv,
                                                          "--enablerepo=",
                                                          *strviter,
                                                          NULL));
    }
  
  for (strviter = argv; strviter && *strviter; strviter++)
    g_ptr_array_add (yum_argv, *strviter);

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
      tmp_reposdir_path = g_file_resolve_relative_path (yumroot, "etc/yum.repos.d.tmp");
      if (!gs_file_rename (reposdir_path, tmp_reposdir_path,
                           cancellable, error))
        goto out;
    }

  if (stdin_str)
    gs_subprocess_context_set_stdin_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE);

  yum_process = gs_subprocess_new (context, cancellable, error);
  if (!yum_process)
    goto out;

  if (stdin_str)
    {
      stdin_pipe = gs_subprocess_get_stdin_pipe (yum_process);
      g_assert (stdin_pipe);
      
      stdin_data = (GMemoryInputStream*)g_memory_input_stream_new_from_data (stdin_str,
                                                                             strlen (stdin_str),
                                                                             NULL);
      
      if (0 > g_output_stream_splice (stdin_pipe, (GInputStream*)stdin_data,
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                      cancellable, error))
        goto out;
    }

  if (!gs_subprocess_wait_sync_check (yum_process, cancellable, error))
    goto out;

  if (tmp_reposdir_path)
    {
      if (!gs_file_rename (tmp_reposdir_path, reposdir_path,
                           cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
yuminstall (GFile   *yumroot,
            char   **packages,
            GCancellable *cancellable,
            GError      **error)
{
  gboolean ret = FALSE;
  GString *buf = g_string_new ("");
  char **strviter;
  char *yumargs[] = { "shell", NULL };

  g_string_append (buf, "makecache fast\n");

  for (strviter = packages; strviter && *strviter; strviter++)
    {
      const char *package = *strviter;
      if (g_str_has_prefix (package, "@"))
        g_string_append_printf (buf, "group install %s\n", package);
      else
        g_string_append_printf (buf, "install %s\n", package);
    }
  g_string_append (buf, "run\n");

  if (!run_yum (yumroot, yumargs, buf->str,
                cancellable, error))
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
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 3)
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

  /* Ensure we have enough to modify NSS */
  {
    char *packages_for_nss[] = {"filesystem", "glibc", "nss-altfiles", "shadow-utils", NULL };
    if (!yuminstall (yumroot, packages_for_nss, cancellable, error))
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
    GPtrArray *packages = g_ptr_array_new ();
    int i;
    for (i = 3; i < argc; i++)
      g_ptr_array_add (packages, argv[i - 1]);
    
    g_ptr_array_add (packages, NULL);

    if (!yuminstall (yumroot, (char**)packages->pdata,
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
    gs_unref_object GFile *yumroot_rpmlibdir = 
      g_file_resolve_relative_path (yumroot, "var/lib/rpm");
    gs_free char *cached_packageset_name =
      g_strconcat ("packageset-", ref_unix, ".txt", NULL);
    gs_unref_object GFile *rpmtextlist_path = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name);
    gs_free char *cached_packageset_name_new =
      g_strconcat (cached_packageset_name, ".new", NULL);
    gs_unref_object GFile *rpmtextlist_path_new = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name_new);
    char *rpmqa_argv[] = { PKGLIBDIR "/rpmqa-sorted", NULL };
    gs_unref_object GSSubprocessContext *rpmqa_proc_ctx = NULL;
    gs_unref_object GSSubprocess *rpmqa_proc = NULL;
    gboolean differs = TRUE;
      
    rpmqa_proc_ctx = gs_subprocess_context_new (rpmqa_argv);
    gs_subprocess_context_set_stdout_file_path (rpmqa_proc_ctx, gs_file_get_path_cached (rpmtextlist_path_new));
    gs_subprocess_context_set_stderr_disposition (rpmqa_proc_ctx, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT);
    rpmqa_proc = gs_subprocess_new (rpmqa_proc_ctx, cancellable, error);
    if (!rpmqa_proc)
      goto out;
    
    if (g_file_query_exists (rpmtextlist_path, NULL))
      {
        GError *temp_error = NULL;
        gs_unref_object GSSubprocess *diff_proc = NULL;

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
            goto out;
          }
      }

    if (!gs_file_rename (rpmtextlist_path_new, rpmtextlist_path,
                         cancellable, error))
      goto out;
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
