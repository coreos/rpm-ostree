/*
 * Copyright (C) 2015,2017 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "ostree.h"

#include <sys/types.h>
#include <sys/xattr.h>
#include <systemd/sd-journal.h>
#include <libglnx.h>

#include "rpmostreed-transaction-types.h"
#include "rpmostreed-transaction.h"
#include "rpmostreed-deployment-utils.h"
#include "rpmostreed-sysroot.h"
#include "rpmostree-sysroot-core.h"
#include "rpmostree-util.h"
#include "rpmostree-origin.h"
#include "rpmostree-db.h"
#include "rpmostree-output.h"
#include "rpmostree-core.h"
#include "rpmostreed-utils.h"

#define RPMOSTREE_MESSAGE_LIVEFS_BEGIN SD_ID128_MAKE(30,60,1f,0b,bb,fe,4c,bd,a7,87,23,53,a2,ed,75,81)
#define RPMOSTREE_MESSAGE_LIVEFS_END SD_ID128_MAKE(d6,8a,b4,d9,d1,32,4a,32,8f,f8,c6,24,1c,6e,b3,c3)

typedef struct {
  RpmostreedTransaction parent;
  RpmOstreeTransactionLiveFsFlags flags;
} LiveFsTransaction;

typedef RpmostreedTransactionClass LiveFsTransactionClass;

GType livefs_transaction_get_type (void);

G_DEFINE_TYPE (LiveFsTransaction,
               livefs_transaction,
               RPMOSTREED_TYPE_TRANSACTION)

static void
livefs_transaction_finalize (GObject *object)
{
  G_GNUC_UNUSED LiveFsTransaction *self;

  self = (LiveFsTransaction *) object;

  G_OBJECT_CLASS (livefs_transaction_parent_class)->finalize (object);
}

typedef enum {
  COMMIT_DIFF_FLAGS_ETC = (1<< 0), /* Change in /usr/etc */
  COMMIT_DIFF_FLAGS_BOOT = (1<< 1), /* Change in /boot */
  COMMIT_DIFF_FLAGS_ROOTFS = (1 << 2), /* Change in / */
  COMMIT_DIFF_FLAGS_REPLACEMENT = (1 << 3)  /* Files in /usr were replaced */
} CommitDiffFlags;

typedef struct {
  guint refcount;
  CommitDiffFlags flags;
  guint n_usretc;
  guint n_tmpfilesd;

  char *from;
  char *to;

  /* Files */
  GPtrArray *added; /* Set<GFile> */
  GPtrArray *modified; /* Set<OstreeDiffItem> */
  GPtrArray *removed; /* Set<GFile> */

  /* Package view */
  GPtrArray *removed_pkgs;
  GPtrArray *added_pkgs;
  GPtrArray *modified_pkgs_old;
  GPtrArray *modified_pkgs_new;
} CommitDiff;

static void
commit_diff_unref (CommitDiff *diff)
{
  diff->refcount--;
  if (diff->refcount > 0)
    return;
  g_free (diff->from);
  g_free (diff->to);
  g_clear_pointer (&diff->added, g_ptr_array_unref);
  g_clear_pointer (&diff->modified, g_ptr_array_unref);
  g_clear_pointer (&diff->removed, g_ptr_array_unref);
  g_clear_pointer (&diff->removed_pkgs, g_ptr_array_unref);
  g_clear_pointer (&diff->added_pkgs, g_ptr_array_unref);
  g_clear_pointer (&diff->modified_pkgs_old, g_ptr_array_unref);
  g_clear_pointer (&diff->modified_pkgs_new, g_ptr_array_unref);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CommitDiff, commit_diff_unref);

static gboolean
path_is_boot (const char *path)
{
  return g_str_has_prefix (path, "/boot/") ||
    g_str_has_prefix (path, "/usr/lib/ostree-boot/");
}

static gboolean
path_is_usretc (const char *path)
{
  return g_str_has_prefix (path, "/usr/etc/");
}

static gboolean
path_is_rpmdb (const char *path)
{
  return g_str_has_prefix (path, "/" RPMOSTREE_RPMDB_LOCATION "/");
}

static gboolean
path_is_rootfs (const char *path)
{
  return !g_str_has_prefix (path, "/usr/");
}

static gboolean
path_is_ignored_for_diff (const char *path)
{
  /* /proc SELinux labeling is broken, ignore it
   * https://github.com/ostreedev/ostree/pull/768
   */
  return strcmp (path, "/proc") == 0;
}

typedef enum {
  FILE_DIFF_RESULT_KEEP,
  FILE_DIFF_RESULT_OMIT,
} FileDiffResult;

/* Given a file path, update @diff's global flags which track high level
 * modifications, and return whether or not a change to this file should be
 * ignored.
 */
static FileDiffResult
diff_one_path (CommitDiff        *diff,
               const char        *path)
{
  if (path_is_ignored_for_diff (path) ||
      path_is_rpmdb (path))
    return FILE_DIFF_RESULT_OMIT;
  else if (path_is_usretc (path))
    {
      diff->flags |= COMMIT_DIFF_FLAGS_ETC;
      diff->n_usretc++;
    }
  else if (g_str_has_prefix (path, "/usr/lib/tmpfiles.d"))
    diff->n_tmpfilesd++;
  else if (path_is_boot (path))
    diff->flags |= COMMIT_DIFF_FLAGS_BOOT;
  else if (path_is_rootfs (path))
    diff->flags |= COMMIT_DIFF_FLAGS_ROOTFS;
  return FILE_DIFF_RESULT_KEEP;
}

static gboolean
copy_new_config_files (OstreeRepo          *repo,
                       OstreeDeployment    *merge_deployment,
                       int                  new_deployment_dfd,
                       OstreeSePolicy      *sepolicy,
                       CommitDiff          *diff,
                       GCancellable        *cancellable,
                       GError             **error)
{
  g_auto(RpmOstreeProgress) task = { 0, };
  rpmostree_output_task_begin (&task, "Copying new config files");

  /* Initialize checkout options; we want to make copies, and don't replace any
   * existing files.
   */
  OstreeRepoCheckoutAtOptions etc_co_opts = { .force_copy = TRUE,
                                              .overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES };
  /* Use SELinux policy if it's initialized */
  if (ostree_sepolicy_get_name (sepolicy) != NULL)
    etc_co_opts.sepolicy = sepolicy;

  glnx_autofd int deployment_etc_dfd = -1;
  if (!glnx_opendirat (new_deployment_dfd, "etc", TRUE, &deployment_etc_dfd, error))
    return FALSE;

  guint n_added = 0;
  /* Avoid checking out added subdirs recursively */
  g_autoptr(GPtrArray) added_subdirs = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < diff->added->len; i++)
    {
      GFile *added_f = diff->added->pdata[i];
      const char *path = gs_file_get_path_cached (added_f);
      if (!g_str_has_prefix (path, "/usr/etc/"))
        continue;
      const char *etc_path = path + strlen ("/usr");

      etc_co_opts.subpath = path;
      /* Strip off /usr for selinux labeling */
      etc_co_opts.sepolicy_prefix = etc_path;

      const char *sub_etc_relpath = etc_path + strlen ("/etc/");
      /* We keep track of added subdirectories and skip children of it, since
       * both the diff and checkout are recursive, but we only need to checkout
       * the directory, which will get all children. To do better I'd say we
       * should add an option to ostree_repo_diff() to avoid recursing into
       * changed subdirectories.  But at the scale we're dealing with here the
       * constants for this O(N²) algorithm are tiny.
       */
      if (rpmostree_str_has_prefix_in_ptrarray (sub_etc_relpath, added_subdirs))
        continue;

      g_autoptr(GFileInfo) finfo = g_file_query_info (added_f, "standard::type",
                                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                      cancellable, error);
      if (!finfo)
        return FALSE;

      /* If this is a directory, add it to our "added subdirs" set. See above.  Add
       * a trailing / to ensure we don't match other file prefixes.
       */
      const gboolean is_dir = g_file_info_get_file_type (finfo) == G_FILE_TYPE_DIRECTORY;
      if (is_dir)
        g_ptr_array_add (added_subdirs, g_strconcat (sub_etc_relpath, "/", NULL));

      /* And now, to deal with ostree semantics around subpath checkouts,
       * we want '.' for files, otherwise get the real dir name.  See also
       * the comment in ostree-repo-checkout.c:checkout_tree_at().
       */
      /* First, get the destination parent dfd */
      glnx_autofd int dest_dfd = -1;
      g_autofree char *dnbuf = g_strdup (sub_etc_relpath);
      const char *dn = dirname (dnbuf);
      if (!glnx_opendirat (deployment_etc_dfd, dn, TRUE, &dest_dfd, error))
        return FALSE;
      const char *dest_path;
      /* Is it a non-directory?  OK, we check out into the parent directly */
      if (!is_dir)
        {
          dest_path = ".";
        }
      else
        {
          /* For directories, we need to match the target's name and hence
           * create a new directory. */
          dest_path = glnx_basename (sub_etc_relpath);
        }

      if (!ostree_repo_checkout_at (repo, &etc_co_opts,
                                    dest_dfd, dest_path,
                                    ostree_deployment_get_csum (merge_deployment),
                                    cancellable, error))
        return g_prefix_error (error, "Copying %s: ", path), FALSE;
      n_added++;
    }
  rpmostree_output_progress_end_msg (&task, "%u", n_added);
  return TRUE;
}

/* Generate a CommitDiff */
static gboolean
analyze_commit_diff (OstreeRepo      *repo,
                     const char      *from_rev,
                     const char      *to_rev,
                     CommitDiff     **out_diff,
                     GCancellable    *cancellable,
                     GError         **error)
{
  g_autoptr(CommitDiff) diff = g_new0(CommitDiff, 1);
  diff->refcount = 1;

  diff->from = g_strdup (from_rev);
  diff->to = g_strdup (to_rev);

  /* Read the "from" and "to" commits */
  glnx_unref_object GFile *from_tree = NULL;
  if (!ostree_repo_read_commit (repo, from_rev, &from_tree, NULL,
                                cancellable, error))
    return FALSE;
  glnx_unref_object GFile *to_tree = NULL;
  if (!ostree_repo_read_commit (repo, to_rev, &to_tree, NULL,
                                cancellable, error))
    return FALSE;

  /* Diff the two commits at the filesystem level */
  g_autoptr(GPtrArray) modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (0, from_tree, to_tree, modified, removed, added,
                         cancellable, error))
    return FALSE;

  /* We'll filter these arrays below. */
  diff->modified = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  diff->removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  diff->added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  /* Analyze the differences */
  for (guint i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diffitem = modified->pdata[i];
      const char *path = gs_file_get_path_cached (diffitem->src);

      if (diff_one_path (diff, path) == FILE_DIFF_RESULT_KEEP)
        g_ptr_array_add (diff->modified, g_object_ref (diffitem->src));
    }

  for (guint i = 0; i < removed->len; i++)
    {
      GFile *gfpath = removed->pdata[i];
      const char *path = gs_file_get_path_cached (gfpath);

      if (diff_one_path (diff, path) == FILE_DIFF_RESULT_KEEP)
        g_ptr_array_add (diff->removed, g_object_ref (gfpath));
    }

  for (guint i = 0; i < added->len; i++)
    {
      GFile *added_f = added->pdata[i];
      const char *path = gs_file_get_path_cached (added_f);

      if (diff_one_path (diff, path) == FILE_DIFF_RESULT_KEEP)
        g_ptr_array_add (diff->added, g_object_ref (added_f));
    }

  /* And gather the RPM level changes */
  if (!rpm_ostree_db_diff (repo, from_rev, to_rev,
                           &diff->removed_pkgs, &diff->added_pkgs,
                           &diff->modified_pkgs_old, &diff->modified_pkgs_new,
                           cancellable, error))
    return FALSE;
  g_assert (diff->modified_pkgs_old->len == diff->modified_pkgs_new->len);

  *out_diff = g_steal_pointer (&diff);
  return TRUE;
}

static void
print_commit_diff (CommitDiff *diff)
{
  /* Print out the results of the two diffs */
  rpmostree_output_message ("Diff Analysis: %s => %s", diff->from, diff->to);
  rpmostree_output_message ("Files: modified: %u removed: %u added: %u",
                            diff->modified->len, diff->removed->len, diff->added->len);
  rpmostree_output_message ("Packages: modified: %u removed: %u added: %u",
                            diff->modified_pkgs_new->len,
                            diff->removed_pkgs->len,
                            diff->added_pkgs->len);

  if (diff->flags & COMMIT_DIFF_FLAGS_ETC)
    {
      rpmostree_output_message ("* Configuration changed in /etc");
    }
  if (diff->flags & COMMIT_DIFF_FLAGS_ROOTFS)
    {
      rpmostree_output_message ("* Content outside of /usr and /etc is modified");
    }
  if (diff->flags & COMMIT_DIFF_FLAGS_BOOT)
    {
      rpmostree_output_message ("* Kernel/initramfs changed");
    }
}

/* We want to ensure the rollback deployment matches our booted checksum. If it
 * doesn't, we'll push a new one, and GC the previous one(s).
 */
static OstreeDeployment *
get_rollback_deployment (OstreeSysroot *sysroot,
                         OstreeDeployment *booted)
{
  const char *booted_csum = ostree_deployment_get_csum (booted);

  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  ostree_sysroot_query_deployments_for (sysroot, ostree_deployment_get_osname (booted),
                                        NULL, &rollback_deployment);
  /* If no rollback found, we're done */
  if (!rollback_deployment)
    return NULL;
  /* We found a rollback, but it needs to match our checksum */
  const char *csum = ostree_deployment_get_csum (rollback_deployment);
  if (strcmp (csum, booted_csum) == 0)
    return g_object_ref (rollback_deployment);
  return NULL;
}

static gboolean
prepare_rollback_deployment (OstreeSysroot  *sysroot,
                             OstreeRepo     *repo,
                             OstreeDeployment *booted_deployment,
                             GCancellable *cancellable,
                             GError **error)
{
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  OstreeBootconfigParser *original_bootconfig = ostree_deployment_get_bootconfig (booted_deployment);
  glnx_unref_object OstreeBootconfigParser *new_bootconfig = ostree_bootconfig_parser_clone (original_bootconfig);

  /* Ensure we have a clean slate */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    return g_prefix_error (error, "Performing initial cleanup: "), FALSE;

  rpmostree_output_message ("Preparing new rollback matching currently booted deployment");

  if (!ostree_sysroot_deploy_tree (sysroot,
                                   ostree_deployment_get_osname (booted_deployment),
                                   ostree_deployment_get_csum (booted_deployment),
                                   ostree_deployment_get_origin (booted_deployment),
                                   booted_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    return FALSE;

  /* Inherit kernel arguments */
  ostree_deployment_set_bootconfig (new_deployment, new_bootconfig);

  if (!rpmostree_syscore_write_deployment (sysroot, new_deployment, booted_deployment,
                                           TRUE, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
checkout_add_usr (OstreeRepo *repo,
                  int deployment_dfd,
                  CommitDiff *diff,
                  const char *target_csum,
                  GCancellable *cancellable,
                  GError **error)
{
  OstreeRepoCheckoutAtOptions usr_checkout_opts = { .mode = OSTREE_REPO_CHECKOUT_MODE_NONE,
                                                    .overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES,
                                                    .no_copy_fallback = TRUE,
                                                    .subpath = "/usr" };

  if (!ostree_repo_checkout_at (repo, &usr_checkout_opts, deployment_dfd, "usr",
                                target_csum, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Even when doing a pure "add" there are some things we need to actually
 * replace:
 *
 *  - rpm database
 *  - /usr/lib/{passwd,group}
 *
 * This function can swap in a new file/directory.
 */
static gboolean
replace_subpath (OstreeRepo *repo,
                 int deployment_dfd,
                 GLnxTmpDir *tmpdir,
                 const char *target_csum,
                 const char *subpath,
                 GCancellable *cancellable,
                 GError **error)
{
  /* The subpath for ostree_repo_checkout_at() must be absolute, but
   * our real filesystem paths must be relative.
   */
  g_assert_cmpint (*subpath, ==, '/');
  const char *relsubpath = subpath += strspn (subpath, "/");
  const char *bname = glnx_basename (relsubpath);

  /* See if it exists, if it does gather stat info; we need to handle
   * directories differently from non-dirs.
   */
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (deployment_dfd, relsubpath, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT)
    return TRUE;  /* Do nothing if the path doesn't exist */

  OstreeRepoCheckoutAtOptions replace_checkout_opts = { .mode = OSTREE_REPO_CHECKOUT_MODE_NONE,
                                                        .no_copy_fallback = TRUE,
                                                        .subpath = subpath };
  /* We need to differentiate nondirs vs dirs; for two reasons.  First,
   * see the /etc path code - ostree_repo_checkout_at() has
   * some legacy bits around checking out individual files.
   *
   * Second, for directories we want RENAME_EXCHANGE if at all possible, but for
   * non-dirs there's no reason not to just use plain old renameat() which will
   * also work atomically even on old kernels (e.g. CentOS7). For some critical
   * files like /usr/lib/passwd we really do want atomicity.
   */
  const gboolean target_is_nondirectory = !S_ISDIR (stbuf.st_mode);
  if (target_is_nondirectory)
    {
      if (!ostree_repo_checkout_at (repo, &replace_checkout_opts, tmpdir->fd, ".",
                                    target_csum, cancellable, error))
        return FALSE;
      if (!glnx_renameat (tmpdir->fd, bname, deployment_dfd, relsubpath, error))
        return FALSE;
    }
  else
    {
      if (!ostree_repo_checkout_at (repo, &replace_checkout_opts, tmpdir->fd, bname,
                                    target_csum, cancellable, error))
        return FALSE;

      if (glnx_renameat2_exchange (tmpdir->fd, bname, deployment_dfd, relsubpath) < 0)
        return glnx_throw_errno_prefix (error, "rename(..., RENAME_EXCHANGE) for %s", subpath);
      /* And nuke the old one */
      if (!glnx_shutil_rm_rf_at (tmpdir->fd, bname, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* The sledgehammer 🔨 approach. Because /usr is a mount point, we can't replace
 * all of it. We could do a diff, but doing that precisely and quickly depends
 * on https://github.com/ostreedev/ostree/issues/1224
 *
 * In this approach, we iterate over and exchange just the subdirectories of
 * /usr (not recursively, as exchanging the toplevels accomplishes that). Some
 * issues here are processes that hold directory fds open will have those
 * invalidated, even if they shouldn't otherwise be affected. Another issue is
 * that if the kernel is too old to have `RENAME_EXCHANGE`, we'll have e.g.
 * `/usr/bin` be temporarily broken.
 *
 * Yet another issue is that doing things all at once makes it much more likely
 * that we'll e.g. replace code for a program before updating a shared library
 * it depends on. There's really no fixing that problem in general, but we could
 * minimize the race window in the same way package systems tend to do by doing
 * the filesystem tree replacements in package reverse dependency order.
 *
 * On the other hand, this handles tricky cases like replacing a directory with
 * a regfile or symlink.
 *
 * Probably what we really want is to have a lightweight replacement path that
 * handles simple updates (e.g. 1-5 packages which just change file content).
 */
static gboolean
replace_usr (OstreeRepo *repo,
             int deployment_dfd,
             GLnxTmpDir *tmpdir,
             CommitDiff *diff,
             const char *target_csum,
             GCancellable *cancellable,
             GError **error)
{
  /* Grab a reference to the current /usr */
  glnx_autofd int deployment_usr_dfd = -1;
  if (!glnx_opendirat (deployment_dfd, "usr", TRUE, &deployment_usr_dfd, error))
    return FALSE;

  /* Check out our new /usr */
  OstreeRepoCheckoutAtOptions usr_checkout_opts = { .mode = OSTREE_REPO_CHECKOUT_MODE_NONE,
                                                    .overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                                    .no_copy_fallback = TRUE,
                                                    .subpath = "/usr" };

  if (!ostree_repo_checkout_at (repo, &usr_checkout_opts, tmpdir->fd, "usr",
                                target_csum, cancellable, error))
    return FALSE;

  /* Iterate over new /usr, see whether the context exists. If not, just
   * rename(). If so, `RENAME_EXCHANGE`; the old content will be rm-rf'd since
   * it will be moved to the tmpdir.
   */
  g_autoptr(GHashTable) seen_new_children = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  if (!glnx_dirfd_iterator_init_at (tmpdir->fd, "usr", TRUE, &dfd_iter, error))
    return FALSE;
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      const char *name = dent->d_name;
      /* Keep track of what entries are in the new /usr */
      g_hash_table_add (seen_new_children, g_strdup (name));
      if (!glnx_fstatat_allow_noent (deployment_usr_dfd, name, NULL, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT)
        {
          if (!glnx_renameat (dfd_iter.fd, name, deployment_usr_dfd, name, error))
            return FALSE;
        }
      else
        {
          if (glnx_renameat2_exchange (dfd_iter.fd, name, deployment_usr_dfd, name) < 0)
            return glnx_throw_errno_prefix (error, "rename(..., RENAME_EXCHANGE) for %s", name);
        }
    }

  /* Iterate over the old /usr, and delete anything that isn't in the new dir */
  glnx_dirfd_iterator_clear (&dfd_iter);
  if (!glnx_dirfd_iterator_init_at (deployment_usr_dfd, ".", TRUE, &dfd_iter, error))
    return FALSE;
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      const char *name = dent->d_name;
      /* See whether this was in the new /usr */
      if (g_hash_table_contains (seen_new_children, name))
        continue;
      /* If not, delete it */
      if (!glnx_shutil_rm_rf_at (dfd_iter.fd, name, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Update the origin for @booted with new livefs state */
static gboolean
write_livefs_state (OstreeSysroot    *sysroot,
                    OstreeDeployment *booted,
                    const char       *live_inprogress,
                    const char       *live,
                    GError          **error)
{
  g_autoptr(RpmOstreeOrigin) new_origin = rpmostree_origin_parse_deployment (booted, error);
  if (!new_origin)
    return FALSE;
  rpmostree_origin_set_live_state (new_origin, live_inprogress, live);
  g_autoptr(GKeyFile) kf = rpmostree_origin_dup_keyfile (new_origin);
  if (!ostree_sysroot_write_origin_file (sysroot, booted, kf, NULL, error))
    return FALSE;
  return TRUE;
}

static gboolean
livefs_transaction_execute_inner (LiveFsTransaction *self,
                                  OstreeSysroot *sysroot,
                                  GCancellable *cancellable,
                                  GError **error)
{
  /* Initial setup - load sysroot, repo, and booted deployment */
  glnx_unref_object OstreeRepo *repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (!booted_deployment)
    return glnx_throw (error, "Not currently booted into an OSTree system");

  /* Overlayfs doesn't support mutation of the lowerdir. And broadly speaking,
   * our medium term goal here is to obviate most of the unlock usage.
   */
  OstreeDeploymentUnlockedState unlockstate = ostree_deployment_get_unlocked (booted_deployment);
  if (unlockstate != OSTREE_DEPLOYMENT_UNLOCKED_NONE)
    return glnx_throw (error, "livefs is incompatible with unlocked state");

  /* Find the source for /etc - either booted or pending, but down below we
     require pending */
  OstreeDeployment *origin_merge_deployment =
    rpmostree_syscore_get_origin_merge_deployment (sysroot,
                                                   ostree_deployment_get_osname (booted_deployment));
  g_assert (origin_merge_deployment);
  const char *booted_csum = ostree_deployment_get_csum (booted_deployment);
  const char *target_csum = ostree_deployment_get_csum (origin_merge_deployment);

  /* Require a pending deployment to use as a source - perhaps in the future we
   * handle direct live overlays.
   */
  if (origin_merge_deployment == booted_deployment)
    return glnx_throw (error, "No pending deployment");
  if (!ostree_deployment_is_staged (origin_merge_deployment))
    return glnx_throw (error, "livefs requires staged deployments");

  /* Open a fd for the booted deployment */
  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (sysroot, booted_deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (ostree_sysroot_get_fd (sysroot), deployment_path, TRUE,
                       &deployment_dfd, error))
    return FALSE;

  /* Find out whether we already have a live overlay */
  g_autofree char *live_inprogress = NULL;
  g_autofree char *live_replaced = NULL;
  if (!rpmostree_syscore_deployment_get_live (booted_deployment, &live_inprogress,
                                              &live_replaced, error))
    return FALSE;
  const char *resuming_overlay = NULL;
  if (live_inprogress != NULL)
    {
      if (strcmp (live_inprogress, target_csum) == 0)
        resuming_overlay = target_csum;
    }
  const char *replacing_overlay = NULL;
  if (live_replaced != NULL)
    {
      if (strcmp (live_replaced, target_csum) == 0)
        return glnx_throw (error, "Current overlay is already %s", target_csum);
      replacing_overlay = live_replaced;
    }

  if (resuming_overlay)
    rpmostree_output_message ("Note: Resuming interrupted overlay of %s", target_csum);
  if (replacing_overlay)
    rpmostree_output_message ("Note: Previous overlay: %s", replacing_overlay);

  /* Look at the difference between the two commits - we could also walk the
   * filesystem, but doing it at the ostree level is potentially faster, since
   * we know when two directories are the same.
   */
  g_autoptr(CommitDiff) diff = NULL;
  if (!analyze_commit_diff (repo, booted_csum, target_csum,
                            &diff, cancellable, error))
    return FALSE;

  print_commit_diff (diff);

  const gboolean replacing = (self->flags & RPMOSTREE_TRANSACTION_LIVEFS_FLAG_REPLACE) > 0;
  const gboolean requires_etc_merge = (diff->flags & COMMIT_DIFF_FLAGS_ETC) > 0;
  const gboolean adds_packages = diff->added_pkgs->len > 0;
  const gboolean modifies_packages = diff->removed_pkgs->len > 0 || diff->modified_pkgs_new->len > 0;
  if ((diff->flags & COMMIT_DIFF_FLAGS_ROOTFS) > 0)
    return glnx_throw (error, "livefs update would modify non-/usr content");
  /* Is this a dry run? */
 /* Error out in various cases if we're not doing a replacement */
  if (!replacing)
    {
      if (!adds_packages)
        return glnx_throw (error, "No packages added; cannot apply");
      if (modifies_packages)
        return glnx_throw (error, "livefs update modifies/replaces packages; cannot apply");
      else if ((diff->flags & COMMIT_DIFF_FLAGS_REPLACEMENT) > 0)
        return glnx_throw (error, "livefs update would replace files in /usr; cannot apply");
    }
  if ((self->flags & RPMOSTREE_TRANSACTION_LIVEFS_FLAG_DRY_RUN) > 0)
    {
      rpmostree_output_message ("livefs OK (dry run)");
      /* Note early return */
      return TRUE;
    }

  g_autoptr(GString) journal_msg = g_string_new ("");
  g_string_append_printf (journal_msg, "Starting livefs for commit %s", target_csum);
  if (resuming_overlay)
    g_string_append (journal_msg, " (resuming)");

  if (replacing)
    g_string_append_printf (journal_msg, " replacement; %u/%u/%u pkgs (added, removed, modified); %u/%u/%u files",
                            diff->added_pkgs->len, diff->removed_pkgs->len, diff->modified_pkgs_old->len,
                            diff->added->len, diff->removed->len, diff->modified->len);
  else
    g_string_append_printf (journal_msg, " addition; %u pkgs, %u files",
                            diff->added_pkgs->len, diff->added->len);

  if (replacing_overlay)
    g_string_append_printf (journal_msg, "; replacing %s", replacing_overlay);
  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_LIVEFS_BEGIN),
                   "MESSAGE=%s", journal_msg->str,
                   "RESUMING=%s", resuming_overlay ?: "",
                   "REPLACING=%s", replacing_overlay ?: "",
                   "BOOTED_COMMIT=%s", booted_csum,
                   "TARGET_COMMIT=%s", target_csum,
                   NULL);
  g_string_truncate (journal_msg, 0);

  /* Ensure that we have a rollback deployment that matches our booted checksum,
   * so that if something goes wrong, the user can get to it.  If we have an
   * older rollback, that gets GC'd.
   */
  OstreeDeployment *rollback_deployment = get_rollback_deployment (sysroot, booted_deployment);
  if (!rollback_deployment)
    {
      if (!prepare_rollback_deployment (sysroot, repo, booted_deployment, cancellable, error))
        return g_prefix_error (error, "Preparing rollback: "), FALSE;
    }

  /* Reload this, the sysroot may have changed it */
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  /* Load SELinux policy for making changes to /etc */
  g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    return FALSE;

  /* Note we inherit the previous value of `live_replaced` (which may be NULL) */
  if (!write_livefs_state (sysroot, booted_deployment, target_csum, live_replaced, error))
    return FALSE;

  g_auto(GLnxTmpDir) replace_tmpdir = { 0, };
  if (!glnx_mkdtempat (ostree_repo_get_dfd (repo), "tmp/rpmostree-livefs.XXXXXX", 0700,
                       &replace_tmpdir, error))
    return FALSE;

  if (!replacing)
    {
      g_auto(RpmOstreeProgress) task = { 0, };
      rpmostree_output_task_begin (&task, "Overlaying /usr");
      if (!checkout_add_usr (repo, deployment_dfd, diff, target_csum, cancellable, error))
        return FALSE;

      /* And files that we always need to replace; the rpmdb, /usr/lib/passwd. We
       * make a tmpdir just for this since it's a more convenient place to put
       * temporary files/dirs without generating tempnames.
       */
      const char *replace_paths[] = { "/" RPMOSTREE_RPMDB_LOCATION, "/usr/lib/passwd", "/usr/lib/group" };
      for (guint i = 0; i < G_N_ELEMENTS(replace_paths); i++)
        {
          const char *replace_path = replace_paths[i];
          if (!replace_subpath (repo, deployment_dfd, &replace_tmpdir,
                                target_csum, replace_path,
                                cancellable, error))
            return FALSE;
        }
    }
  else
    {
      /* Hold my beer 🍺, we're going loop over /usr and RENAME_EXCHANGE things
       * that were modified.
       */
      g_auto(RpmOstreeProgress) task = { 0, };
      rpmostree_output_task_begin (&task, "Replacing /usr");
      if (!replace_usr (repo, deployment_dfd, &replace_tmpdir,
                        diff, target_csum,
                        cancellable, error))
        return FALSE;
    }

  if (diff->n_tmpfilesd > 0)
    {
      const char *tmpfiles_prefixes[] = { "/run/", "/var/"};
      const char *tmpfiles_argv[] = { "systemd-tmpfiles", "--create",
                                      "--prefix", NULL, NULL };

      g_auto(RpmOstreeProgress) task = { 0, };
      rpmostree_output_task_begin (&task, "Running systemd-tmpfiles for /run,/var");

      for (guint i = 0; i < G_N_ELEMENTS (tmpfiles_prefixes); i++)
        {
          const char *prefix = tmpfiles_prefixes[i];
          GLNX_AUTO_PREFIX_ERROR ("Executing systemd-tmpfiles", error);
          tmpfiles_argv[G_N_ELEMENTS(tmpfiles_argv)-2] = prefix;
          g_autoptr(GSubprocess) subproc = g_subprocess_newv ((const char *const*)tmpfiles_argv,
                                                              G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                                              G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                                                              error);
          if (!subproc)
            return FALSE;
          g_autofree char *stderr_buf = NULL;
          if (!g_subprocess_communicate_utf8 (subproc, NULL, cancellable,
                                              NULL, &stderr_buf, error))
            return FALSE;
          int estatus = g_subprocess_get_exit_status (subproc);
          if (!g_spawn_check_exit_status (estatus, error))
            {
              /* Only dump stderr if it actually failed; otherwise we know
               * there are (harmless) warnings, no need to bother everyone.
               */
              sd_journal_print (LOG_ERR, "systemd-tmpfiles failed: %s", stderr_buf);
              return FALSE;
            }
        }
    }

  if (requires_etc_merge)
    {
      if (!copy_new_config_files (repo, origin_merge_deployment,
                                  deployment_dfd, sepolicy, diff,
                                  cancellable, error))
        return FALSE;
    }

  /* Write out the origin as having completed this */
  if (!write_livefs_state (sysroot, booted_deployment, NULL, target_csum, error))
    return FALSE;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_LIVEFS_END),
                   "MESSAGE=Completed livefs for commit %s", target_csum,
                   "BOOTED_COMMIT=%s", booted_csum,
                   "TARGET_COMMIT=%s", target_csum,
                   NULL);

  return TRUE;
}

static gboolean
livefs_transaction_execute (RpmostreedTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)
{
  LiveFsTransaction *self = (LiveFsTransaction *) transaction;
  OstreeSysroot *sysroot = rpmostreed_transaction_get_sysroot (transaction);

  /* Run the transaction */
  gboolean ret = livefs_transaction_execute_inner (self, sysroot, cancellable, error);

  /* We use this to notify ourselves of changes, which is a bit silly, but it
   * keeps things consistent if `ostree admin` is invoked directly.  Always
   * invoke it, in case we error out, so that we correctly update for the
   * partial state.
   */
  (void) rpmostree_syscore_bump_mtime (sysroot, NULL);
  return ret;
}


static void
livefs_transaction_class_init (LiveFsTransactionClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = livefs_transaction_finalize;

  class->execute = livefs_transaction_execute;
}

static void
livefs_transaction_init (LiveFsTransaction *self)
{
}

RpmostreedTransaction *
rpmostreed_transaction_new_livefs (GDBusMethodInvocation *invocation,
                                   OstreeSysroot         *sysroot,
                                   RpmOstreeTransactionLiveFsFlags flags,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  LiveFsTransaction *self;

  g_return_val_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation), NULL);
  g_return_val_if_fail (OSTREE_IS_SYSROOT (sysroot), NULL);

  self = g_initable_new (livefs_transaction_get_type (),
                         cancellable, error,
                         "invocation", invocation,
                         "sysroot-path", gs_file_get_path_cached (ostree_sysroot_get_path (sysroot)),
                         NULL);

  if (self != NULL)
    {
      self->flags = flags;
    }

  return (RpmostreedTransaction *) self;
}
