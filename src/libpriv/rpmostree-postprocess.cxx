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

#include <err.h>
#include <errno.h>
#include <functional>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <grp.h>
#include <json-glib/json-glib.h>
#include <libglnx.h>
#include <ostree.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <utime.h>
#include <vector>

#include "rpmostree-core.h"
#include "rpmostree-kernel.h"
#include "rpmostree-output.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"

typedef enum
{
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW,
  RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES,
} RpmOstreePostprocessBootLocation;

static gboolean
rename_if_exists (int src_dfd, const char *from, int dest_dfd, const char *to, GError **error)
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

/* Handle the kernel/initramfs, which can be in at least 2 different places:
 *  - /boot (CentOS, Fedora treecompose before we suppressed kernel.spec's %posttrans)
 *  - /usr/lib/modules (Fedora treecompose without kernel.spec's %posttrans)
 *
 * We then need to handle the boot-location option, which can put that data in
 * either both (/boot and /usr/lib/ostree-boot), or just the latter.
 */
static gboolean
process_kernel_and_initramfs (int rootfs_dfd, rpmostreecxx::Treefile &treefile,
                              gboolean unified_core_mode, GCancellable *cancellable, GError **error)
{
  /* The current systemd kernel-install will inject
   * /boot/${machine_id}/${uname -r} which we don't use;
   * to avoid confusion, we will delete it.  This relies
   * on systemd itself having set up the machine id from its %post,
   * so we need to read it.  We'll reset the machine ID after this.
   */
  {
    glnx_autofd int fd = openat (rootfs_dfd, "usr/etc/machine-id", O_RDONLY | O_CLOEXEC);
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
  g_autoptr (GVariant) kernelstate = rpmostree_find_kernel (rootfs_dfd, cancellable, error);
  if (!kernelstate)
    return FALSE;
  const char *kernel_path;
  const char *initramfs_path;
  const char *kver;
  const char *bootdir;
  /* Used to optionally hardlink result of our dracut run */
  g_variant_get (kernelstate, "(&s&s&sm&s)", &kver, &bootdir, &kernel_path, &initramfs_path);

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
  ROSCXX_TRY (run_depmod (rootfs_dfd, kver, unified_core_mode), error);

  RpmOstreePostprocessBootLocation boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_NEW;
  if (treefile.get_boot_location_is_modules ())
    boot_location = RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES;

  auto machineid_compat = treefile.get_machineid_compat ();
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
      if (!glnx_file_replace_contents_at (rootfs_dfd, "usr/etc/machine-id", (guint8 *)"", 0,
                                          GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
        return FALSE;
    }
  else
    {
      (void)unlinkat (rootfs_dfd, "usr/etc/machine-id", 0);
    }

  /* Run dracut with our chosen arguments (commonly at least --no-hostonly) */
  g_autoptr (GPtrArray) dracut_argv = g_ptr_array_new ();
  rust::Vec<rust::String> initramfs_args = treefile.get_initramfs_args ();
  if (!initramfs_args.empty ())
    {
      for (auto &arg : initramfs_args)
        g_ptr_array_add (dracut_argv, (void *)arg.c_str ());
    }
  else
    {
      /* Default to this for treecomposes */
      g_ptr_array_add (dracut_argv, (char *)"--no-hostonly");
    }
  g_ptr_array_add (dracut_argv, NULL);

  g_auto (GLnxTmpfile) initramfs_tmpf = {
    0,
  };
  /* We use a tmpdir under the target root since dracut currently tries to copy
   * xattrs, including e.g. user.ostreemeta, which can't be copied to tmpfs.
   */
  {
    g_auto (GLnxTmpDir) dracut_host_tmpd = {
      0,
    };
    if (!glnx_mkdtempat (rootfs_dfd, "rpmostree-dracut.XXXXXX", 0700, &dracut_host_tmpd, error))
      return FALSE;
    if (!rpmostree_run_dracut (rootfs_dfd, (const char *const *)dracut_argv->pdata, kver, NULL,
                               FALSE, &dracut_host_tmpd, &initramfs_tmpf, cancellable, error))
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
  RpmOstreeFinalizeKernelDestination fin_dest
      = (boot_location == RPMOSTREE_POSTPROCESS_BOOT_LOCATION_MODULES)
            ? RPMOSTREE_FINALIZE_KERNEL_USRLIB_MODULES
            : RPMOSTREE_FINALIZE_KERNEL_USRLIB_OSTREEBOOT;
  if (!rpmostree_finalize_kernel (rootfs_dfd, bootdir, kver, kernel_path, &initramfs_tmpf, fin_dest,
                                  cancellable, error))
    return FALSE;

  /* We always ensure this exists as a mountpoint */
  if (!glnx_ensure_dir (rootfs_dfd, "boot", 0755, error))
    return FALSE;

  return TRUE;
}

gboolean
rpmostree_prepare_rootfs_get_sepolicy (int dfd, OstreeSePolicy **out_sepolicy,
                                       GCancellable *cancellable, GError **error)
{
  ROSCXX_TRY (workaround_selinux_cross_labeling (dfd, *cancellable), error);

  g_autoptr (OstreeSePolicy) ret_sepolicy = ostree_sepolicy_new_at (dfd, cancellable, error);
  if (ret_sepolicy == NULL)
    return FALSE;

  *out_sepolicy = util::move_nullify (ret_sepolicy);
  return TRUE;
}

/* Change the policy store location.
 * Part of SELinux in Fedora >= 24: https://bugzilla.redhat.com/show_bug.cgi?id=1290659
 */
gboolean
rpmostree_rootfs_fixup_selinux_store_root (int rootfs_dfd, GCancellable *cancellable,
                                           GError **error)
{
  const char *semanage_path = "usr/etc/selinux/semanage.conf";

  /* Check if the config file exists; if not, do nothing silently */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, semanage_path, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT)
    return TRUE;

  g_autofree char *orig_contents
      = glnx_file_get_contents_utf8_at (rootfs_dfd, semanage_path, NULL, cancellable, error);
  if (orig_contents == NULL)
    return glnx_prefix_error (error, "Opening %s", semanage_path);

  static const char new_store_root[] = "\nstore-root=/etc/selinux\n";
  if (strstr (orig_contents, new_store_root) == NULL)
    {
      g_autofree char *new_contents = g_strconcat (orig_contents, new_store_root, NULL);
      if (!glnx_file_replace_contents_at (rootfs_dfd, semanage_path, (guint8 *)new_contents, -1,
                                          static_cast<GLnxFileReplaceFlags> (0), cancellable,
                                          error))
        return glnx_prefix_error (error, "Replacing %s", semanage_path);
    }

  return TRUE;
}

/* SELinux in Fedora >= 24: https://bugzilla.redhat.com/show_bug.cgi?id=1290659 */
static gboolean
postprocess_selinux_policy_store_location (int rootfs_dfd, GCancellable *cancellable,
                                           GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
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

namespace rpmostreecxx
{

/* All "final" processing; things that are really required to use
 * rpm-ostree on the target host.
 */
gboolean
postprocess_final (int rootfs_dfd, rpmostreecxx::Treefile &treefile, gboolean unified_core_mode,
                   GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finalizing rootfs", error);

  /* Use installation of the tmpfiles integration as an "idempotence" marker to
   * avoid doing postprocessing twice, which can happen when mixing `compose
   * postprocess-root` with `compose commit`.
   */
  const char tmpfiles_integration_path[] = "usr/lib/tmpfiles.d/rpm-ostree-0-integration.conf";
  if (!glnx_fstatat_allow_noent (rootfs_dfd, tmpfiles_integration_path, NULL, AT_SYMLINK_NOFOLLOW,
                                 error))
    return FALSE;
  if (errno == 0)
    return TRUE;

  auto selinux = treefile.get_selinux ();

  ROSCXX_TRY (compose_postprocess_final_pre (rootfs_dfd), error);

  if (selinux)
    {
      g_print ("Recompiling policy\n");

      {
        /* Now regenerate SELinux policy so that postprocess scripts from users and from us
         * (e.g. the /etc/default/useradd incision) that affect it are baked in. */
        rust::Vec child_argv = { rust::String ("semodule"), rust::String ("-nB") };
        ROSCXX_TRY (bubblewrap_run_sync (rootfs_dfd, child_argv, false, (bool)unified_core_mode),
                    error);
      }

      /* Temporary workaround for https://github.com/openshift/os/issues/1036. */
      {
        rust::Vec child_argv
            = { rust::String ("semodule"), rust::String ("-n"), rust::String ("--refresh") };
        ROSCXX_TRY (bubblewrap_run_sync (rootfs_dfd, child_argv, false, (bool)unified_core_mode),
                    error);
      }
    }

  auto container = treefile.get_container ();

  g_print ("Migrating /usr/etc/passwd to /usr/lib/\n");
  ROSCXX_TRY (migrate_passwd_except_root (rootfs_dfd), error);

  rust::Vec<rust::String> preserve_groups_set = treefile.get_etc_group_members ();

  g_print ("Migrating /usr/etc/group to /usr/lib/\n");
  ROSCXX_TRY (migrate_group_except_root (rootfs_dfd, preserve_groups_set), error);

  /* NSS configuration to look at the new files */
  ROSCXX_TRY (composepost_nsswitch_altfiles (rootfs_dfd), error);

  if (selinux)
    {
      if (!postprocess_selinux_policy_store_location (rootfs_dfd, cancellable, error))
        return glnx_prefix_error (error, "SELinux postprocess");
    }

  ROSCXX_TRY (rootfs_prepare_links (rootfs_dfd, false), error);

  if (!unified_core_mode)
    ROSCXX_TRY (convert_var_to_tmpfiles_d (rootfs_dfd, *cancellable), error);
  else
    {
      /* In unified core mode, /var entries are converted to tmpfiles.d at
       * import time and scriptlets are prevented from writing to /var. What
       * remains is just the compat symlinks that we created ourselves, which we
       * should stop writing since it duplicates other tmpfiles.d entries. */
      if (!glnx_shutil_rm_rf_at (rootfs_dfd, "var", cancellable, error))
        return FALSE;
      /* but we still want the mount point as part of the OSTree commit */
      if (mkdirat (rootfs_dfd, "var", 0755) < 0)
        return glnx_throw_errno_prefix (error, "mkdirat(var)");
    }

  if (!rpmostree_rootfs_postprocess_common (rootfs_dfd, cancellable, error))
    return FALSE;

  g_print ("Adding rpm-ostree-0-integration.conf\n");
  /* This is useful if we're running in an uninstalled configuration, e.g.
   * during tests. */
  const char *pkglibdir_path = g_getenv ("RPMOSTREE_UNINSTALLED_PKGLIBDIR") ?: PKGLIBDIR;
  glnx_autofd int pkglibdir_dfd = -1;

  if (!glnx_opendirat (AT_FDCWD, pkglibdir_path, TRUE, &pkglibdir_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (rootfs_dfd, "usr/lib/tmpfiles.d", 0755, cancellable, error))
    return FALSE;

  if (!glnx_file_copy_at (pkglibdir_dfd, "rpm-ostree-0-integration.conf", NULL, rootfs_dfd,
                          tmpfiles_integration_path,
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

      if (!process_kernel_and_initramfs (rootfs_dfd, treefile, unified_core_mode, cancellable,
                                         error))
        return glnx_prefix_error (error, "During kernel processing");
    }

  /* And now the penultimate postprocessing */
  ROSCXX_TRY (compose_postprocess_final (rootfs_dfd, treefile), error);

  return TRUE;
}
}

static gboolean
cleanup_leftover_files (int rootfs_fd, const char *subpath, const char *files[],
                        const char *prefixes[], GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
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
      const gboolean has_prefix
          = (prefixes != NULL && rpmostree_str_has_prefix_in_strv (name, (char **)prefixes, -1));

      if (!in_files && !has_prefix)
        continue;

      if (!glnx_unlinkat (dfd_iter.fd, name, 0, error))
        return FALSE;
    }

  return TRUE;
}

static const char *selinux_leftover_files[] = { "semanage.trans.LOCK", "semanage.read.LOCK", NULL };

static gboolean
cleanup_selinux_lockfiles (int rootfs_fd, GCancellable *cancellable, GError **error)
{
  if (!glnx_fstatat_allow_noent (rootfs_fd, "usr/etc/selinux", NULL, 0, error))
    return FALSE;

  if (errno == ENOENT)
    return TRUE; /* Note early return */

  /* really we only have to do this for the active policy, but let's scan all the dirs */
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
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
      if (!cleanup_leftover_files (dfd_iter.fd, name, selinux_leftover_files, NULL, cancellable,
                                   error))
        return FALSE;
    }

  return TRUE;
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
rpmostree_rootfs_postprocess_common (int rootfs_fd, GCancellable *cancellable, GError **error)
{
  if (!rename_if_exists (rootfs_fd, "etc", rootfs_fd, "usr/etc", error))
    return FALSE;

  if (!glnx_fstatat_allow_noent (rootfs_fd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW,
                                 error))
    return FALSE;
  const bool have_rpmdb = (errno == 0);
  if (!have_rpmdb)
    {
      /* Try looking in var/lib/rpm */
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (rootfs_fd, "var/lib/rpm", &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == 0 && S_ISDIR (stbuf.st_mode))
        {
          if (!rename_if_exists (rootfs_fd, "var/lib/rpm", rootfs_fd, RPMOSTREE_RPMDB_LOCATION,
                                 error))
            return FALSE;
          if (symlinkat ("../../" RPMOSTREE_RPMDB_LOCATION, rootfs_fd, "var/lib/rpm") < 0)
            return glnx_throw_errno_prefix (error, "symlinkat(%s)", "var/lib/rpm");
        }
    }

  /* Make sure there is an RPM macro in place pointing to the rpmdb in /usr */
  ROSCXX_TRY (compose_postprocess_rpm_macro (rootfs_fd), error);

  CXX_TRY (rpmostreecxx::postprocess_cleanup_rpmdb (rootfs_fd), error);

  if (!cleanup_selinux_lockfiles (rootfs_fd, cancellable, error))
    return FALSE;

  ROSCXX_TRY (passwd_cleanup (rootfs_fd), error);

  return TRUE;
}

struct CommitThreadData
{
  gint done; /* atomic */
  off_t n_bytes;
  off_t n_processed;
  gint percent; /* atomic */
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

// In unified core mode, we'll see user-mode checkout files.
// What we want for now is to access the embedded xattr values
// which (along with other canonical file metadata) have been serialized
// into the `user.ostreemeta` extended attribute.  What we should
// actually do is add nice support for this in core ostree, and also
// automatically pick up file owner for example.  This would be a key
// thing to unblock fully unprivileged builds.
//
// But for now, just slurp up the xattrs so we get IMA in particular.
static void
extend_ostree_xattrs (GVariantBuilder *builder, GVariant *vbytes)
{
  g_autoptr (GBytes) bytes = g_variant_get_data_as_bytes (vbytes);
  g_autoptr (GVariant) filemeta = g_variant_ref_sink (
      g_variant_new_from_bytes (OSTREE_FILEMETA_GVARIANT_FORMAT, bytes, false));
  g_autoptr (GVariant) xattrs = g_variant_get_child_value (filemeta, 3);
  g_autoptr (GVariantIter) viter = g_variant_iter_new (xattrs);
  GVariant *key, *value;
  while (g_variant_iter_loop (viter, "(@ay@ay)", &key, &value))
    {
      const char *attrkey = g_variant_get_bytestring (key);
      // Don't try to add the SELinux label - that's handled by the full
      // relabel we do, although in the future it may be interesting to
      // try canonically relying on the labeled pkgcache.
      if (g_str_equal (attrkey, "security.selinux"))
        continue;
      g_variant_builder_add (builder, "(@ay@ay)", key, value);
    }
}

/* Filters out all xattrs that aren't accepted. */
static GVariant *
filter_xattrs_cb (OstreeRepo *repo, const char *relpath, GFileInfo *file_info, gpointer user_data)
{
  g_assert (relpath);

  auto tdata = static_cast<struct CommitThreadData *> (user_data);
  int rootfs_fd = tdata->rootfs_fd;
  /* If you have a use case for something else, file an issue */
  static const char *accepted_xattrs[] = {
    "security.capability", /* https://lwn.net/Articles/211883/ */
    "user.pax.flags",      /* https://github.com/projectatomic/rpm-ostree/issues/412 */
    RPMOSTREE_USER_IMA,    /* will be replaced with security.ima */
  };
  g_autoptr (GVariant) existing_xattrs = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  // From here, ensure the path is relative
  if (relpath[0] == '/')
    relpath++;

  if (!*relpath)
    {
      if (!glnx_fd_get_all_xattrs (rootfs_fd, &existing_xattrs, NULL, error))
        g_error ("Reading xattrs on /: %s", local_error->message);
    }
  else
    {
      if (!glnx_dfd_name_get_all_xattrs (rootfs_fd, relpath, &existing_xattrs, NULL, error))
        g_error ("Reading xattrs on %s: %s", relpath, local_error->message);
    }

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
    {
      tdata->n_processed += g_file_info_get_size (file_info);
      g_atomic_int_set (&tdata->percent, (gint)((100.0 * tdata->n_processed) / tdata->n_bytes));
    }

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  GVariantIter viter;
  g_variant_iter_init (&viter, existing_xattrs);
  // Look for the special user.ostreemeta xattr; if present then it wins
  GVariant *key, *value;
  while (g_variant_iter_loop (&viter, "(@ay@ay)", &key, &value))
    {
      const char *attrkey = g_variant_get_bytestring (key);

      // If it's the special bare-user xattr, then slurp out the embedded
      // xattrs.
      if (g_str_equal (attrkey, "user.ostreemeta"))
        {
          extend_ostree_xattrs (&builder, value);
          return g_variant_ref_sink (g_variant_builder_end (&builder));
        }
    }

  // Otherwise, find the physical xattrs; this happens in the unified core case.
  g_variant_iter_init (&viter, existing_xattrs);
  while (g_variant_iter_loop (&viter, "(@ay@ay)", &key, &value))
    {
      const char *attrkey = g_variant_get_bytestring (key);

      for (guint i = 0; i < G_N_ELEMENTS (accepted_xattrs); i++)
        {
          const char *validkey = accepted_xattrs[i];
          if (g_str_equal (validkey, attrkey))
            {
              // Translate user.ima to its final security.ima value.  This allows handling
              // IMA outside of rpm-ostree, without needing IMA to be enabled on the
              // "host" system.
              if (g_str_equal (validkey, RPMOSTREE_USER_IMA))
                {
                  g_variant_builder_add (&builder, "(@ay@ay)",
                                         g_variant_new_bytestring (RPMOSTREE_SYSTEM_IMA), value);
                }
              else
                g_variant_builder_add (&builder, "(@ay@ay)", key, value);
            }
        }
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gpointer
write_dfd_thread (gpointer datap)
{
  auto data = static_cast<struct CommitThreadData *> (datap);

  data->success
      = ostree_repo_write_dfd_to_mtree (data->repo, data->rootfs_fd, ".", data->mtree,
                                        data->commit_modifier, data->cancellable, data->error);
  g_atomic_int_inc (&data->done);
  g_main_context_wakeup (NULL);
  return NULL;
}

static gboolean
on_progress_timeout (gpointer datap)
{
  auto data = static_cast<struct CommitThreadData *> (datap);
  const gint percent = g_atomic_int_get (&data->percent);

  /* clamp to 100 if it somehow goes over (XXX: bad counting?) */
  data->progress->percent_update (MIN (percent, 100));

  return TRUE;
}

/* This is the server-side-only variant; see also the code in rpmostree-core.c
 * for all the other cases like client side layering and `ex container` for
 * buildroots.
 */
gboolean
rpmostree_compose_commit (int rootfs_fd, OstreeRepo *repo, const char *parent_revision,
                          GVariant *src_metadata, GVariant *detached_metadata,
                          const char *gpg_keyid, gboolean container, RpmOstreeSELinuxMode selinux,
                          OstreeRepoDevInoCache *devino_cache, char **out_new_revision,
                          GCancellable *cancellable, GError **error)
{
  int label_modifier_flags = 0;
  g_autoptr (OstreeSePolicy) sepolicy = NULL;
  if (selinux != RPMOSTREE_SELINUX_MODE_DISABLED)
    {
      sepolicy = ostree_sepolicy_new_at (rootfs_fd, cancellable, error);
      if (!sepolicy)
        return FALSE;
      if (selinux == RPMOSTREE_SELINUX_MODE_V1)
        label_modifier_flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SELINUX_LABEL_V1;
    }

  g_autoptr (OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  /* We may make this configurable if someone complains about including some
   * unlabeled content, but I think the fix for that is to ensure that policy is
   * labeling it.
   *
   * Also right now we unconditionally use the CONSUME flag, but this will need
   * to change for the split compose/commit root patches.
   */
  auto modifier_flags = static_cast<OstreeRepoCommitModifierFlags> (
      OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED
      | OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME | label_modifier_flags);
  /* If changing this, also look at changing rpmostree-unpacker.c */
  g_autoptr (OstreeRepoCommitModifier) commit_modifier
      = ostree_repo_commit_modifier_new (modifier_flags, NULL, NULL, NULL);
  struct CommitThreadData tdata = {
    0,
  };
  ostree_repo_commit_modifier_set_xattr_callback (commit_modifier, filter_xattrs_cb, NULL, &tdata);

  if (sepolicy && ostree_sepolicy_get_name (sepolicy) != NULL)
    ostree_repo_commit_modifier_set_sepolicy (commit_modifier, sepolicy);
  else if (selinux != RPMOSTREE_SELINUX_MODE_DISABLED)
    return glnx_throw (error, "SELinux enabled, but no policy found");

  if (devino_cache)
    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

  CXX_TRY_VAR (n_bytes, rpmostreecxx::directory_size (rootfs_fd, *cancellable), error);

  tdata.n_bytes = n_bytes;
  tdata.repo = repo;
  tdata.rootfs_fd = rootfs_fd;
  tdata.mtree = mtree;
  tdata.sepolicy = sepolicy;
  tdata.commit_modifier = commit_modifier;
  tdata.error = error;

  {
    g_autoptr (GThread) commit_thread = g_thread_new ("commit", write_dfd_thread, &tdata);

    tdata.progress = rpmostreecxx::progress_percent_begin ("Committing");

    g_autoptr (GSource) progress_src = g_timeout_source_new_seconds (1);
    g_source_set_callback (progress_src, on_progress_timeout, &tdata, NULL);
    g_source_attach (progress_src, NULL);

    while (g_atomic_int_get (&tdata.done) == 0)
      g_main_context_iteration (NULL, TRUE);

    g_source_destroy (progress_src);
    g_thread_join (util::move_nullify (commit_thread));

    tdata.progress->percent_update (100);
  }

  if (!tdata.success)
    return glnx_prefix_error (error, "While writing rootfs to mtree");

  g_autoptr (GFile) root_tree = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root_tree, cancellable, error))
    return glnx_prefix_error (error, "While writing tree");

  // Unfortunately these API takes GVariantDict, not GVariantBuilder, so convert
  g_autoptr (GVariantDict) metadata_dict = g_variant_dict_new (src_metadata);

  if (!ostree_repo_commit_add_composefs_metadata (repo, 0, metadata_dict,
                                                  (OstreeRepoFile *)root_tree, cancellable, error))
    return glnx_prefix_error (error, "Adding composefs metadata");

  if (!container)
    {
      if (!ostree_commit_metadata_for_bootable (root_tree, metadata_dict, cancellable, error))
        return glnx_prefix_error (error, "Looking for bootable kernel");
    }
  g_autoptr (GVariant) metadata = g_variant_dict_end (metadata_dict);

  g_autofree char *new_revision = NULL;
  if (!ostree_repo_write_commit (repo, parent_revision, "", "", metadata,
                                 (OstreeRepoFile *)root_tree, &new_revision, cancellable, error))
    return glnx_prefix_error (error, "While writing commit");

  if (detached_metadata != NULL)
    {
      if (!ostree_repo_write_commit_detached_metadata (repo, new_revision, detached_metadata,
                                                       cancellable, error))
        return glnx_prefix_error (error, "While writing detached metadata");
    }

  if (gpg_keyid)
    {
      if (!ostree_repo_sign_commit (repo, new_revision, gpg_keyid, NULL, cancellable, error))
        return glnx_prefix_error (error, "While signing commit");
    }

  if (out_new_revision)
    *out_new_revision = util::move_nullify (new_revision);

  return TRUE;
}
