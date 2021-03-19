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
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <libglnx.h>
#include <stdlib.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <utility>
#include <vector>

#include "rpmostree-postprocess.h"
#include "rpmostree-kernel.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-output.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-util.h"

typedef enum {
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES,
} RpmOstreePostprocessBootLocation;

static gboolean
rename_if_exists (int         src_dfd,
                  const char *from,
                  int         dest_dfd,
                  const char *to,
                  GError    **error)
{
  const char *errmsg = glnx_strjoina ("renaming ", from);
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);

  if (!glnx_fstatat_allow_noent (src_dfd, from, NULL, 0, error))
    return FALSE;
  if (errno == 0)
    {
      if (renameat (src_dfd, from, dest_dfd, to) < 0)
        {
          /* Handle empty directory in legacy location */
          if (G_IN_SET (errno, EEXIST, ENOTEMPTY))
            {
              if (!glnx_unlinkat (src_dfd, from, AT_REMOVEDIR, error))
                return FALSE;
            }
          else
            return glnx_throw_errno_prefix (error, "renameat(%s)", to);
        }
    }

  return TRUE;
}

typedef struct {
  const char *target;
  const char *src;
} Symlink;

/* Initialize deployment root directory. This is mostly hardcoded; in the future
 * we may make things more configurable.
 */
static gboolean
init_rootfs (int            dfd,
             gboolean       tmp_is_dir,
             GCancellable  *cancellable,
             GError       **error)
{
  const char *toplevel_dirs[] = { "dev", "proc", "run", "sys", "var", "sysroot" };
  const Symlink symlinks[] = {
    { "var/opt", "opt" },
    { "var/srv", "srv" },
    { "var/mnt", "mnt" },
    { "var/roothome", "root" },
    { "var/home", "home" },
    { "run/media", "media" },
    { "sysroot/ostree", "ostree" },
  };

  /* Ensure the rootfs has the right mode, we don't want to be affected
   * by umask.
   **/
  if (fchmod (dfd, 0755) == -1)
    return glnx_throw_errno_prefix (error, "fchmod(rootfs)");

  for (guint i = 0; i < G_N_ELEMENTS (toplevel_dirs); i++)
    {
      if (!glnx_ensure_dir (dfd, toplevel_dirs[i], 0755, error))
        return FALSE;
      if (fchmodat (dfd, toplevel_dirs[i], 0755, 0) == -1)
        return glnx_throw_errno_prefix (error, "fchmodat");
    }

  for (guint i = 0; i < G_N_ELEMENTS (symlinks); i++)
    {
      const Symlink*linkinfo = symlinks + i;
      if (symlinkat (linkinfo->target, dfd, linkinfo->src) < 0)
        return glnx_throw_errno_prefix (error, "symlinkat");
    }

  if (tmp_is_dir)
    {
      if (!glnx_shutil_mkdir_p_at (dfd, "tmp", 01777,
                                   cancellable, error))
        return FALSE;
      if (fchmodat (dfd, "tmp", 01777, 0) == -1)
        return glnx_throw_errno_prefix (error, "fchmodat");
    }
  else
    {
      if (symlinkat ("sysroot/tmp", dfd, "tmp") < 0)
        return glnx_throw_errno_prefix (error, "symlinkat");
    }

  return TRUE;
}

/* Given a directory referenced by @src_dfd+@src_path, as well as a target
 * directory @dest_dfd+@dest_path (which must already exist), hardlink all
 * content recursively.
 */
static gboolean
hardlink_recurse (int                src_dfd,
                  const char        *src_path,
                  int                dest_dfd,
                  const char        *dest_path,
                  GCancellable      *cancellable,
                  GError            **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_autofd int dest_target_dfd = -1;

  if (!glnx_dirfd_iterator_init_at (src_dfd, src_path, TRUE, &dfd_iter, error))
    return FALSE;

  if (!glnx_opendirat (dest_dfd, dest_path, TRUE, &dest_target_dfd, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      if (dent->d_type == DT_DIR)
        {
          mode_t perms = stbuf.st_mode & ~S_IFMT;

          if (!glnx_ensure_dir (dest_target_dfd, dent->d_name, perms, error))
            return FALSE;
          if (fchmodat (dest_target_dfd, dent->d_name, perms, 0) < 0)
            return glnx_throw_errno_prefix (error, "fchmodat");
          if (!hardlink_recurse (dfd_iter.fd, dent->d_name,
                                 dest_target_dfd, dent->d_name,
                                 cancellable, error))
            return FALSE;
        }
      else
        {
          if (linkat (dfd_iter.fd, dent->d_name,
                      dest_target_dfd, dent->d_name, 0) < 0)
            return glnx_throw_errno_prefix (error, "linkat");
        }
    }

  return TRUE;
}

gboolean
rpmostree_postprocess_run_depmod (int           rootfs_dfd,
                                  const char   *kver,
                                  gboolean      unified_core_mode,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  rust::Vec child_argv = { rust::String("depmod"), rust::String("-a"), rust::String(kver) };
  rpmostreecxx::bwrap_run_mutable (rootfs_dfd, "depmod", child_argv, (bool)unified_core_mode);
  return TRUE;
}

/* Handle the kernel/initramfs, which can be in at least 2 different places:
 *  - /boot (CentOS, Fedora treecompose before we suppressed kernel.spec's %posttrans)
 *  - /usr/lib/modules (Fedora treecompose without kernel.spec's %posttrans)
 *
 * We then need to handle the boot-location option, which can put that data in
 * either both (/boot and /usr/lib/ostree-boot), or just the latter.
 */
static gboolean
process_kernel_and_initramfs (int            rootfs_dfd,
                              JsonObject    *treefile,
                              gboolean       unified_core_mode,
                              GCancellable  *cancellable,
                              GError       **error)
{
  /* The current systemd kernel-install will inject
   * /boot/${machine_id}/${uname -r} which we don't use;
   * to avoid confusion, we will delete it.  This relies
   * on systemd itself having set up the machine id from its %post,
   * so we need to read it.  We'll reset the machine ID after this.
   */
  { glnx_autofd int fd = openat (rootfs_dfd, "usr/etc/machine-id", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      {
        if (errno != ENOENT)
          return glnx_throw_errno_prefix (error, "openat(usr/etc/machine-id)");
      }
    else
      {
        g_autofree char *old_machine_id = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
        if (!old_machine_id)
          return FALSE;
        size_t len = strlen (old_machine_id);
        /* Length might already be zero when starting from e.g. Fedora container base image */
        if (len > 0)
          {
            if (len != 33)
              return glnx_throw (error, "invalid machine ID '%.33s'", old_machine_id);
            /* Trim newline */
            old_machine_id[32] = '\0';

            const char *boot_machineid_dir = glnx_strjoina ("boot/", old_machine_id);
            if (!glnx_shutil_rm_rf_at (rootfs_dfd, boot_machineid_dir, cancellable, error))
              return FALSE;
          }
      }
  }

  /* We need to move non-kernel data (bootloader bits usually) into
   * /usr/lib/ostree-boot; this will also take care of moving the kernel in legacy
   * paths (CentOS, Fedora <= 24), etc.
   */
  if (!rename_if_exists (rootfs_dfd, "boot", rootfs_dfd, "usr/lib/ostree-boot", error))
    return FALSE;

  /* Find the kernel in the source root (at this point one of usr/lib/modules or
   * usr/lib/ostree-boot)
   */
  g_autoptr(GVariant) kernelstate = rpmostree_find_kernel (rootfs_dfd, cancellable, error);
  if (!kernelstate)
    return FALSE;
  const char* kernel_path;
  const char* initramfs_path;
  const char *kver;
  const char *bootdir;
  /* Used to optionally hardlink result of our dracut run */
  g_variant_get (kernelstate, "(&s&s&sm&s)",
                 &kver, &bootdir,
                 &kernel_path, &initramfs_path);

  /* We generate our own initramfs with custom arguments, so if the RPM install
   * generated one (should only happen on CentOS now), delete it.
   */
  if (initramfs_path)
    {
      g_assert_cmpstr (bootdir, ==, "usr/lib/ostree-boot");
      g_assert_cmpint (*initramfs_path, !=, '/');
      g_print ("Removing RPM-generated '%s'\n", initramfs_path);
      if (!glnx_shutil_rm_rf_at (rootfs_dfd, initramfs_path, cancellable, error))
        return FALSE;
      initramfs_path = NULL;
    }

  /* Ensure depmod (kernel modules index) is up to date; because on Fedora we
   * suppress the kernel %posttrans we need to take care of this.
   */
  if (!rpmostree_postprocess_run_depmod (rootfs_dfd, kver, unified_core_mode,
                                         cancellable, error))
    return FALSE;

  RpmOstreePostprocessBootLocation boot_location =
    RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW;
  const char *boot_location_str = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile,
                                                              "boot-location",
                                                              &boot_location_str, error))
    return FALSE;
  if (boot_location_str != NULL)
    {
      if (g_str_equal (boot_location_str, "new"))
        boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW;
      else if (g_str_equal (boot_location_str, "modules"))
        boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES;
      else
        return glnx_throw (error, "Invalid boot location '%s'", boot_location_str);
    }

  gboolean machineid_compat = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile, "machineid-compat",
                                                               &machineid_compat, error))
    return FALSE;
  if (machineid_compat)
    {
      /* Update June 2018: systemd seems to work fine with this deleted now in
       * both Fedora 28 and RHEL 7.5. However, see the treefile.md docs for why
       * "compat" mode is enabled by default.
       *
       * ORIGINAL COMMENT:
       * Ensure the /etc/machine-id file is present and empty; it is read by
       * dracut.  Systemd doesn't work when the file is missing (as of
       * systemd-219-9.fc22) but it is correctly populated if the file is there.
       */
      g_print ("Creating empty machine-id\n");
      if (!glnx_file_replace_contents_at (rootfs_dfd, "usr/etc/machine-id", (guint8*)"", 0,
                                          GLNX_FILE_REPLACE_NODATASYNC,
                                          cancellable, error))
        return FALSE;
    }
  else
    {
      (void) unlinkat (rootfs_dfd, "usr/etc/machine-id", 0);
    }

  /* Run dracut with our chosen arguments (commonly at least --no-hostonly) */
  g_autoptr(GPtrArray) dracut_argv = g_ptr_array_new ();
  if (treefile && json_object_has_member (treefile, "initramfs-args"))
    {
      JsonArray *initramfs_args = json_object_get_array_member (treefile, "initramfs-args");
      guint len = json_array_get_length (initramfs_args);

      for (guint i = 0; i < len; i++)
        {
          const char *arg = _rpmostree_jsonutil_array_require_string_element (initramfs_args, i, error);
          if (!arg)
            return FALSE;
          g_ptr_array_add (dracut_argv, (char*)arg);
        }
    }
  else
    {
      /* Default to this for treecomposes */
      g_ptr_array_add (dracut_argv, (char*)"--no-hostonly");
    }
  g_ptr_array_add (dracut_argv, NULL);

  g_auto(GLnxTmpfile) initramfs_tmpf = { 0, };
  /* We use a tmpdir under the target root since dracut currently tries to copy
   * xattrs, including e.g. user.ostreemeta, which can't be copied to tmpfs.
   */
  { g_auto(GLnxTmpDir) dracut_host_tmpd = { 0, };
    if (!glnx_mkdtempat (rootfs_dfd, "rpmostree-dracut.XXXXXX", 0700,
                         &dracut_host_tmpd, error))
      return FALSE;
    if (!rpmostree_run_dracut (rootfs_dfd,
                               (const char *const*)dracut_argv->pdata, kver,
                               NULL, FALSE, &dracut_host_tmpd,
                               &initramfs_tmpf, cancellable, error))
      return FALSE;
    /* No reason to have the initramfs not be world-readable since
     * it's server-side generated and shouldn't contain any secrets.
     * https://github.com/coreos/coreos-assembler/pull/372#issuecomment-467620937
     */
    if (!glnx_fchmod (initramfs_tmpf.fd, 0644, error))
      return FALSE;
  }

  /* We always tell rpmostree_finalize_kernel() to skip /boot, since we'll do a
   * full hardlink pass if needed after that for the kernel + bootloader data.
   */
  RpmOstreeFinalizeKernelDestination fin_dest =
    (boot_location == RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES) ?
    RPMOSTREE_FINALIZE_KERNEL_USRLIB_MODULES :
    RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT;
  if (!rpmostree_finalize_kernel (rootfs_dfd, bootdir, kver, kernel_path,
                                  &initramfs_tmpf, fin_dest,
                                  cancellable, error))
    return FALSE;

  /* We always ensure this exists as a mountpoint */
  if (!glnx_ensure_dir (rootfs_dfd, "boot", 0755, error))
    return FALSE;

  return TRUE;
}

static gboolean
convert_var_to_tmpfiles_d_recurse (GOutputStream          *tmpfiles_out,
                                   int                    dfd,
                                   rpmostreecxx::PasswdDB &pwdb,
                                   GString                *prefix,
                                   GCancellable           *cancellable,
                                   GError                 **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gsize bytes_written;

  if (!glnx_dirfd_iterator_init_at (dfd, prefix->str + 1, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      char filetype_c;
      switch (dent->d_type)
        {
        case DT_DIR:
          filetype_c = 'd';
          break;
        case DT_LNK:
          filetype_c = 'L';
          break;
        case DT_REG:
          /* nfs-utils in RHEL7; https://bugzilla.redhat.com/show_bug.cgi?id=1427537 */
          if (g_str_has_prefix (prefix->str, "/var/lib/nfs"))
            {
              filetype_c = 'f';
              break;
            }
          /* Fallthrough */
        default:
          if (!glnx_unlinkat (dfd_iter.fd, dent->d_name, 0, error))
            return FALSE;
          g_print ("Ignoring non-directory/non-symlink '%s/%s'\n",
                   prefix->str,
                   dent->d_name);
          continue;
        }

      g_autoptr(GString) tmpfiles_d_buf = g_string_new ("");
      g_string_append_c (tmpfiles_d_buf, filetype_c);
      g_string_append_c (tmpfiles_d_buf, ' ');
      g_string_append (tmpfiles_d_buf, prefix->str);
      g_string_append_c (tmpfiles_d_buf, '/');
      g_string_append (tmpfiles_d_buf, dent->d_name);

      if (filetype_c == 'd' || filetype_c == 'f')
        {
          struct stat stbuf;
          if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;
          g_string_append_printf (tmpfiles_d_buf, " 0%02o", stbuf.st_mode & ~S_IFMT);

          auto username = pwdb.lookup_user (stbuf.st_uid);
          auto groupname = pwdb.lookup_group (stbuf.st_gid);
          g_string_append_printf (tmpfiles_d_buf, " %s %s - -", username.c_str(), groupname.c_str());

          if (filetype_c == 'd')
            {
              /* Push prefix */
              gsize prev_len = prefix->len;
              g_string_append_c (prefix, '/');
              g_string_append (prefix, dent->d_name);

              if (!convert_var_to_tmpfiles_d_recurse (tmpfiles_out, dfd, pwdb, prefix,
                                                      cancellable, error))
                return FALSE;

              /* Pop prefix */
              g_string_truncate (prefix, prev_len);
            }
        }
      else
        {
          g_autofree char *link = glnx_readlinkat_malloc (dfd_iter.fd, dent->d_name, cancellable, error);
          if (!link)
            return FALSE;
          g_string_append (tmpfiles_d_buf, " - - - - ");
          g_string_append (tmpfiles_d_buf, link);

        }

      if (!glnx_unlinkat (dfd_iter.fd, dent->d_name,
                          dent->d_type == DT_DIR ? AT_REMOVEDIR : 0, error))
        return FALSE;

      g_string_append_c (tmpfiles_d_buf, '\n');

      if (!g_output_stream_write_all (tmpfiles_out, tmpfiles_d_buf->str,
                                      tmpfiles_d_buf->len, &bytes_written,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
convert_var_to_tmpfiles_d (int            rootfs_dfd,
                           GCancellable  *cancellable,
                           GError       **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Converting /var to tmpfiles.d", error);

  auto pwdb = rpmostreecxx::passwddb_open (rootfs_dfd);

  glnx_autofd int var_dfd = -1;
  /* List of files that are known to possibly exist, but in practice
   * things work fine if we simply ignore them.  Don't add something
   * to this list unless you've verified it's handled correctly at
   * runtime.  (And really both in CentOS and Fedora)
   */
  const char *known_state_files[] = {
    "lib/systemd/random-seed", /* https://bugzilla.redhat.com/show_bug.cgi?id=789407 */
    "lib/systemd/catalog/database",
    "lib/plymouth/boot-duration",
    "log/wtmp", /* These two are part of systemd's var.tmp */
    "log/btmp",
  };

  if (!glnx_opendirat (rootfs_dfd, "var", TRUE, &var_dfd, error))
    return FALSE;

  /* We never want to traverse into /run when making tmpfiles since it's a tmpfs */
  /* Note that in a Fedora root, /var/run is a symlink, though on el7, it can be a dir.
   * See: https://github.com/projectatomic/rpm-ostree/pull/831 */
  if (!glnx_shutil_rm_rf_at (var_dfd, "run", cancellable, error))
    return FALSE;

  /* Here, delete some files ahead of time to avoid emitting warnings
   * for things that are known to be harmless.
   */
  for (guint i = 0; i < G_N_ELEMENTS (known_state_files); i++)
    {
      const char *path = known_state_files[i];
      if (unlinkat (var_dfd, path, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "unlinkat(%s)", path);
        }
    }

  /* Convert /var wholesale to tmpfiles.d. Note that with unified core, this
   * code should no longer be necessary as we convert packages on import.
   */
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (rootfs_dfd, "usr/lib/tmpfiles.d", O_WRONLY | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) tmpfiles_out = g_unix_output_stream_new (tmpf.fd, FALSE);
  if (!tmpfiles_out)
    return FALSE;

  g_autoptr(GString) prefix = g_string_new ("/var");
  if (!convert_var_to_tmpfiles_d_recurse (tmpfiles_out, rootfs_dfd, *pwdb, prefix, cancellable, error))
    return FALSE;

  if (!g_output_stream_close (tmpfiles_out, cancellable, error))
    return FALSE;

  /* Make it world-readable, no reason why not to
   * https://bugzilla.redhat.com/show_bug.cgi?id=1631794
   */
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE,
                             rootfs_dfd, "usr/lib/tmpfiles.d/rpm-ostree-1-autovar.conf",
                             error))
    return FALSE;

  return TRUE;
}

/* SELinux uses PCRE pre-compiled regexps for binary caches, which can
 * fail if the version of PCRE on the host differs from the version
 * which generated the cache (in the target root).
 *
 * Note also this function is probably already broken in Fedora
 * 23+ from https://bugzilla.redhat.com/show_bug.cgi?id=1265406
 */
static gboolean
workaround_selinux_cross_labeling_recurse (int            dfd,
                                           const char    *path,
                                           GCancellable  *cancellable,
                                           GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (dfd, path, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      const char *name;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;

      if (!dent)
        break;

      name = dent->d_name;

      if (dent->d_type == DT_DIR)
        {
          if (!workaround_selinux_cross_labeling_recurse (dfd_iter.fd, name, cancellable, error))
            return FALSE;
        }
      else if (g_str_has_suffix (name, ".bin"))
        {
          struct stat stbuf;
          const char *lastdot;
          g_autofree char *nonbin_name = NULL;

          if (!glnx_fstatat (dfd_iter.fd, name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          lastdot = strrchr (name, '.');
          g_assert (lastdot);

          nonbin_name = g_strndup (name, lastdot - name);

          if (TEMP_FAILURE_RETRY (utimensat (dfd_iter.fd, nonbin_name, NULL, 0)) == -1)
            return glnx_throw_errno_prefix (error, "utimensat");
        }
    }

  return TRUE;
}

gboolean
rpmostree_prepare_rootfs_get_sepolicy (int            dfd,
                                       OstreeSePolicy **out_sepolicy,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  const char *policy_path;

  /* Handle the policy being in both /usr/etc and /etc since
   * this function can be called at different points.
   */
  if (!glnx_fstatat_allow_noent (dfd, "usr/etc", NULL, 0, error))
    return FALSE;
  if (errno == ENOENT)
    policy_path = "etc/selinux";
  else
    policy_path = "usr/etc/selinux";

  if (!glnx_fstatat_allow_noent (dfd, policy_path, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      if (!workaround_selinux_cross_labeling_recurse (dfd, policy_path,
                                                      cancellable, error))
        return FALSE;
    }

  g_autoptr(OstreeSePolicy) ret_sepolicy = ostree_sepolicy_new_at (dfd, cancellable, error);
  if (ret_sepolicy == NULL)
    return FALSE;

  *out_sepolicy = util::move_nullify (ret_sepolicy);
  return TRUE;
}

/* Change the policy store location.
 * Part of SELinux in Fedora >= 24: https://bugzilla.redhat.com/show_bug.cgi?id=1290659
 */
gboolean
rpmostree_rootfs_fixup_selinux_store_root (int rootfs_dfd,
                                           GCancellable *cancellable,
                                           GError **error)
{
  const char *semanage_path = "usr/etc/selinux/semanage.conf";

  /* Check if the config file exists; if not, do nothing silently */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, semanage_path, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT)
    return TRUE;

  g_autofree char *orig_contents =
    glnx_file_get_contents_utf8_at (rootfs_dfd, semanage_path, NULL,
                                    cancellable, error);
  if (orig_contents == NULL)
    return glnx_prefix_error (error, "Opening %s", semanage_path);

  static const char new_store_root[] = "\nstore-root=/etc/selinux\n";
  if (strstr (orig_contents, new_store_root) == NULL)
    {
      g_autofree char *new_contents = g_strconcat (orig_contents, new_store_root, NULL);
      if (!glnx_file_replace_contents_at (rootfs_dfd, semanage_path,
                                          (guint8*)new_contents, -1, static_cast<GLnxFileReplaceFlags>(0),
                                          cancellable, error))
        return glnx_prefix_error (error, "Replacing %s", semanage_path);
    }

  return TRUE;
}

/* SELinux in Fedora >= 24: https://bugzilla.redhat.com/show_bug.cgi?id=1290659 */
static gboolean
postprocess_selinux_policy_store_location (int rootfs_dfd,
                                           GCancellable *cancellable,
                                           GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  const char *var_policy_location = NULL;
  const char *etc_policy_location = NULL;
  const char *name;
  glnx_autofd int etc_selinux_dfd = -1;

  if (!rpmostree_prepare_rootfs_get_sepolicy (rootfs_dfd, &sepolicy, cancellable, error))
    return FALSE;

  name = ostree_sepolicy_get_name (sepolicy);
  if (!name) /* If there's no policy, shortcut here */
    return TRUE;

  var_policy_location = glnx_strjoina ("var/lib/selinux/", name);
  const char *modules_location = glnx_strjoina (var_policy_location, "/active/modules");
  if (!glnx_fstatat_allow_noent (rootfs_dfd, modules_location, NULL, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      /* Okay, this is probably CentOS 7, or maybe we have a build of
       * selinux-policy with the path moved back into /etc (or maybe it's
       * in /usr).
       */
      return TRUE;
    }
  g_print ("SELinux policy in /var, enabling workaround\n");

  if (!rpmostree_rootfs_fixup_selinux_store_root (rootfs_dfd, cancellable, error))
    return FALSE;

  etc_policy_location = glnx_strjoina ("usr/etc/selinux/", name);
  if (!glnx_opendirat (rootfs_dfd, etc_policy_location, TRUE, &etc_selinux_dfd, error))
    return FALSE;

  if (!glnx_dirfd_iterator_init_at (rootfs_dfd, var_policy_location, TRUE, &dfd_iter, error))
    return FALSE;

  /* We take all of the contents of the directory, but not the directory itself */
  while (TRUE)
    {
      struct dirent *dent = NULL;
      const char *name;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      name = dent->d_name;
      if (!glnx_renameat (dfd_iter.fd, name, etc_selinux_dfd, name, error))
        return FALSE;
    }

  return TRUE;
}

/* All "final" processing; things that are really required to use
 * rpm-ostree on the target host.
 */
gboolean
rpmostree_postprocess_final (int            rootfs_dfd,
                             JsonObject    *treefile,
                             gboolean       unified_core_mode,
                             GCancellable  *cancellable,
                             GError       **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finalizing rootfs", error);

  /* Use installation of the tmpfiles integration as an "idempotence" marker to
   * avoid doing postprocessing twice, which can happen when mixing `compose
   * postprocess-root` with `compose commit`.
   */
  const char tmpfiles_integration_path[] = "usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf";
  if (!glnx_fstatat_allow_noent (rootfs_dfd, tmpfiles_integration_path, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    return TRUE;

  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "selinux",
                                                               &selinux,
                                                               error))
    return FALSE;

  rpmostreecxx::compose_postprocess_final (rootfs_dfd);

  if (selinux)
    {
      g_print ("Recompiling policy\n");

      /* Now regenerate SELinux policy so that postprocess scripts from users and from us
       * (e.g. the /etc/default/useradd incision) that affect it are baked in. */
      rust::Vec child_argv = { rust::String("semodule"), rust::String("-nB") };
      rpmostreecxx::bwrap_run_mutable (rootfs_dfd, "semodule", child_argv, (bool)unified_core_mode);
    }

  gboolean container = FALSE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "container",
                                                               &container,
                                                               error))
    return FALSE;

  try {
    g_print ("Migrating /usr/etc/passwd to /usr/lib/\n");
    rpmostreecxx::migrate_passwd_except_root(rootfs_dfd);
  } catch (std::exception& e) {
    util::rethrow_prefixed(e, "failed to migrate 'passwd' to /usr/lib");
  }

  rust::Vec<rust::String> preserve_groups_set;
  if (treefile && json_object_has_member (treefile, "etc-group-members"))
    {
      JsonArray *etc_group_members = json_object_get_array_member (treefile, "etc-group-members");
      const guint len = json_array_get_length (etc_group_members);
      for (guint i = 0; i < len; i++)
        {
          const char *elt = json_array_get_string_element (etc_group_members, i);
          g_assert (elt != NULL);
          auto entry = std::string(elt);
          // TODO(lucab): drop this once we can pass the treefile directly to Rust.
          preserve_groups_set.push_back(entry);
        }
    }

  try {
    g_print ("Migrating /usr/etc/group to /usr/lib/\n");
    rpmostreecxx::migrate_group_except_root(rootfs_dfd, preserve_groups_set);
  } catch (std::exception& e) {
    util::rethrow_prefixed(e, "failed to migrate 'group' to /usr/lib");
  }

  /* NSS configuration to look at the new files */
  rpmostreecxx::composepost_nsswitch_altfiles(rootfs_dfd);

  if (selinux)
    {
      if (!postprocess_selinux_policy_store_location (rootfs_dfd, cancellable, error))
        return glnx_prefix_error (error, "SELinux postprocess");
    }

  if (!convert_var_to_tmpfiles_d (rootfs_dfd, cancellable, error))
    return FALSE;

  if (!rpmostree_rootfs_prepare_links (rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (rootfs_dfd, cancellable, error))
    return FALSE;

  g_print ("Adding rpm-ostree-0-integration.conf\n");
  /* This is useful if we're running in an uninstalled configuration, e.g.
   * during tests. */
  const char *pkglibdir_path
    = g_getenv("RPMOSTREE_UNINSTALLED_PKGLIBDIR") ?: PKGLIBDIR;
  glnx_autofd int pkglibdir_dfd = -1;

  if (!glnx_opendirat (AT_FDCWD, pkglibdir_path, TRUE, &pkglibdir_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (rootfs_dfd, "usr/lib/tmpfiles.d", 0755, cancellable, error))
    return FALSE;

  if (!glnx_file_copy_at (pkglibdir_dfd, "rpm-ostree-0-integration.conf", NULL,
                          rootfs_dfd, tmpfiles_integration_path,
                          GLNX_FILE_COPY_NOXATTRS, /* Don't take selinux label */
                          cancellable, error))
    return FALSE;

  /* Handle kernel/initramfs if we're not doing a container */
  if (!container)
    {
      g_print ("Preparing kernel\n");

      /* OSTree needs to own this */
      if (!glnx_shutil_rm_rf_at (rootfs_dfd, "boot/loader", cancellable, error))
        return FALSE;

      if (!process_kernel_and_initramfs (rootfs_dfd, treefile, unified_core_mode,
                                         cancellable, error))
        return glnx_prefix_error (error, "During kernel processing");
    }

  /* we're composing a new tree; copy the rpmdb to the base location */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, RPMOSTREE_RPMDB_LOCATION, NULL,
                                 AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      if (!glnx_shutil_mkdir_p_at (rootfs_dfd, RPMOSTREE_BASE_RPMDB, 0755,
                                   cancellable, error))
        return FALSE;
      if (!hardlink_recurse (rootfs_dfd, RPMOSTREE_RPMDB_LOCATION,
                             rootfs_dfd, RPMOSTREE_BASE_RPMDB,
                             cancellable, error))
        return glnx_prefix_error (error, "Hardlinking %s", RPMOSTREE_BASE_RPMDB);
      /* And write a symlink from the proposed standard /usr/lib/sysimage/rpm
       * to our /usr/share/rpm - eventually we will invert this.
       */
     if (symlinkat ("../../share/rpm", rootfs_dfd, RPMOSTREE_SYSIMAGE_RPMDB) < 0)
        return glnx_throw_errno_prefix (error, "symlinking %s", RPMOSTREE_SYSIMAGE_RPMDB);
    }

  return TRUE;
}

gboolean
rpmostree_rootfs_symlink_emptydir_at (int rootfs_fd,
                                      const char *dest,
                                      const char *src,
                                      GError **error)
{
  const char *parent = dirname (strdupa (src));
  struct stat stbuf;
  gboolean make_symlink = TRUE;

  /* For maximum compatibility, create parent directories too.  This
   * is necessary when we're doing layering on top of a base commit,
   * and the /var will be empty.  We should probably consider running
   * systemd-tmpfiles to setup the temporary /var.
   */
  if (parent && strcmp (parent, ".") != 0)
    {
      if (!glnx_shutil_mkdir_p_at (rootfs_fd, parent, 0755, NULL, error))
        return FALSE;
    }

  if (!glnx_fstatat_allow_noent (rootfs_fd, src, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      if (S_ISLNK (stbuf.st_mode))
        make_symlink = FALSE;
      else if (S_ISDIR (stbuf.st_mode))
        {
          if (!glnx_unlinkat (rootfs_fd, src, AT_REMOVEDIR, error))
            return FALSE;
        }
    }

  if (make_symlink)
    {
      if (symlinkat (dest, rootfs_fd, src) < 0)
        return glnx_throw_errno_prefix (error, "Symlinking %s", src);
    }
  return TRUE;
}

/**
 * rpmostree_rootfs_prepare_links:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  - Symlink /usr/local -> /var/usrlocal
 *  - Symlink /var/lib/alternatives -> /usr/lib/alternatives
 *  - Symlink /var/lib/vagrant -> /usr/lib/vagrant
 */
gboolean
rpmostree_rootfs_prepare_links (int           rootfs_fd,
                                GCancellable *cancellable,
                                GError       **error)
{
  if (!glnx_shutil_rm_rf_at (rootfs_fd, "usr/local", cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../var/usrlocal", "usr/local", error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/lib/alternatives", 0755, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../../usr/lib/alternatives", "var/lib/alternatives", error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/lib/vagrant", 0755, cancellable, error))
    return FALSE;
  if (!rpmostree_rootfs_symlink_emptydir_at (rootfs_fd, "../../usr/lib/vagrant", "var/lib/vagrant", error))
    return FALSE;

  return TRUE;
}

static gboolean
cleanup_leftover_files (int            rootfs_fd,
                        const char    *subpath,
                        const char    *files[],
                        const char    *prefixes[],
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (rootfs_fd, subpath, TRUE, &dfd_iter, error))
    return glnx_prefix_error (error, "Opening %s", subpath);

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;
      if (dent->d_type != DT_REG)
        continue;

      const char *name = dent->d_name;

      const gboolean in_files = (files != NULL && g_strv_contains (files, name));
      const gboolean has_prefix =
        (prefixes != NULL && rpmostree_str_has_prefix_in_strv (name, (char**)prefixes, -1));

      if (!in_files && !has_prefix)
        continue;

      if (!glnx_unlinkat (dfd_iter.fd, name, 0, error))
        return FALSE;
    }

  return TRUE;
}

static const char *selinux_leftover_files[] = { "semanage.trans.LOCK",
                                                "semanage.read.LOCK", NULL };
static const char *rpmdb_leftover_files[] = { ".dbenv.lock",
                                              ".rpm.lock", NULL };
static const char *rpmdb_leftover_prefixes[] = { "__db.", NULL };

static gboolean
cleanup_selinux_lockfiles (int            rootfs_fd,
                           GCancellable  *cancellable,
                           GError       **error)
{
  if (!glnx_fstatat_allow_noent (rootfs_fd, "usr/etc/selinux", NULL, 0, error))
    return FALSE;

  if (errno == ENOENT)
    return TRUE; /* Note early return */

  /* really we only have to do this for the active policy, but let's scan all the dirs */
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (rootfs_fd, "usr/etc/selinux", FALSE, &dfd_iter, error))
    return glnx_prefix_error (error, "Opening /usr/etc/selinux");

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;
      if (dent->d_type != DT_DIR)
        continue;

      const char *name = dent->d_name;
      if (!cleanup_leftover_files (dfd_iter.fd, name, selinux_leftover_files, NULL,
                                   cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_cleanup_leftover_rpmdb_files (int            rootfs_fd,
                                        GCancellable  *cancellable,
                                        GError       **error)
{
  return cleanup_leftover_files (rootfs_fd, RPMOSTREE_RPMDB_LOCATION, rpmdb_leftover_files,
                                 rpmdb_leftover_prefixes, cancellable, error);
}

/**
 * rpmostree_rootfs_postprocess_common:
 *
 * Walk over the root filesystem and perform some core conversions
 * from RPM conventions to OSTree conventions.  For example:
 *
 *  - Move /etc to /usr/etc
 *  - Move /var/lib/rpm to /usr/share/rpm
 *  - Clean up RPM db leftovers
 *  - Clean /usr/etc/passwd- backup files and such
 *
 * This function is idempotent; it may be called multiple times with
 * no further effect after the first.
 */
gboolean
rpmostree_rootfs_postprocess_common (int           rootfs_fd,
                                     GCancellable *cancellable,
                                     GError       **error)
{
  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  if (!glnx_fstatat_allow_noent (rootfs_fd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  const bool have_rpmdb = (errno == 0);
  if (!have_rpmdb)
    {
      /* Try looking in var/lib/rpm */
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (rootfs_fd, "var/lib/rpm", &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == 0 && S_ISDIR(stbuf.st_mode))
        {
          if (!rename_if_exists (rootfs_fd, "var/lib/rpm", rootfs_fd, RPMOSTREE_RPMDB_LOCATION, error))
            return FALSE;
          if (symlinkat ("../../" RPMOSTREE_RPMDB_LOCATION, rootfs_fd, "var/lib/rpm") < 0)
            return glnx_throw_errno_prefix (error, "symlinkat(%s)", "var/lib/rpm");
        }
    }

  if (!rpmostree_cleanup_leftover_rpmdb_files (rootfs_fd, cancellable, error))
    return FALSE;

  if (!cleanup_selinux_lockfiles (rootfs_fd, cancellable, error))
    return FALSE;

  rpmostreecxx::passwd_cleanup(rootfs_fd);

  return TRUE;
}

/* Move etc -> usr/etc in the rootfs, and run through treefile
 * postprocessing.
 */
gboolean
rpmostree_treefile_postprocessing (int            rootfs_fd,
                                   rpmostreecxx::Treefile &treefile_rs,
                                   JsonObject    *treefile,
                                   const char    *next_version,
                                   gboolean       unified_core_mode,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  g_assert (treefile);

  treefile_rs.write_compose_json(rootfs_fd);

  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  rpmostreecxx::compose_postprocess_targets(rootfs_fd, treefile_rs);

  /* Put /etc back for backwards compatibility */
  if (!rename_if_exists (rootfs_fd, "usr/etc", rootfs_fd, "etc", error))
    return FALSE;

  rpmostreecxx::compose_postprocess_remove_files(rootfs_fd, treefile_rs);

  {
    const char *base_version = NULL;

    if (!_rpmostree_jsonutil_object_get_optional_string_member (treefile,
                                                                "mutate-os-release",
                                                                &base_version,
                                                                error))
      return FALSE;

    if (base_version != NULL && next_version == NULL)
      {
        g_print ("Ignoring mutate-os-release: no commit version specified.\n");
      }
    else if (base_version != NULL)
      {
        /* find the real path to os-release using bwrap; this is an overkill but safer way
         * of resolving a symlink relative to a rootfs (see discussions in
         * https://github.com/projectatomic/rpm-ostree/pull/410/) */
        g_autofree char *pathbuf = NULL;
        const char *path = NULL;
        {
          g_autoptr(RpmOstreeBwrap) bwrap =
            rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_IMMUTABLE, error);
          if (!bwrap)
            return FALSE;

          /* map back to /etc so relative symlinks work */
          rpmostree_bwrap_append_child_argv (bwrap, "realpath", "-z", "/etc/os-release", NULL);

          g_autoptr(GBytes) out = NULL;
          if (!rpmostree_bwrap_run_captured (bwrap, &out, NULL, cancellable, error))
            return FALSE;

          gsize len;
          pathbuf = (char*)g_bytes_unref_to_data (util::move_nullify (out), &len);

          /* if realpath returned successfully, it must've printed something */
          g_assert_cmpuint (len, >, 0);
          g_assert_cmpuint (pathbuf[0], ==, '/');
          pathbuf[len-1] = '\0';

          path = pathbuf+1; /* skip initial '/' */
        }

        /* fallback on just overwriting etc/os-release */
        if (!path)
          path = "etc/os-release";

        g_print ("Mutating /%s\n", path);

        g_autofree char *contents = glnx_file_get_contents_utf8_at (rootfs_fd, path, NULL,
                                                                    cancellable, error);
        if (contents == NULL)
          return FALSE;

        auto new_contents = rpmostreecxx::mutate_os_release (contents, base_version, next_version);
        if (!glnx_file_replace_contents_at (rootfs_fd, path,
                                            (guint8*)new_contents.data(), new_contents.length(), static_cast<GLnxFileReplaceFlags>(0),
                                            cancellable, error))
          return FALSE;
      }
  }

  /* Undo etc move again */
  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  rpmostreecxx::compose_postprocess(rootfs_fd, treefile_rs, (bool)unified_core_mode);

  return TRUE;
}

/**
 * rpmostree_prepare_rootfs_for_commit:
 *
 * Initialize a basic root filesystem in @target_root_dfd, then walk over the
 * root filesystem in @src_rootfs_fd and take the basic content and perform some
 * core conversions from RPM conventions to OSTree conventions. For example:
 *
 *  * Checksum the kernel in /boot (and move kernel /usr/lib/modules)
 *  * Migrate content in /var to systemd-tmpfiles
 */
gboolean
rpmostree_prepare_rootfs_for_commit (int            src_rootfs_dfd,
                                     int            target_rootfs_dfd,
                                     JsonObject    *treefile,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  /* Initialize target root */
  g_print ("Initializing rootfs\n");
  gboolean tmp_is_dir = FALSE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treefile,
                                                               "tmp-is-dir",
                                                               &tmp_is_dir,
                                                               error))
    return FALSE;
  /* Make an empty root */
  if (!init_rootfs (target_rootfs_dfd, tmp_is_dir, cancellable, error))
    return FALSE;

  g_print ("Moving /usr to target\n");
  if (!glnx_renameat (src_rootfs_dfd, "usr", target_rootfs_dfd, "usr", error))
    return FALSE;

  /* The kernel may be in the source rootfs /boot; to handle that, we always
   * rename the source /boot to the target, and will handle everything after
   * that in the target root.
   */
  if (!rename_if_exists (src_rootfs_dfd, "boot", target_rootfs_dfd, "boot", error))
    return FALSE;

  /* And grab /var - we'll convert to tmpfiles.d later */
  if (!rename_if_exists (src_rootfs_dfd, "var", target_rootfs_dfd, "var", error))
    return FALSE;

  /* Also carry along toplevel compat links */
  g_print ("Copying toplevel compat symlinks\n");
  {
    const char *toplevel_links[] = { "lib", "lib64", "lib32",
                                     "bin", "sbin" };
    for (guint i = 0; i < G_N_ELEMENTS (toplevel_links); i++)
      {
        if (!glnx_fstatat_allow_noent (src_rootfs_dfd, toplevel_links[i], NULL,
                                       AT_SYMLINK_NOFOLLOW, error))
          return FALSE;
        if (errno == ENOENT)
          continue;

        if (!glnx_renameat (src_rootfs_dfd, toplevel_links[i],
                            target_rootfs_dfd, toplevel_links[i], error))
          return FALSE;
      }
  }

  return TRUE;
}

struct CommitThreadData {
  gint done;  /* atomic */
  off_t n_bytes;
  off_t n_processed;
  gint percent;  /* atomic */
  std::unique_ptr<rpmostreecxx::Progress> progress;
  OstreeRepo *repo;
  int rootfs_fd;
  OstreeMutableTree *mtree;
  OstreeSePolicy *sepolicy;
  OstreeRepoCommitModifier *commit_modifier;
  gboolean success;
  GCancellable *cancellable;
  GError **error;
};

static GVariant *
filter_xattrs_impl (OstreeRepo     *repo,
                  const char     *relpath,
                  GFileInfo      *file_info,
                  gpointer        user_data)
{
  auto tdata = static_cast<struct CommitThreadData *>(user_data);
  int rootfs_fd = tdata->rootfs_fd;
  /* If you have a use case for something else, file an issue */
  static const char *accepted_xattrs[] =
    { "security.capability", /* https://lwn.net/Articles/211883/ */
      "user.pax.flags" /* https://github.com/projectatomic/rpm-ostree/issues/412 */
    };
  g_autoptr(GVariant) existing_xattrs = NULL;
  g_autoptr(GVariantIter) viter = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  GVariant *key, *value;
  GVariantBuilder builder;

  if (relpath[0] == '/')
    relpath++;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  if (!*relpath)
    {
      if (!glnx_fd_get_all_xattrs (rootfs_fd, &existing_xattrs, NULL, error))
        util::throw_gerror(local_error);
    }
  else
    {
      if (!glnx_dfd_name_get_all_xattrs (rootfs_fd, relpath, &existing_xattrs,
                                         NULL, error))
        util::throw_gerror(local_error);
    }

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
    {
      tdata->n_processed += g_file_info_get_size (file_info);
      g_atomic_int_set (&tdata->percent, (gint)((100.0*tdata->n_processed)/tdata->n_bytes));
    }

  viter = g_variant_iter_new (existing_xattrs);

  while (g_variant_iter_loop (viter, "(@ay@ay)", &key, &value))
    {
      for (guint i = 0; i < G_N_ELEMENTS (accepted_xattrs); i++)
        {
          const char *validkey = accepted_xattrs[i];
          const char *attrkey = g_variant_get_bytestring (key);
          if (g_str_equal (validkey, attrkey))
            g_variant_builder_add (&builder, "(@ay@ay)", key, value);
        }
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Filters out all xattrs that aren't accepted. */
static GVariant *
filter_xattrs_cb (OstreeRepo     *repo,
                  const char     *relpath,
                  GFileInfo      *file_info,
                  gpointer        user_data)
{
  g_assert (relpath);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  try {
    return filter_xattrs_impl(repo, relpath, file_info, user_data);
  } catch (std::exception& e) {
      g_variant_builder_clear (&builder);
      /* Unfortunately we have no way to throw from this callback */
      g_printerr ("Failed to read xattrs of '%s': %s\n", relpath, e.what());
      exit (1);
  }
}

static gboolean
count_filesizes (int dfd,
                 const char *path,
                 off_t *out_n_bytes,
                 GCancellable *cancellable,
                 GError **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  
  if (!glnx_dirfd_iterator_init_at (dfd, path, TRUE, &dfd_iter, error))
    return FALSE;
  
  while (TRUE)
    {
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (!dent)
        break;

      if (dent->d_type == DT_DIR)
        {
          if (!count_filesizes (dfd_iter.fd, dent->d_name, out_n_bytes,
                                cancellable, error))
            return FALSE;
        }
      else
        {
          struct stat stbuf;

          if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;

          (*out_n_bytes) += stbuf.st_size;
        }
    }

  return TRUE;
}

static gpointer
write_dfd_thread (gpointer datap)
{
  auto data = static_cast<struct CommitThreadData *>(datap);

  if (!ostree_repo_write_dfd_to_mtree (data->repo, data->rootfs_fd, ".",
                                       data->mtree,
                                       data->commit_modifier,
                                       data->cancellable, data->error))
    goto out;

  data->success = TRUE;
 out:
  g_atomic_int_inc (&data->done);
  g_main_context_wakeup (NULL);
  return NULL;
}

static gboolean
on_progress_timeout (gpointer datap)
{
  auto data = static_cast<struct CommitThreadData *>(datap);
  const gint percent = g_atomic_int_get (&data->percent);

  /* clamp to 100 if it somehow goes over (XXX: bad counting?) */
  data->progress->percent_update(MIN(percent, 100));

  return TRUE;
}

/* This is the server-side-only variant; see also the code in rpmostree-core.c
 * for all the other cases like client side layering and `ex container` for
 * buildroots.
 */
gboolean
rpmostree_compose_commit (int            rootfs_fd,
                          OstreeRepo    *repo,
                          const char    *parent_revision,
                          GVariant      *metadata,
                          const char    *gpg_keyid,
                          gboolean       enable_selinux,
                          OstreeRepoDevInoCache *devino_cache,
                          char         **out_new_revision,
                          GCancellable  *cancellable,
                          GError       **error)
{
  g_autoptr(OstreeSePolicy) sepolicy = NULL;
  if (enable_selinux)
    {
      sepolicy = ostree_sepolicy_new_at (rootfs_fd, cancellable, error);
      if (!sepolicy)
        return FALSE;
    }

  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  /* We may make this configurable if someone complains about including some
   * unlabeled content, but I think the fix for that is to ensure that policy is
   * labeling it.
   *
   * Also right now we unconditionally use the CONSUME flag, but this will need
   * to change for the split compose/commit root patches.
   */
  auto modifier_flags = static_cast<OstreeRepoCommitModifierFlags>(OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED |
    OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME);
  /* If changing this, also look at changing rpmostree-unpacker.c */
  g_autoptr(OstreeRepoCommitModifier) commit_modifier =
    ostree_repo_commit_modifier_new (modifier_flags, NULL, NULL, NULL);
  struct CommitThreadData tdata = { 0, };
  ostree_repo_commit_modifier_set_xattr_callback (commit_modifier,
                                                  filter_xattrs_cb, NULL,
                                                  &tdata);

  if (sepolicy && ostree_sepolicy_get_name (sepolicy) != NULL)
    ostree_repo_commit_modifier_set_sepolicy (commit_modifier, sepolicy);
  else if (enable_selinux)
    return glnx_throw (error, "SELinux enabled, but no policy found");

  if (devino_cache)
    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

  off_t n_bytes = 0;
  if (!count_filesizes (rootfs_fd, ".", &n_bytes, cancellable, error))
    return FALSE;

  tdata.n_bytes = n_bytes;
  tdata.repo = repo;
  tdata.rootfs_fd = rootfs_fd;
  tdata.mtree = mtree;
  tdata.sepolicy = sepolicy;
  tdata.commit_modifier = commit_modifier;
  tdata.error = error;

  {
    g_autoptr(GThread) commit_thread = g_thread_new ("commit", write_dfd_thread, &tdata);

    tdata.progress = rpmostreecxx::progress_percent_begin("Committing");

    g_autoptr(GSource) progress_src = g_timeout_source_new_seconds (1);
    g_source_set_callback (progress_src, on_progress_timeout, &tdata, NULL);
    g_source_attach (progress_src, NULL);

    while (g_atomic_int_get (&tdata.done) == 0)
      g_main_context_iteration (NULL, TRUE);

    g_source_destroy (progress_src);
    g_thread_join (util::move_nullify (commit_thread));

    tdata.progress->percent_update(100);
  }

  if (!tdata.success)
    return glnx_prefix_error (error, "While writing rootfs to mtree");

  g_autoptr(GFile) root_tree = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root_tree, cancellable, error))
    return glnx_prefix_error (error, "While writing tree");

  g_autofree char *new_revision = NULL;
  if (!ostree_repo_write_commit (repo, parent_revision, "", "", metadata,
                                 (OstreeRepoFile*)root_tree, &new_revision,
                                 cancellable, error))
    return glnx_prefix_error (error, "While writing commit");

  if (gpg_keyid)
    {
      if (!ostree_repo_sign_commit (repo, new_revision, gpg_keyid, NULL,
                                    cancellable, error))
        return glnx_prefix_error (error, "While signing commit");
    }

  if (out_new_revision)
    *out_new_revision = util::move_nullify (new_revision);

  return TRUE;
}
