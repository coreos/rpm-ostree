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
#include <stdlib.h>

#include "rpmostree-postprocess.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-util.h"

#include "libgsystem.h"

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

      if (g_str_has_prefix (name, "vmlinuz-"))
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

  if (!ret_kernel)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find vmlinuz- in %s",
                   gs_file_get_path_cached (bootdir));
      goto out;
    }

  ret = TRUE;
  gs_transfer_out_value (out_kernel, &ret_kernel);
  gs_transfer_out_value (out_initramfs, &ret_initramfs);
 out:
  return ret;
}

static gboolean
do_kernel_prep (GFile         *yumroot,
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
  const char *kname;
  const char *kver;

  if (!find_kernel_and_initramfs_in_bootdir (bootdir, &kernel_path,
                                             &initramfs_path,
                                             cancellable, error))
    goto out;

  if (initramfs_path)
    {
      g_print ("Removing RPM-generated '%s'\n",
               gs_file_get_path_cached (initramfs_path));
      if (!gs_shutil_rm_rf (initramfs_path, cancellable, error))
        goto out;
    }

  kname = gs_file_get_basename_cached (kernel_path);
  kver = strchr (kname, '-');
  g_assert (kver);
  kver += 1;

  /* OSTree needs to own this */
  {
    gs_unref_object GFile *loaderdir = g_file_get_child (bootdir, "loader");
    if (!gs_shutil_rm_rf (loaderdir, cancellable, error))
      goto out;
  }

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (yumroot),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      "chroot", gs_file_get_path_cached (yumroot),
                                      "depmod", kver,
                                      NULL))
    goto out;

  /* Copy of code from gnome-continuous; yes, we hardcode
     the machine id for now, because distributing pre-generated
     initramfs images with dracut/systemd at the moment
     effectively requires this.
     http://lists.freedesktop.org/archives/systemd-devel/2013-July/011770.html
  */
  g_print ("Hardcoding machine-id\n");
  {
    const char *hardcoded_machine_id = "45bb3b96146aa94f299b9eb43646eb35\n";
    gs_unref_object GFile *machineid_path =
      g_file_resolve_relative_path (yumroot, "etc/machine-id");
    if (!g_file_replace_contents (machineid_path, hardcoded_machine_id,
                                  strlen (hardcoded_machine_id),
                                  NULL, FALSE, 0, NULL,
                                  cancellable, error))
      goto out;
  }

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (yumroot),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      "chroot", gs_file_get_path_cached (yumroot),
                                      "dracut", "-v", "--tmpdir=/tmp",
                                      "-f", "/tmp/initramfs.img", kver,
                                      NULL))
    goto out;

  initramfs_path = g_file_resolve_relative_path (yumroot, "tmp/initramfs.img");
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

static FILE *
gfopen (const char       *path,
        const char       *mode,
        GCancellable     *cancellable,
        GError          **error)
{
  FILE *ret = NULL; 

  ret = fopen (path, mode);
  if (!ret)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "fopen(%s): %s", path, g_strerror (errsv));
      return NULL;
    }
  return ret;
}

static gboolean
gfflush (FILE         *f,
         GCancellable *cancellable,
         GError      **error)
{
  if (fflush (f) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "fflush: %s", g_strerror (errsv));
      return FALSE;
    }
  return TRUE;
}

typedef enum {
  MIGRATE_PASSWD,
  MIGRATE_GROUP
} MigrateKind;

/*
 * This function is taking the /etc/passwd generated in the install
 * root, and splitting it into two streams: a new /etc/passwd that
 * just contains the root entry, and /usr/lib/passwd which contains
 * everything else.
 *
 * The implementation is kind of horrible because I wanted to avoid
 * duplicating the user/group code.
 */
static gboolean
migrate_passwd_file_except_root (GFile         *rootfs,
                                 MigrateKind    kind,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
  gboolean ret = FALSE;
  const char *name = kind == MIGRATE_PASSWD ? "passwd" : "group";
  gs_free char *src_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, NULL);
  gs_free char *etctmp_path = g_strconcat (gs_file_get_path_cached (rootfs), "/etc/", name, ".tmp", NULL);
  gs_free char *usrdest_path = g_strconcat (gs_file_get_path_cached (rootfs), "/usr/lib/", name, NULL);
  FILE *src_stream = NULL;
  FILE *etcdest_stream = NULL;
  FILE *usrdest_stream = NULL;

  src_stream = gfopen (src_path, "r", cancellable, error);
  if (!src_stream)
    goto out;

  etcdest_stream = gfopen (etctmp_path, "w", cancellable, error);
  if (!etcdest_stream)
    goto out;

  usrdest_stream = gfopen (usrdest_path, "a", cancellable, error);
  if (!usrdest_stream)
    goto out;

  errno = 0;
  while (TRUE)
    {
      struct passwd *pw = NULL;
      struct group *gr = NULL;
      FILE *deststream;
      int r;
      
      if (kind == MIGRATE_PASSWD)
        pw = fgetpwent (src_stream);
      else
        gr = fgetgrent (src_stream);

      if (!(pw || gr))
        {
          if (errno != 0 && errno != ENOENT)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "fgetpwent: %s", g_strerror (errno));
              goto out;
            }
          else
            break;
        }

      if ((pw && pw->pw_uid == 0) ||
          (gr && gr->gr_gid == 0))
        deststream = etcdest_stream;
      else
        deststream = usrdest_stream;

      if (pw)
        r = putpwent (pw, deststream);
      else
        r = putgrent (gr, deststream);

      if (r == -1)
        {
          int errsv = errno;
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "putpwent: %s", g_strerror (errsv));
          goto out;
        }
    }

  if (!gfflush (etcdest_stream, cancellable, error))
    goto out;
  if (!gfflush (usrdest_stream, cancellable, error))
    goto out;

  if (rename (etctmp_path, src_path) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "rename(%s, %s): %s", etctmp_path, src_path, g_strerror (errsv));
      goto out;
    }

  ret = TRUE;
 out:
  if (src_stream) (void) fclose (src_stream);
  if (etcdest_stream) (void) fclose (etcdest_stream);
  if (usrdest_stream) (void) fclose (usrdest_stream);
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
                                    RpmOstreePostprocessBootLocation boot_location,
                                    GFile         *yumroot,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *kernel_path = NULL;
  gs_unref_object GFile *initramfs_path = NULL;

  g_print ("Preparing kernel\n");
  if (!do_kernel_prep (yumroot, cancellable, error))
    goto out;
  
  g_print ("Initializing rootfs\n");
  if (!init_rootfs (targetroot, cancellable, error))
    goto out;

  g_print ("Migrating /etc/passwd to /usr/lib/\n");
  if (!migrate_passwd_file_except_root (yumroot, MIGRATE_PASSWD, cancellable, error))
    goto out;
  g_print ("Migrating /etc/group to /usr/lib/\n");
  if (!migrate_passwd_file_except_root (yumroot, MIGRATE_GROUP, cancellable, error))
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
  g_print ("Moving /boot\n");
  {
    gs_unref_object GFile *yumroot_boot =
      g_file_get_child (yumroot, "boot");
    gs_unref_object GFile *target_boot =
      g_file_get_child (targetroot, "boot");
    gs_unref_object GFile *target_usrlib =
      g_file_resolve_relative_path (targetroot, "usr/lib");
    gs_unref_object GFile *target_usrlib_ostree_boot =
      g_file_resolve_relative_path (target_usrlib, "ostree-boot");

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

gboolean
rpmostree_treefile_postprocessing (GFile         *yumroot,
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
                                     RpmOstreePostprocessBootLocation boot_location,
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

  if (!create_rootfs_from_yumroot_content (rootfs_tmp, boot_location, rootfs, cancellable, error))
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
  GFile *rootpath = (GFile*)user_data;
  /* Hardcoded at the moment, we're only taking file caps */
  static const char *accepted_xattrs[] = { "security.capability" };
  guint i;
  gs_unref_variant GVariant *existing_xattrs = NULL;
  gs_free_variant_iter GVariantIter *viter = NULL;
  gs_unref_object GFile *path = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  GVariant *key, *value;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  path = g_file_resolve_relative_path (rootpath, relpath[0] == '/' ? relpath+1 : relpath);
  if (!gs_file_get_all_xattrs (path, &existing_xattrs, NULL, error))
    goto out;

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
                  gs_file_get_path_cached (path), local_error->message);
      exit (1);
    }
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
metadata_version_unique (OstreeRepo  *repo,
                         const char  *checksum,
                         const char  *version,
                         GError     **error)
{
  gs_unref_variant GVariant *variant = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant GVariant *value = NULL;
  gs_free gchar *parent = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, error))
    {
      if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (error);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Do not have full history to validate version metadata is unique.");
        }
      goto out;
    }

  metadata = g_variant_get_child_value (variant, 0);
  if ((value = g_variant_lookup_value (metadata, "version", NULL)))
    if (g_str_equal (version, g_variant_get_string (value, NULL)))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Version already specified in commit %s", checksum);
        goto out;
      }
  
  if (!(parent = ostree_commit_get_parent (variant)))
    return TRUE;
  
  return metadata_version_unique (repo, parent, version, error);

 out:
  return FALSE;
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

  mtree = ostree_mutable_tree_new ();
  commit_modifier = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_xattr_callback (commit_modifier,
                                                  read_xattrs_cb, NULL,
                                                  rootfs);
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


  if (metadata && parent_revision)
    {
      gs_unref_variant GVariant *md_version = g_variant_lookup_value (metadata,
                                                                      "version",
                                                                      NULL);
      const char *version = g_variant_get_string (md_version, NULL);

      if (!metadata_version_unique (repo, parent_revision, version, error))
        goto out;
    }

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
