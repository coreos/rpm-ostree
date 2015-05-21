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

#include "string.h"

#include <ostree.h>
#include <errno.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <utime.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <libglnx.h>
#include <stdlib.h>

#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-cleanup.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-util.h"

typedef enum {
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_LEGACY,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW
} RpmOstreePostprocessBootLocation;

static gboolean
move_to_dir (GFile        *src,
             GFile        *dest_dir,
             GCancellable *cancellable,
             GError      **error)
{
  gs_unref_object GFile *dest =
    g_file_get_child (dest_dir, gs_file_get_basename_cached (src));

  return gs_file_rename (src, dest, cancellable, error);
}

static gboolean
run_sync_in_root (GFile        *yumroot,
                  const char   *binpath,
                  char        **child_argv,
                  GError     **error)
{
  gboolean ret = FALSE;
  const char *yumroot_path = gs_file_get_path_cached (yumroot);
  pid_t child = glnx_libcontainer_run_in_root (yumroot_path, binpath, child_argv);

  if (child == -1)
    {
      _rpmostree_set_error_from_errno (error, errno);
      goto out;
    }
  
  if (!_rpmostree_sync_wait_on_pid (child, error))
    goto out;
  
  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  const char *target;
  const char *src;
} Symlink;

static gboolean
init_rootfs (GFile         *targetroot,
             GCancellable  *cancellable,
             GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *child = NULL;
  guint i;
  const char *toplevel_dirs[] = { "dev", "proc", "run", "sys", "var", "sysroot" };
  const Symlink symlinks[] = {
    { "var/opt", "opt" },
    { "var/srv", "srv" },
    { "var/mnt", "mnt" },
    { "var/roothome", "root" },
    { "var/home", "home" },
    { "run/media", "media" },
    { "sysroot/ostree", "ostree" },
    { "sysroot/tmp", "tmp" },
  };

  if (!gs_file_ensure_directory (targetroot, TRUE,
                                 cancellable, error))
    goto out;
  
  for (i = 0; i < G_N_ELEMENTS (toplevel_dirs); i++)
    {
      gs_unref_object GFile *dir = g_file_get_child (targetroot, toplevel_dirs[i]);

      if (!gs_file_ensure_directory (dir, TRUE,
                                     cancellable, error))
        goto out;
    }

  for (i = 0; i < G_N_ELEMENTS (symlinks); i++)
    {
      const Symlink*linkinfo = symlinks + i;
      gs_unref_object GFile *src =
        g_file_resolve_relative_path (targetroot, linkinfo->src);

      if (!g_file_make_symbolic_link (src, linkinfo->target,
                                      cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_kernel_and_initramfs_in_bootdir (GFile       *bootdir,
                                      GFile      **out_kernel,
                                      GFile      **out_initramfs,
                                      GCancellable *cancellable,
                                      GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;
  gs_unref_object GFile *ret_kernel = NULL;
  gs_unref_object GFile *ret_initramfs = NULL;

  direnum = g_file_enumerate_children (bootdir, "standard::name", 0, 
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      const char *name;
      GFileInfo *file_info;
      GFile *child;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      name = g_file_info_get_name (file_info);

      /* Current Fedora 23 kernel.spec installs as just vmlinuz */
      if (strcmp (name, "vmlinuz") == 0 || g_str_has_prefix (name, "vmlinuz-"))
        {
          if (ret_kernel)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple vmlinuz- in %s",
                           gs_file_get_path_cached (bootdir));
              goto out;
            }
          ret_kernel = g_object_ref (child);
        }
      else if (g_str_has_prefix (name, "initramfs-"))
        {
          if (ret_initramfs)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple initramfs- in %s",
                           gs_file_get_path_cached (bootdir));
              goto out;
            }
          ret_initramfs = g_object_ref (child);
        }
    }

  ret = TRUE;
  gs_transfer_out_value (out_kernel, &ret_kernel);
  gs_transfer_out_value (out_initramfs, &ret_initramfs);
 out:
  return ret;
}

/* Given a directory @d, find the first child that is a directory,
 * returning it in @out_subdir.  If there are multiple directories,
 * return an error.
 */
static gboolean
find_ensure_one_subdirectory (GFile         *d,
                              GFile        **out_subdir,
                              GCancellable  *cancellable,
                              GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;
  gs_unref_object GFile *ret_subdir = NULL;

  direnum = g_file_enumerate_children (d, "standard::name,standard::type", 0, 
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (ret_subdir)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple subdirectories found in: %s", gs_file_get_path_cached (d));
              goto out;
            }
          ret_subdir = g_object_ref (child);
        }
    }

  ret = TRUE;
  gs_transfer_out_value (out_subdir, &ret_subdir);
 out:
  return ret;
}

static gboolean
do_kernel_prep (GFile         *yumroot,
                JsonObject    *treefile,
                GCancellable  *cancellable,
                GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *bootdir = 
    g_file_get_child (yumroot, "boot");
  gs_unref_object GFile *kernel_path = NULL;
  gs_unref_object GFile *initramfs_path = NULL;
  const char *boot_checksum_str = NULL;
  GChecksum *boot_checksum = NULL;
  g_autofree char *kver = NULL;

  if (!find_kernel_and_initramfs_in_bootdir (bootdir, &kernel_path,
                                             &initramfs_path,
                                             cancellable, error))
    goto out;

  if (kernel_path == NULL)
    {
      gs_unref_object GFile *mod_dir = g_file_resolve_relative_path (yumroot, "usr/lib/modules");
      gs_unref_object GFile *modversion_dir = NULL;

      if (!find_ensure_one_subdirectory (mod_dir, &modversion_dir, cancellable, error))
        goto out;

      if (modversion_dir)
        {
          kver = g_file_get_basename (modversion_dir);
          if (!find_kernel_and_initramfs_in_bootdir (modversion_dir, &kernel_path,
                                                     &initramfs_path,
                                                     cancellable, error))
            goto out;
        }
    }

  if (kernel_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find kernel (vmlinuz) in /boot or /usr/lib/modules");
      goto out;
    }
      
  if (initramfs_path)
    {
      g_print ("Removing RPM-generated '%s'\n",
               gs_file_get_path_cached (initramfs_path));
      if (!gs_shutil_rm_rf (initramfs_path, cancellable, error))
        goto out;
    }

  if (!kver)
    {
      const char *kname = gs_file_get_basename_cached (kernel_path);
      const char *kver_p;

      kver_p = strchr (kname, '-');
      g_assert (kver_p);
      kver = g_strdup (kver_p + 1);
    }

  /* OSTree needs to own this */
  {
    gs_unref_object GFile *loaderdir = g_file_get_child (bootdir, "loader");
    if (!gs_shutil_rm_rf (loaderdir, cancellable, error))
      goto out;
  }

  {
    char *child_argv[] = { "depmod", (char*)kver, NULL };
    if (!run_sync_in_root (yumroot, "/usr/sbin/depmod", child_argv, error))
      goto out;
  }

  /* Ensure the /etc/machine-id file is present and empty. Apparently systemd
     doesn't work when the file is missing (as of systemd-219-9.fc22) but it is
     correctly populated if the file is there.  */
  g_print ("Creating empty machine-id\n");
  {
    const char *hardcoded_machine_id = "";
    gs_unref_object GFile *machineid_path =
      g_file_resolve_relative_path (yumroot, "etc/machine-id");
    if (!g_file_replace_contents (machineid_path, hardcoded_machine_id,
                                  strlen (hardcoded_machine_id),
                                  NULL, FALSE, 0, NULL,
                                  cancellable, error))
      goto out;
  }

  {
    gs_unref_ptrarray GPtrArray *dracut_argv = g_ptr_array_new ();

    g_ptr_array_add (dracut_argv, "dracut");
    g_ptr_array_add (dracut_argv, "-v");
    g_ptr_array_add (dracut_argv, "--tmpdir=/tmp");
    g_ptr_array_add (dracut_argv, "-f");
    g_ptr_array_add (dracut_argv, "/var/tmp/initramfs.img");
    g_ptr_array_add (dracut_argv, (char*)kver);

    if (json_object_has_member (treefile, "initramfs-args"))
      {
        guint i, len;
        JsonArray *initramfs_args;

        initramfs_args = json_object_get_array_member (treefile, "initramfs-args");
        len = json_array_get_length (initramfs_args);

        for (i = 0; i < len; i++)
          {
            const char *arg = _rpmostree_jsonutil_array_require_string_element (initramfs_args, i, error);
            if (!arg)
              goto out;
            g_ptr_array_add (dracut_argv, (char*)arg);
          }
      }

    g_ptr_array_add (dracut_argv, NULL);

    if (!run_sync_in_root (yumroot, "/usr/sbin/dracut", (char**)dracut_argv->pdata, error))
      goto out;
  }

  initramfs_path = g_file_resolve_relative_path (yumroot, "var/tmp/initramfs.img");
  if (!g_file_query_exists (initramfs_path, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Dracut failed to generate '%s'",
                   gs_file_get_path_cached (initramfs_path));
      goto out;
    }

  {
    gs_free char *initramfs_name = g_strconcat ("initramfs-", kver, ".img", NULL);
    gs_unref_object GFile *initramfs_dest =
      g_file_get_child (bootdir, initramfs_name);

    if (!gs_file_rename (initramfs_path, initramfs_dest,
                         cancellable, error))
      goto out;

    /* Transfer ownership */
    g_object_unref (initramfs_path);
    initramfs_path = initramfs_dest;
    initramfs_dest = NULL;
  }

  boot_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (!_rpmostree_util_update_checksum_from_file (boot_checksum, kernel_path,
                                                  cancellable, error))
    goto out;
  if (!_rpmostree_util_update_checksum_from_file (boot_checksum, initramfs_path,
                                                  cancellable, error))
    goto out;

  boot_checksum_str = g_checksum_get_string (boot_checksum);
  
  {
    gs_free char *new_kernel_name =
      g_strconcat (gs_file_get_basename_cached (kernel_path), "-",
                   boot_checksum_str, NULL);
    gs_unref_object GFile *new_kernel_path =
      g_file_get_child (bootdir, new_kernel_name);
    gs_free char *new_initramfs_name =
      g_strconcat (gs_file_get_basename_cached (initramfs_path), "-",
                   boot_checksum_str, NULL);
    gs_unref_object GFile *new_initramfs_path =
      g_file_get_child (bootdir, new_initramfs_name);

    if (!gs_file_rename (kernel_path, new_kernel_path,
                         cancellable, error))
      goto out;
    if (!gs_file_rename (initramfs_path, new_initramfs_path,
                         cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  if (boot_checksum) g_checksum_free (boot_checksum);
  return ret;
}

static gboolean
convert_var_to_tmpfiles_d (GOutputStream *tmpfiles_out,
                           GFile         *yumroot,
                           GFile         *dir,
                           GCancellable  *cancellable,
                           GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;
  gsize bytes_written;

  direnum = g_file_enumerate_children (dir, "standard::name,standard::type,unix::mode,standard::symlink-target,unix::uid,unix::gid",
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    {
      g_prefix_error (error, "Enumerating /var in '%s'", gs_file_get_path_cached (dir));
      goto out;
    }

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GString *tmpfiles_d_buf;
      gs_free char *tmpfiles_d_line = NULL;
      char filetype_c;
      gs_free char *relpath = NULL;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      switch (g_file_info_get_file_type (file_info))
        {
        case G_FILE_TYPE_DIRECTORY:
          filetype_c = 'd';
          break;
        case G_FILE_TYPE_SYMBOLIC_LINK:
          filetype_c = 'L';
          break;
        default:
          g_print ("Ignoring non-directory/non-symlink '%s'\n",
                   gs_file_get_path_cached (child));
          continue;
        }

      tmpfiles_d_buf = g_string_new ("");
      g_string_append_c (tmpfiles_d_buf, filetype_c);
      g_string_append_c (tmpfiles_d_buf, ' ');

      relpath = g_file_get_relative_path (yumroot, child);
      g_assert (relpath);
      
      g_string_append_c (tmpfiles_d_buf, '/');
      g_string_append (tmpfiles_d_buf, relpath);

      if (filetype_c == 'd')
        {
          guint mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
          mode &= ~S_IFMT;
          g_string_append_printf (tmpfiles_d_buf, " 0%02o", mode);
          g_string_append_printf (tmpfiles_d_buf, " %d %d - -",
                                  g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                  g_file_info_get_attribute_uint32 (file_info, "unix::gid"));

          if (!convert_var_to_tmpfiles_d (tmpfiles_out, yumroot, child,
                                          cancellable, error))
            goto out;
        }
      else
        {
          g_string_append (tmpfiles_d_buf, " - - - - ");
          g_string_append (tmpfiles_d_buf, g_file_info_get_symlink_target (file_info));
        }

      g_string_append_c (tmpfiles_d_buf, '\n');

      tmpfiles_d_line = g_string_free (tmpfiles_d_buf, FALSE);

      if (!g_output_stream_write_all (tmpfiles_out, tmpfiles_d_line,
                                      strlen (tmpfiles_d_line), &bytes_written,
                                      cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

#if 0
static gboolean
clean_yumdb_extraneous_files (GFile         *dir,
                              GCancellable  *cancellable,
                              GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;

  direnum = g_file_enumerate_children (dir, "standard::name,standard::type",
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!clean_yumdb_extraneous_files (child, cancellable, error))
            goto out;
        }
      else if (strcmp (g_file_info_get_name (file_info), "var_uuid") == 0 ||
               strcmp (g_file_info_get_name (file_info), "from_repo_timestamp") == 0 ||
               strcmp (g_file_info_get_name (file_info), "from_repo_revision") == 0)
        {
          if (!g_file_delete (child, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
#endif

static gboolean
workaround_selinux_cross_labeling_recurse (GFile         *dir,
                                           GCancellable  *cancellable,
                                           GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;

  direnum = g_file_enumerate_children (dir, "standard::name,standard::type,time::modified,time::access",
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      const char *name;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      name = g_file_info_get_name (file_info);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!workaround_selinux_cross_labeling_recurse (child, cancellable, error))
            goto out;
        }
      else if (g_str_has_suffix (name, ".bin"))
        {
          const char *lastdot;
          gs_free char *nonbin_name = NULL;
          gs_unref_object GFile *nonbin_path = NULL;
          guint64 mtime = g_file_info_get_attribute_uint64 (file_info, "time::modified");
          guint64 atime = g_file_info_get_attribute_uint64 (file_info, "time::access");
          struct utimbuf times;

          lastdot = strrchr (name, '.');
          g_assert (lastdot);

          nonbin_name = g_strndup (name, lastdot - name);
          nonbin_path = g_file_get_child (dir, nonbin_name);

          times.actime = (time_t)atime;
          times.modtime = ((time_t)mtime) + 60;

          g_print ("Setting mtime of '%s' to newer than '%s'\n",
                   gs_file_get_path_cached (nonbin_path),
                   gs_file_get_path_cached (child));
          if (utime (gs_file_get_path_cached (nonbin_path), &times) == -1)
            {
              int errsv = errno;
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "utime(%s): %s", gs_file_get_path_cached (nonbin_path),
                           strerror (errsv));
              goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
workaround_selinux_cross_labeling (GFile         *rootfs,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *etc_selinux_dir = g_file_resolve_relative_path (rootfs, "usr/etc/selinux");

  if (g_file_query_exists (etc_selinux_dir, NULL))
    {
      if (!workaround_selinux_cross_labeling_recurse (etc_selinux_dir,
                                                      cancellable, error))
        goto out;
    }
    
  ret = TRUE;
 out:
  return ret;
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

static gboolean
migrate_rpm_and_yumdb (GFile          *targetroot,
                       GFile          *yumroot,
                       GCancellable   *cancellable,
                       GError        **error)

{
  gboolean ret = FALSE;
  gs_unref_object GFile *usrbin_rpm =
    g_file_resolve_relative_path (targetroot, "usr/bin/rpm");
  gs_unref_object GFile *legacyrpm_path =
    g_file_resolve_relative_path (yumroot, "var/lib/rpm");
  gs_unref_object GFile *newrpm_path =
    g_file_resolve_relative_path (targetroot, "usr/share/rpm");
  gs_unref_object GFile *src_yum_rpmdb_indexes =
    g_file_resolve_relative_path (yumroot, "var/lib/yum");
  gs_unref_object GFile *target_yum_rpmdb_indexes =
    g_file_resolve_relative_path (targetroot, "usr/share/yumdb");
  gs_unref_object GFile *yumroot_yumlib =
    g_file_get_child (yumroot, "var/lib/yum");
  gs_unref_object GFileEnumerator *direnum = NULL;

  direnum = g_file_enumerate_children (legacyrpm_path, "standard::name",
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      const char *name;
      GFileInfo *file_info;
      GFile *child;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      name = g_file_info_get_name (file_info);

      if (g_str_has_prefix (name, "__db.") ||
          strcmp (name, ".dbenv.lock") == 0 ||
          strcmp (name, ".rpm.lock") == 0)
        {
          if (!gs_file_unlink (child, cancellable, error))
            goto out;
        }
    }

  (void) g_file_enumerator_close (direnum, cancellable, error);
    
  g_print ("Placing RPM db in /usr/share/rpm\n");
  if (!gs_file_rename (legacyrpm_path, newrpm_path, cancellable, error))
    goto out;
    
  /* Move the yum database to usr/share/yumdb; disabled for now due
   * to bad conflict with OSTree's current
   * one-http-request-per-file.
   */
#if 0
  if (g_file_query_exists (src_yum_rpmdb_indexes, NULL))
    {
      g_print ("Moving %s to %s\n", gs_file_get_path_cached (src_yum_rpmdb_indexes),
               gs_file_get_path_cached (target_yum_rpmdb_indexes));
      if (!gs_file_rename (src_yum_rpmdb_indexes, target_yum_rpmdb_indexes,
                           cancellable, error))
        goto out;
        
      if (!clean_yumdb_extraneous_files (target_yum_rpmdb_indexes, cancellable, error))
        goto out;
    }
#endif

  /* Remove /var/lib/yum; we don't want it here. */
  if (!gs_shutil_rm_rf (yumroot_yumlib, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


/* Prepare a root filesystem, taking mainly the contents of /usr from yumroot */
static gboolean 
create_rootfs_from_yumroot_content (GFile         *targetroot,
                                    GFile         *yumroot,
                                    JsonObject    *treefile,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *kernel_path = NULL;
  gs_unref_object GFile *initramfs_path = NULL;
  gs_unref_hashtable GHashTable *preserve_groups_set = NULL;
  gboolean container = FALSE;

  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "container",
                                                               &container,
                                                               error))
    goto out;

  g_print ("Preparing kernel\n");
  if (!container && !do_kernel_prep (yumroot, treefile, cancellable, error))
    goto out;
  
  g_print ("Initializing rootfs\n");
  if (!init_rootfs (targetroot, cancellable, error))
    goto out;

  g_print ("Migrating /etc/passwd to /usr/lib/\n");
  if (!rpmostree_passwd_migrate_except_root (yumroot, RPM_OSTREE_PASSWD_MIGRATE_PASSWD, NULL,
                                             cancellable, error))
    goto out;

  if (json_object_has_member (treefile, "etc-group-members"))
    {
      JsonArray *etc_group_members = json_object_get_array_member (treefile, "etc-group-members");
      preserve_groups_set = _rpmostree_jsonutil_jsarray_strings_to_set (etc_group_members);
    }
      
  g_print ("Migrating /etc/group to /usr/lib/\n");
  if (!rpmostree_passwd_migrate_except_root (yumroot, RPM_OSTREE_PASSWD_MIGRATE_GROUP,
                                             preserve_groups_set,
                                             cancellable, error))
    goto out;

  /* NSS configuration to look at the new files */
  {
    gs_unref_object GFile *yumroot_etc = 
      g_file_resolve_relative_path (yumroot, "etc");

    if (!replace_nsswitch (yumroot_etc, cancellable, error))
      goto out;
  }

  /* We take /usr from the yum content */
  g_print ("Moving /usr to target\n");
  {
    gs_unref_object GFile *usr = g_file_get_child (yumroot, "usr");
    if (!move_to_dir (usr, targetroot, cancellable, error))
      goto out;
  }

  /* Except /usr/local -> ../var/usrlocal */
  g_print ("Linking /usr/local -> ../var/usrlocal\n");
  {
    gs_unref_object GFile *target_usrlocal =
      g_file_resolve_relative_path (targetroot, "usr/local");

    if (!gs_shutil_rm_rf (target_usrlocal, cancellable, error))
      goto out;

    if (!g_file_make_symbolic_link (target_usrlocal, "../var/usrlocal",
                                    cancellable, error))
      goto out;
  }

  /* And now we take the contents of /etc and put them in /usr/etc */
  g_print ("Moving /etc to /usr/etc\n");
  {
    gs_unref_object GFile *yumroot_etc =
      g_file_get_child (yumroot, "etc");
    gs_unref_object GFile *target_usretc =
      g_file_resolve_relative_path (targetroot, "usr/etc");
    
    if (!gs_file_rename (yumroot_etc, target_usretc,
                         cancellable, error))
      goto out;
  }

  if (!migrate_rpm_and_yumdb (targetroot, yumroot, cancellable, error))
    goto out;

  {
    gs_unref_object GFile *yumroot_var = g_file_get_child (yumroot, "var");
    gs_unref_object GFile *rpmostree_tmpfiles_path =
      g_file_resolve_relative_path (targetroot, "usr/lib/tmpfiles.d/rpm-ostree-autovar.conf");
    gs_unref_object GFileOutputStream *tmpfiles_out =
      g_file_create (rpmostree_tmpfiles_path,
                     G_FILE_CREATE_REPLACE_DESTINATION,
                     cancellable, error);

    if (!tmpfiles_out)
      goto out;

    
    if (!convert_var_to_tmpfiles_d ((GOutputStream*)tmpfiles_out, yumroot, yumroot_var,
                                    cancellable, error))
      goto out;

    if (!g_output_stream_close ((GOutputStream*)tmpfiles_out, cancellable, error))
      goto out;
  }

  /* Move boot, but rename the kernel/initramfs to have a checksum */
  if (!container)
    {
      gs_unref_object GFile *yumroot_boot =
        g_file_get_child (yumroot, "boot");
      gs_unref_object GFile *target_boot =
        g_file_get_child (targetroot, "boot");
      gs_unref_object GFile *target_usrlib =
        g_file_resolve_relative_path (targetroot, "usr/lib");
      gs_unref_object GFile *target_usrlib_ostree_boot =
        g_file_resolve_relative_path (target_usrlib, "ostree-boot");
      RpmOstreePostprocessBootLocation boot_location =
        RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH;
      const char *boot_location_str = NULL;

      g_print ("Moving /boot\n");

      if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile,
                                                                  "boot_location",
                                                                  &boot_location_str, error))
        goto out;

      if (boot_location_str != NULL)
        {
          if (strcmp (boot_location_str, "legacy") == 0)
            boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_LEGACY;
          else if (strcmp (boot_location_str, "both") == 0)
            boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH;
          else if (strcmp (boot_location_str, "new") == 0)
            boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW;
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid boot location '%s'", boot_location_str);
              goto out;
            }
        }

      if (!gs_file_ensure_directory (target_usrlib, TRUE, cancellable, error))
        goto out;

      switch (boot_location)
        {
        case RPMOSTREE_POSTPROCESS_BOOT_LOCATION_LEGACY:
          {
            g_print ("Using boot location: legacy\n");
            if (!gs_file_rename (yumroot_boot, target_boot, cancellable, error))
              goto out;
          }
          break;
        case RPMOSTREE_POSTPROCESS_BOOT_LOCATION_BOTH:
          {
            g_print ("Using boot location: both\n");
            if (!gs_file_rename (yumroot_boot, target_boot, cancellable, error))
              goto out;
            /* Hardlink the existing content, only a little ugly as
             * we'll end up sha256'ing it twice, but oh well. */
            if (!gs_shutil_cp_al_or_fallback (target_boot, target_usrlib_ostree_boot, cancellable, error))
              goto out;
          }
          break;
        case RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW:
          {
            g_print ("Using boot location: new\n");
            if (!gs_file_rename (yumroot_boot, target_usrlib_ostree_boot, cancellable, error))
              goto out;
          }
          break;
        }
    }

  /* Also carry along toplevel compat links */
  g_print ("Copying toplevel compat symlinks\n");
  {
    guint i;
    const char *toplevel_links[] = { "lib", "lib64", "lib32",
                                     "bin", "sbin" };
    for (i = 0; i < G_N_ELEMENTS (toplevel_links); i++)
      {
        gs_unref_object GFile *srcpath =
          g_file_get_child (yumroot, toplevel_links[i]);

        if (g_file_query_file_type (srcpath, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
          {
            if (!move_to_dir (srcpath, targetroot, cancellable, error))
              goto out;
          }
      }
  }

  g_print ("Adding tmpfiles-ostree-integration.conf\n");
  {
    gs_unref_object GFile *src_pkglibdir = g_file_new_for_path (PKGLIBDIR);
    gs_unref_object GFile *src_tmpfilesd =
      g_file_get_child (src_pkglibdir, "tmpfiles-ostree-integration.conf");
    gs_unref_object GFile *target_tmpfilesd =
      g_file_resolve_relative_path (targetroot, "usr/lib/tmpfiles.d/tmpfiles-ostree-integration.conf");
    gs_unref_object GFile *target_tmpfilesd_parent = g_file_get_parent (target_tmpfilesd);
    
    if (!gs_file_ensure_directory (target_tmpfilesd_parent, TRUE, cancellable, error))
      goto out;

    if (!g_file_copy (src_tmpfilesd, target_tmpfilesd, 0,
                      cancellable, NULL, NULL, error))
      goto out;
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
handle_remove_files_from_package (GFile         *yumroot,
                                  HySack         sack,
                                  JsonArray     *removespec,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  gboolean ret = FALSE;
  const char *pkg = json_array_get_string_element (removespec, 0);
  guint i, j, npackages;
  guint len = json_array_get_length (removespec);
  HyPackage hypkg;
  _cleanup_hyquery_ HyQuery query = NULL;
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      
  query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkg);
  pkglist = hy_query_run (query);
  npackages = hy_packagelist_count (pkglist);
  if (npackages == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find package '%s' specified in remove-files-from", pkg);
      goto out;
    }

  for (j = 0; j < npackages; j++)
    {
      _cleanup_hystringarray_ HyStringArray pkg_files = NULL;

      hypkg = hy_packagelist_get (pkglist, 0);
      pkg_files = hy_package_get_files (hypkg);

      for (i = 1; i < len; i++)
        {
          const char *remove_regex_pattern = json_array_get_string_element (removespec, i);
          GRegex *regex;
          char **strviter;

          regex = g_regex_new (remove_regex_pattern, G_REGEX_JAVASCRIPT_COMPAT, 0, error);

          if (!regex)
            goto out;
      
          for (strviter = pkg_files; strviter && strviter[0]; strviter++)
            {
              const char *file = *strviter;

              if (g_regex_match (regex, file, 0, NULL))
                {
                  gs_unref_object GFile *child = NULL;
              
                  if (file[0] == '/')
                    file++;
              
                  child = g_file_resolve_relative_path (yumroot, file);
                  g_print ("Deleting: %s\n", file);
                  if (!gs_shutil_rm_rf (child, cancellable, error))
                    goto out;
                }
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_treefile_postprocessing (GFile         *yumroot,
                                   GFile         *context_directory,
                                   GBytes        *serialized_treefile,
                                   JsonObject    *treefile,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  gboolean ret = FALSE;
  guint i, len;
  JsonArray *units = NULL;
  JsonArray *remove = NULL;
  const char *default_target = NULL;
  const char *postprocess_script = NULL;

  if (json_object_has_member (treefile, "units"))
    units = json_object_get_array_member (treefile, "units");

  if (units)
    len = json_array_get_length (units);
  else
    len = 0;

  {
    gs_unref_object GFile *multiuser_wants_dir =
      g_file_resolve_relative_path (yumroot, "etc/systemd/system/multi-user.target.wants");

    if (!gs_file_ensure_directory (multiuser_wants_dir, TRUE, cancellable, error))
      goto out;

    for (i = 0; i < len; i++)
      {
        const char *unitname = _rpmostree_jsonutil_array_require_string_element (units, i, error);
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
    const guint8 *buf;
    gsize len;

    if (!gs_file_ensure_directory (target_treefile_dir_path, TRUE,
                                   cancellable, error))
      goto out;
                                     
    g_print ("Writing '%s'\n", gs_file_get_path_cached (target_treefile_path));
    buf = g_bytes_get_data (serialized_treefile, &len);
    
    if (!g_file_replace_contents (target_treefile_path, (char*)buf, len,
                                  NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                    NULL, cancellable, error))
      goto out;
  }

  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "default_target",
                                                              &default_target, error))
    goto out;
  
  if (default_target != NULL)
    {
      gs_unref_object GFile *default_target_path =
        g_file_resolve_relative_path (yumroot, "etc/systemd/system/default.target");
      gs_free char *dest_default_target_path =
        g_strconcat ("/usr/lib/systemd/system/", default_target, NULL);

      (void) gs_file_unlink (default_target_path, NULL, NULL);
        
      if (!g_file_make_symbolic_link (default_target_path, dest_default_target_path,
                                      cancellable, error))
        goto out;
    }

  if (json_object_has_member (treefile, "remove-files"))
    {
      remove = json_object_get_array_member (treefile, "remove-files");
      len = json_array_get_length (remove);
    }
  else
    len = 0;
    
  for (i = 0; i < len; i++)
    {
      const char *val = _rpmostree_jsonutil_array_require_string_element (remove, i, error);
      gs_unref_object GFile *child = NULL;

      if (!val)
        return FALSE;
      if (g_path_is_absolute (val))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "'remove' elements must be relative");
          goto out;
        }

      child = g_file_resolve_relative_path (yumroot, val);
        
      if (g_file_query_exists (child, NULL))
        {
          g_print ("Removing '%s'\n", val);
          if (!gs_shutil_rm_rf (child, cancellable, error))
            goto out;
        }
      else
        {
          g_printerr ("warning: Targeted path for remove-files does not exist: %s\n",
                      gs_file_get_path_cached (child));
        }
    }

  if (json_object_has_member (treefile, "remove-from-packages"))
    {
      _cleanup_hysack_ HySack sack = NULL;
      _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
      guint i;

      remove = json_object_get_array_member (treefile, "remove-from-packages");
      len = json_array_get_length (remove);

      if (!rpmostree_get_pkglist_for_root (AT_FDCWD, gs_file_get_path_cached (yumroot),
                                           &sack, &pkglist,
                                           cancellable, error))
        {
          g_prefix_error (error, "Reading package set: ");
          goto out;
        }

      for (i = 0; i < len; i++)
        {
          JsonArray *elt = json_array_get_array_element (remove, i);
          if (!handle_remove_files_from_package (yumroot, sack, elt, cancellable, error))
            goto out;
        }
    }

  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile, "postprocess-script",
                                                              &postprocess_script, error))
    goto out;
    
  if (postprocess_script)
    {
      const char *yumroot_path = gs_file_get_path_cached (yumroot);
      gs_unref_object GFile *src = g_file_resolve_relative_path (context_directory, postprocess_script);
      const char *bn = gs_file_get_basename_cached (src);
      gs_free char *binpath = g_strconcat ("/usr/bin/rpmostree-postprocess-", bn, NULL);
      gs_free char *destpath = g_strconcat (yumroot_path, binpath, NULL);
      gs_unref_object GFile *dest = g_file_new_for_path (destpath);
      /* Clone all the things */

      if (!g_file_copy (src, dest, 0, cancellable, NULL, NULL, error))
        {
          g_prefix_error (error, "Copying postprocess-script '%s' into target: ", bn);
          goto out;
        }

      {
        char *child_argv[] = { binpath, NULL };
        if (!run_sync_in_root (yumroot, binpath, child_argv, error))
          goto out;
      }
                                          
      g_print ("Executing postprocessing script '%s'...done\n", bn);
    }
  
  ret = TRUE;
 out:
  return ret;
}

/**
 * rpmostree_prepare_rootfs_for_commit:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  * Move /etc to /usr/etc
 *  * Checksum the kernel in /boot
 *  * Migrate content in /var to systemd-tmpfiles
 */
gboolean
rpmostree_prepare_rootfs_for_commit (GFile         *rootfs,
                                     JsonObject    *treefile,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *rootfs_tmp = NULL;
  gs_free char *rootfs_tmp_path = NULL;

  rootfs_tmp_path = g_strconcat (gs_file_get_path_cached (rootfs), ".tmp", NULL);
  rootfs_tmp = g_file_new_for_path (rootfs_tmp_path);

  if (!gs_shutil_rm_rf (rootfs_tmp, cancellable, error))
    goto out;

  if (!create_rootfs_from_yumroot_content (rootfs_tmp, rootfs, treefile,
                                           cancellable, error))
    goto out;

  if (!gs_shutil_rm_rf (rootfs, cancellable, error))
    goto out;
  if (!gs_file_rename (rootfs_tmp, rootfs, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static GVariant *
read_xattrs_cb (OstreeRepo     *repo,
                const char     *relpath,
                GFileInfo      *file_info,
                gpointer        user_data)
{
  int rootfs_fd = GPOINTER_TO_INT (user_data);
  /* Hardcoded at the moment, we're only taking file caps */
  static const char *accepted_xattrs[] = { "security.capability" };
  guint i;
  gs_unref_variant GVariant *existing_xattrs = NULL;
  gs_free_variant_iter GVariantIter *viter = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  GVariant *key, *value;
  GVariantBuilder builder;

  if (relpath[0] == '/')
    relpath++;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  if (!*relpath)
    {
      if (!gs_fd_get_all_xattrs (rootfs_fd, &existing_xattrs, NULL, error))
        goto out;
    }
  else
    {
      if (!gs_dfd_and_name_get_all_xattrs (rootfs_fd, relpath, &existing_xattrs,
                                           NULL, error))
        goto out;
    }

  viter = g_variant_iter_new (existing_xattrs);

  while (g_variant_iter_loop (viter, "(@ay@ay)", &key, &value))
    {
      for (i = 0; i < G_N_ELEMENTS (accepted_xattrs); i++)
        {
          const char *validkey = accepted_xattrs[i];
          const char *attrkey = g_variant_get_bytestring (key);
          if (g_str_equal (validkey, attrkey))
            g_variant_builder_add (&builder, "(@ay@ay)", key, value);
        }
    }

 out:
  if (local_error)
    {
      g_variant_builder_clear (&builder);
      /* Unfortunately we have no way to throw from this callback */
      g_printerr ("Failed to read xattrs of '%s': %s\n",
                  relpath, local_error->message);
      exit (1);
    }
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

gboolean
rpmostree_commit (GFile         *rootfs,
                  OstreeRepo    *repo,
                  const char    *refname,
                  GVariant      *metadata,
                  const char    *gpg_keyid,
                  gboolean       enable_selinux,
                  GCancellable  *cancellable,
                  GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeMutableTree *mtree = NULL;
  OstreeRepoCommitModifier *commit_modifier = NULL;
  gs_free char *parent_revision = NULL;
  gs_free char *new_revision = NULL;
  gs_unref_object GFile *root_tree = NULL;
  gs_unref_object OstreeSePolicy *sepolicy = NULL;
  gs_fd_close int rootfs_fd = -1;
  
  /* hardcode targeted policy for now */
  if (enable_selinux)
    {
      if (!workaround_selinux_cross_labeling (rootfs, cancellable, error))
        goto out;
      
      sepolicy = ostree_sepolicy_new (rootfs, cancellable, error);
      if (!sepolicy)
        goto out;
    }

  g_print ("Committing '%s' ...\n", gs_file_get_path_cached (rootfs));
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (!gs_file_open_dir_fd (rootfs, &rootfs_fd, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();
  commit_modifier = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_xattr_callback (commit_modifier,
                                                  read_xattrs_cb, NULL,
                                                  GINT_TO_POINTER (rootfs_fd));
  if (sepolicy)
    {
      const char *policy_name = ostree_sepolicy_get_name (sepolicy);
      g_print ("Labeling with SELinux policy '%s'\n", policy_name);
      ostree_repo_commit_modifier_set_sepolicy (commit_modifier, sepolicy);
    }

  if (!ostree_repo_write_directory_to_mtree (repo, rootfs, mtree, commit_modifier, cancellable, error))
    goto out;
  if (!ostree_repo_write_mtree (repo, mtree, &root_tree, cancellable, error))
    goto out;

  if (!ostree_repo_resolve_rev (repo, refname, TRUE, &parent_revision, error))
    goto out;

  if (!ostree_repo_write_commit (repo, parent_revision, "", "", metadata,
                                 (OstreeRepoFile*)root_tree, &new_revision,
                                 cancellable, error))
    goto out;

  if (gpg_keyid)
    {
      g_print ("Signing commit %s with key %s\n", new_revision, gpg_keyid);
      if (!ostree_repo_sign_commit (repo, new_revision, gpg_keyid, NULL,
                                    cancellable, error))
        goto out;
    }
  
  ostree_repo_transaction_set_ref (repo, NULL, refname, new_revision);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  g_print ("%s => %s\n", refname, new_revision);

  if (!g_getenv ("RPM_OSTREE_PRESERVE_ROOTFS"))
    (void) gs_shutil_rm_rf (rootfs, NULL, NULL);
  else
    g_print ("Preserved %s\n", gs_file_get_path_cached (rootfs));

  ret = TRUE;
 out:
  return ret;
}
