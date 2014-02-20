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

#include "rpmostree-postprocess.h"
#include <ostree.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
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
update_checksum_from_file (GChecksum    *checksum,
                           GFile        *src,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean ret = FALSE;
  gsize bytes_read;
  char buf[4096];
  gs_unref_object GInputStream *filein = NULL;

  filein = (GInputStream*)g_file_read (src, cancellable, error);
  if (!filein)
    goto out;

  do
    {
      if (!g_input_stream_read_all (filein, buf, sizeof(buf), &bytes_read,
                                    cancellable, error))
        goto out;

      g_checksum_update (checksum, (guint8*)buf, bytes_read);
    }
  while (bytes_read > 0);

  ret = TRUE;
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
  if (!update_checksum_from_file (boot_checksum, kernel_path,
                                  cancellable, error))
    goto out;
  if (!update_checksum_from_file (boot_checksum, initramfs_path,
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

static gboolean
clean_uuid_files (GFile         *dir,
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
          if (!clean_uuid_files (child, cancellable, error))
            goto out;
        }
      else if (strcmp (g_file_info_get_name (file_info), "var_uuid") == 0)
        {
          if (!g_file_delete (child, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/* Prepare a root filesystem, taking mainly the contents of /usr from yumroot */
static gboolean 
create_rootfs_from_yumroot_content (GFile         *targetroot,
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

  /* Plus the RPM database goes in usr/share/rpm */
  {
    gs_unref_object GFile *usrbin_rpm =
      g_file_resolve_relative_path (targetroot, "usr/bin/rpm");
    gs_unref_object GFile *legacyrpm_path =
      g_file_resolve_relative_path (yumroot, "var/lib/rpm");
    gs_unref_object GFile *newrpm_path =
      g_file_resolve_relative_path (targetroot, "usr/share/rpm");
    
    if (g_file_query_exists (usrbin_rpm, NULL))
      {
        gs_unref_object GFile *src_yum_rpmdb_indexes =
          g_file_resolve_relative_path (yumroot, "var/lib/yum");
        gs_unref_object GFile *target_yum_rpmdb_indexes =
          g_file_resolve_relative_path (targetroot, "usr/share/yumdb");

        g_print ("Placing RPM db in /usr/share/rpm\n");
        if (!gs_file_rename (legacyrpm_path, newrpm_path, cancellable, error))
          goto out;

        /* Move the yum database to usr/share/yumdb */
        if (g_file_query_exists (src_yum_rpmdb_indexes, NULL))
          {
            g_print ("Moving %s to %s\n", gs_file_get_path_cached (src_yum_rpmdb_indexes),
                     gs_file_get_path_cached (target_yum_rpmdb_indexes));
            if (!gs_file_rename (src_yum_rpmdb_indexes, target_yum_rpmdb_indexes,
                                 cancellable, error))
              goto out;
            
            if (!clean_uuid_files (target_yum_rpmdb_indexes, cancellable, error))
              goto out;
          }
      }
    else
      {
        gs_unref_object GFile *rpm_manifest_txt =
          g_file_resolve_relative_path (targetroot, "usr/share/rpm-manifest.txt");
        gs_unref_object GSSubprocessContext *rpmqa_ctx = NULL;
        gs_unref_object GSSubprocess *rpmqa_proc = NULL;
        gs_free char *rpmqa_path = g_build_filename (PKGLIBDIR, "rpmqa-sorted", NULL);
        char *rpmqa_argv[] = { rpmqa_path, NULL };
        
        g_print ("No rpm found in build root, generating: %s\n",
                 gs_file_get_path_cached (rpm_manifest_txt));

        rpmqa_ctx = gs_subprocess_context_new (rpmqa_argv);
        gs_subprocess_context_set_cwd (rpmqa_ctx, gs_file_get_path_cached (legacyrpm_path));
        gs_subprocess_context_set_stdout_file_path (rpmqa_ctx, gs_file_get_path_cached (rpm_manifest_txt));
        rpmqa_proc = gs_subprocess_new (rpmqa_ctx, cancellable, error);
        if (!rpmqa_proc)
          goto out;
        
        if (!gs_subprocess_wait_sync_check (rpmqa_proc, cancellable, error))
          goto out;

        if (!gs_shutil_rm_rf (legacyrpm_path, cancellable, error))
          goto out;
      }
  }

  /* Remove /var/lib/yum; we don't want it here. */
  {
    gs_unref_object GFile *yumroot_yumlib = g_file_get_child (yumroot, "var/lib/yum");

    if (!gs_shutil_rm_rf (yumroot_yumlib, cancellable, error))
      goto out;
  }

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

    if (!move_to_dir (yumroot_boot, targetroot, cancellable, error))
      goto out;
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
rpmostree_postprocess (GFile         *rootfs,
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

  if (!create_rootfs_from_yumroot_content (rootfs_tmp, rootfs, cancellable, error))
    goto out;

  if (!gs_shutil_rm_rf (rootfs, cancellable, error))
    goto out;
  if (!gs_file_rename (rootfs_tmp, rootfs, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_commit (GFile         *rootfs,
                  OstreeRepo    *repo,
                  const char    *refname,
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
      sepolicy = ostree_sepolicy_new (rootfs, cancellable, error);
      if (!sepolicy)
        goto out;
    }

  g_print ("Committing '%s' ...\n", gs_file_get_path_cached (rootfs));
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();
  /* For now skip ACLs */
  commit_modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS, NULL, NULL, NULL);
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

  if (!ostree_repo_write_commit (repo, parent_revision, "", "",
                                 NULL, (OstreeRepoFile*)root_tree, &new_revision,
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
