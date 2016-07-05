/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

#include <gio/gio.h>
#include <systemd/sd-journal.h>
#include "rpmostree-output.h"
#include <err.h>
#include "libglnx.h"

#include "rpmostree-scripts.h"

typedef struct {
    const char *desc;
    rpmsenseFlags sense;
    rpmTagVal tag;
    rpmTagVal progtag;
    rpmTagVal flagtag;
} KnownRpmScriptKind;

#if 0
static const KnownRpmScriptKind ignored_scripts[] = {
  /* Ignore all of the *un variants since we never uninstall
   * anything in the RPM sense.
   */
  { "%preun", 0,
    RPMTAG_PREUN, RPMTAG_PREUNPROG, RPMTAG_PREUNFLAGS },
  { "%postun", 0,
    RPMTAG_POSTUN, RPMTAG_POSTUNPROG, RPMTAG_POSTUNFLAGS },
  { "%triggerun", RPMSENSE_TRIGGERUN,
    RPMTAG_TRIGGERUN, 0, 0 },
  { "%triggerpostun", RPMSENSE_TRIGGERPOSTUN,
    RPMTAG_TRIGGERPOSTUN, 0, 0 },
};
#endif

static const KnownRpmScriptKind posttrans_scripts[] = {
  /* For now, we treat %post as equivalent to %posttrans */
  { "%post", 0,
    RPMTAG_POSTIN, RPMTAG_POSTINPROG, RPMTAG_POSTINFLAGS },
  { "%posttrans", 0,
    RPMTAG_POSTTRANS, RPMTAG_POSTTRANSPROG, RPMTAG_POSTTRANSFLAGS },
};

static const KnownRpmScriptKind unsupported_scripts[] = {
  { "%prein", 0,
    RPMTAG_PREIN, RPMTAG_PREINPROG, RPMTAG_PREINFLAGS },
  { "%pretrans", 0,
    RPMTAG_PRETRANS, RPMTAG_PRETRANSPROG, RPMTAG_PRETRANSFLAGS },
  { "%triggerprein", RPMSENSE_TRIGGERPREIN,
    RPMTAG_TRIGGERPREIN, 0, 0 },
  { "%triggerin", RPMSENSE_TRIGGERIN,
    RPMTAG_TRIGGERIN, 0, 0 },
  { "%verify", 0,
    RPMTAG_VERIFYSCRIPT, RPMTAG_VERIFYSCRIPTPROG, RPMTAG_VERIFYSCRIPTFLAGS},
};

static void
child_setup_fchdir (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  if (fchdir (fd) < 0)
    err (1, "fchdir");
}

static void
add_const_args (GPtrArray *argv_array, ...)
{
  va_list args;
  char *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, char *)))
    g_ptr_array_add (argv_array, arg);
  va_end (args);
}

static void
fusermount_cleanup (const char *mountpoint)
{
  g_autoptr(GError) tmp_error = NULL;
  const char *fusermount_argv[] = { "fusermount", "-u", mountpoint, NULL};
  int estatus;

  if (!g_spawn_sync (NULL, (char**)fusermount_argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, &tmp_error))
    {
      g_prefix_error (&tmp_error, "Executing fusermount: ");
      goto out;
    }
  if (!g_spawn_check_exit_status (estatus, &tmp_error))
    {
      g_prefix_error (&tmp_error, "Executing fusermount: ");
      goto out;
    }

 out:
  /* We don't want a failure to unmount to be fatal, so all we do here
   * is log.  Though in practice what we *really* want is for the
   * fusermount to be in the bwrap namespace, and hence tied by the
   * kernel to the lifecycle of the container.  This would require
   * special casing for somehow doing FUSE mounts in bwrap.  Which
   * would be hard because NO_NEW_PRIVS turns off the setuid bits for
   * fuse.
   */
  if (tmp_error)
    sd_journal_print (LOG_WARNING, "%s", tmp_error->message);
}

static RpmOstreeScriptAction
lookup_script_action (HifPackage *package,
                      GHashTable *ignored_scripts,
                      const char *scriptdesc)
{
  const char *pkg_script = glnx_strjoina (hif_package_get_name (package), ".", scriptdesc+1);
  const struct RpmOstreePackageScriptHandler *handler = rpmostree_script_gperf_lookup (pkg_script, strlen (pkg_script));
  if (ignored_scripts && g_hash_table_contains (ignored_scripts, pkg_script))
    return RPMOSTREE_SCRIPT_ACTION_IGNORE;
  if (!handler)
    return RPMOSTREE_SCRIPT_ACTION_DEFAULT;
  return handler->action;
}

gboolean
rpmostree_script_txn_validate (HifPackage    *package,
                               Header         hdr,
                               GHashTable    *override_ignored_scripts,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (unsupported_scripts); i++)
    {
      const char *desc = unsupported_scripts[i].desc;
      rpmTagVal tagval = unsupported_scripts[i].tag;
      rpmTagVal progtagval = unsupported_scripts[i].progtag;
      RpmOstreeScriptAction action;

      if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
        continue;

      action = lookup_script_action (package, override_ignored_scripts, desc);
      switch (action)
        {
        case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Package '%s' has (currently) unsupported script of type '%s'",
                         hif_package_get_name (package), desc);
            goto out;
          }
        case RPMOSTREE_SCRIPT_ACTION_IGNORE:
          continue;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
run_script_in_bwrap_container (int rootfs_fd,
                               const char *name,
                               const char *scriptdesc,
                               const char *script,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  int i;
  const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};
  int estatus;
  char *rofiles_mnt = strdupa ("/tmp/rofiles-fuse.XXXXXX");
  const char *rofiles_argv[] = { "rofiles-fuse", "./usr", rofiles_mnt, NULL};
  const char *pkg_script = glnx_strjoina (name, ".", scriptdesc+1);
  const char *postscript_name = glnx_strjoina ("/", pkg_script);
  const char *postscript_path_container = glnx_strjoina ("/usr/", postscript_name);
  const char *postscript_path_host;
  gboolean mntpoint_created = FALSE;
  gboolean fuse_mounted = FALSE;
  g_autoptr(GPtrArray) bwrap_argv = g_ptr_array_new ();
  g_autoptr(GPtrArray) bwrap_argv_mallocd = g_ptr_array_new_with_free_func (g_free);
  GSpawnFlags bwrap_spawnflags = G_SPAWN_SEARCH_PATH;
  gboolean created_var_tmp = FALSE;

  if (!glnx_mkdtempat (AT_FDCWD, rofiles_mnt, 0700, error))
    goto out;

  mntpoint_created = TRUE;

  if (!g_spawn_sync (NULL, (char**)rofiles_argv, NULL, G_SPAWN_SEARCH_PATH,
                     child_setup_fchdir, GINT_TO_POINTER (rootfs_fd),
                     NULL, NULL, &estatus, error))
    goto out;
  if (!g_spawn_check_exit_status (estatus, error))
    {
      g_prefix_error (error, "Executing rofiles-fuse: ");
      goto out;
    }

  fuse_mounted = TRUE;

  postscript_path_host = glnx_strjoina (rofiles_mnt, "/", postscript_name);

  /* TODO - Create a pipe and send this to bwrap so it's inside the
   * tmpfs
   */
  if (!g_file_set_contents (postscript_path_host, script, -1, error))
    {
      g_prefix_error (error, "Writing script to %s: ", postscript_path_host);
      goto out;
    }
  if (chmod (postscript_path_host, 0755) != 0)
    {
      g_prefix_error (error, "chmod %s: ", postscript_path_host);
      goto out;
    }

  /* We need to make the mount point in the case where we're doing
   * package layering, since the host `/var` tree is empty.  We
   * *could* point at the real `/var`...but that seems
   * unnecessary/dangerous to me.  Daemons that need to perform data
   * migrations should do them as part of their systemd units and not
   * in %post.
   *
   * Another alternative would be to make a tmpfs with the compat
   * symlinks.
   */
  if (mkdirat (rootfs_fd, "var/tmp", 0755) < 0)
    {
      if (errno == EEXIST)
        ;
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    created_var_tmp = TRUE;

  add_const_args (bwrap_argv,
                  WITH_BUBBLEWRAP_PATH,
                  "--bind", rofiles_mnt, "/usr",
                  "--dev", "/dev",
                  "--proc", "/proc",
                  "--dir", "/tmp",
                  "--chdir", "/",
                  /* Scripts can see a /var with compat links like alternatives */
                  "--ro-bind", "./var", "/var",
                  /* But no need to access persistent /tmp, so make it /tmp */
                  "--bind", "/tmp", "/var/tmp",
                  /* Allow RPM scripts to change the /etc defaults */
                  "--symlink", "usr/etc", "/etc",
                  "--ro-bind", "/sys/block", "/sys/block",
                  "--ro-bind", "/sys/bus", "/sys/bus",
                  "--ro-bind", "/sys/class", "/sys/class",
                  "--ro-bind", "/sys/dev", "/sys/dev",
                  "--ro-bind", "/sys/devices", "/sys/devices",
                  NULL);

  for (i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      struct stat stbuf;
      char *path;

      if (!(fstatat (rootfs_fd, subdir, &stbuf, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK (stbuf.st_mode)))
        continue;

      g_ptr_array_add (bwrap_argv, "--symlink");

      path = g_strconcat ("usr/", subdir, NULL);
      g_ptr_array_add (bwrap_argv_mallocd, path);
      g_ptr_array_add (bwrap_argv, path);

      path = g_strconcat ("/", subdir, NULL);
      g_ptr_array_add (bwrap_argv_mallocd, path);
      g_ptr_array_add (bwrap_argv, path);
    }

  { const char *debugscript = getenv ("RPMOSTREE_DEBUG_SCRIPT");
    if (g_strcmp0 (debugscript, pkg_script) == 0)
      {
        g_ptr_array_add (bwrap_argv, (char*)"/bin/bash");
        bwrap_spawnflags |= G_SPAWN_CHILD_INHERITS_STDIN;
      }
    else
      g_ptr_array_add (bwrap_argv, (char*)postscript_path_container);
  }
  g_ptr_array_add (bwrap_argv, NULL);

  if (!g_spawn_sync (NULL, (char**)bwrap_argv->pdata, NULL, bwrap_spawnflags,
                     child_setup_fchdir, GINT_TO_POINTER (rootfs_fd),
                     NULL, NULL, &estatus, error))
    {
      g_prefix_error (error, "Executing bwrap: ");
      goto out;
    }
  if (!g_spawn_check_exit_status (estatus, error))
    {
      g_prefix_error (error, "Executing bwrap: ");
      goto out;
    }

  ret = TRUE;
 out:
  if (fuse_mounted)
    {
      (void) unlink (postscript_path_host);
      fusermount_cleanup (rofiles_mnt);
    }
  if (mntpoint_created)
    (void) unlinkat (AT_FDCWD, rofiles_mnt, AT_REMOVEDIR);
  if (created_var_tmp)
    (void) unlinkat (rootfs_fd, "var/tmp", AT_REMOVEDIR);
  return ret;
}

gboolean
rpmostree_posttrans_run_sync (HifPackage    *pkg,
                              Header         hdr,
                              GHashTable    *ignore_scripts,
                              int            rootfs_fd,
                              GCancellable  *cancellable,
                              GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (posttrans_scripts); i++)
    {
      const char *desc = posttrans_scripts[i].desc;
      rpmTagVal tagval = posttrans_scripts[i].tag;
      rpmTagVal progtagval = posttrans_scripts[i].progtag;
      const char *script;
      RpmOstreeScriptAction action;

      if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
        continue;
      
      script = headerGetString (hdr, tagval);
      if (!script)
        continue;

      action = lookup_script_action (pkg, ignore_scripts, desc);
      switch (action)
        {
        case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
          {
            rpmostree_output_task_begin ("Running %s for %s...", desc, hif_package_get_name (pkg));
            if (!run_script_in_bwrap_container (rootfs_fd, hif_package_get_name (pkg), desc, script,
                                                cancellable, error))
              {
                g_prefix_error (error, "Running %s for %s: ", desc, hif_package_get_name (pkg));
                return FALSE;
              }
            rpmostree_output_task_end ("done");
          }
        case RPMOSTREE_SCRIPT_ACTION_IGNORE:
          continue;
        }
    }

  return TRUE;
}

gboolean
rpmostree_script_ignore_hash_from_strv (const char *const *strv,
                                        GHashTable **out_hash,
                                        GError **error)
{
  g_autoptr(GHashTable) ignore_scripts = NULL;
  if (!strv)
    {
      *out_hash = NULL;
      return TRUE;
    }
  ignore_scripts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (const char *const* iter = strv; iter && *iter; iter++)
    g_hash_table_add (ignore_scripts, g_strdup (*iter));
  *out_hash = g_steal_pointer (&ignore_scripts);
  return TRUE;
}
