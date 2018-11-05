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
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <libdnf/dnf-repo.h>
#include <sys/mount.h>
#include <stdio.h>
#include <linux/magic.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-compose-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-composeutil.h"
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

static char *opt_workdir;
static gboolean opt_workdir_tmpfs;
static char *opt_cachedir;
static gboolean opt_download_only;
static gboolean opt_force_nocache;
static gboolean opt_cache_only;
static gboolean opt_unified_core;
static char *opt_proxy;
static char *opt_output_repodata_dir;
static char **opt_metadata_strings;
static char *opt_metadata_json;
static char *opt_repo;
static char *opt_touch_if_changed;
static gboolean opt_dry_run;
static gboolean opt_print_only;
static char *opt_write_commitid_to;
static char *opt_write_composejson_to;

/* shared by both install & commit */
static GOptionEntry common_option_entries[] = {
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { NULL }
};

static GOptionEntry install_option_entries[] = {
  { "force-nocache", 0, 0, G_OPTION_ARG_NONE, &opt_force_nocache, "Always create a new OSTree commit, even if nothing appears to have changed", NULL },
  { "cache-only", 0, 0, G_OPTION_ARG_NONE, &opt_cache_only, "Assume cache is present, do not attempt to update it", NULL },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Like --dry-run, but download RPMs as well; requires --cachedir", NULL },
  { "ex-unified-core", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_unified_core, "Compat alias for --unified-core", NULL }, // Compat
  { "unified-core", 0, 0, G_OPTION_ARG_NONE, &opt_unified_core, "Use new \"unified core\" codepath", NULL },
  { "proxy", 0, 0, G_OPTION_ARG_STRING, &opt_proxy, "HTTP proxy", "PROXY" },
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Just print the transaction and exit", NULL },
  { "output-repodata-dir", 0, 0, G_OPTION_ARG_STRING, &opt_output_repodata_dir, "Save downloaded repodata in DIR", "DIR" },
  { "print-only", 0, 0, G_OPTION_ARG_NONE, &opt_print_only, "Just expand any includes and print treefile", NULL },
  { "touch-if-changed", 0, 0, G_OPTION_ARG_STRING, &opt_touch_if_changed, "Update the modification time on FILE if a new commit was created", "FILE" },
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "WORKDIR" },
  { "workdir-tmpfs", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_workdir_tmpfs, "Use tmpfs for working state", NULL },
  { NULL }
};

static GOptionEntry postprocess_option_entries[] = {
  { NULL }
};

static GOptionEntry commit_option_entries[] = {
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "add-metadata-from-json", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_json, "Parse the given JSON file as object, convert to GVariant, append to OSTree commit", "JSON" },
  { "write-commitid-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_commitid_to, "File to write the composed commitid to instead of updating the ref", "FILE" },
  { "write-composejson-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_composejson_to, "Write JSON to FILE containing information about the compose run", "FILE" },
  { NULL }
};

typedef struct {
  RpmOstreeContext *corectx;
  GFile *treefile_path;
  GHashTable *metadata;
  GFile *previous_root;
  GLnxTmpDir workdir_tmp;
  int workdir_dfd;
  int rootfs_dfd;
  int cachedir_dfd;
  gboolean unified_core_and_fuse;
  OstreeRepo *repo;          /* target repo provided by --repo */
  OstreeRepo *build_repo;    /* unified mode: repo we build into */
  OstreeRepo *pkgcache_repo; /* unified mode: pkgcache repo where we import pkgs */
  OstreeRepoDevInoCache *devino_cache;
  const char *ref;
  char *rojig_spec;
  char *previous_checksum;

  RORTreefile *treefile_rs;
  JsonParser *treefile_parser;
  JsonNode *treefile_rootval; /* Unowned */
  JsonObject *treefile; /* Unowned */
  RpmOstreeTreespec   *treespec;
} RpmOstreeTreeComposeContext;

static void
rpm_ostree_tree_compose_context_free (RpmOstreeTreeComposeContext *ctx)
{
  g_clear_object (&ctx->corectx);
  g_clear_object (&ctx->treefile_path);
  g_clear_pointer (&ctx->metadata, g_hash_table_unref);
  g_clear_object (&ctx->previous_root);
  /* Only close workdir_dfd if it's not owned by the tmpdir */
  if (!ctx->workdir_tmp.initialized)
    glnx_close_fd (&ctx->workdir_dfd);
  if (g_getenv ("RPMOSTREE_PRESERVE_TMPDIR"))
    g_print ("Preserved workdir: %s\n", ctx->workdir_tmp.path);
  else
    (void)glnx_tmpdir_delete (&ctx->workdir_tmp, NULL, NULL);
  glnx_close_fd (&ctx->rootfs_dfd);
  glnx_close_fd (&ctx->cachedir_dfd);
  g_clear_object (&ctx->repo);
  g_clear_object (&ctx->build_repo);
  g_clear_object (&ctx->pkgcache_repo);
  g_clear_pointer (&ctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);
  g_free (ctx->previous_checksum);
  g_clear_pointer (&ctx->treefile_rs, (GDestroyNotify) ror_treefile_free);
  g_clear_object (&ctx->treefile_parser);
  g_clear_object (&ctx->treespec);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeTreeComposeContext, rpm_ostree_tree_compose_context_free)

static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

static gboolean
inputhash_from_commit (OstreeRepo *repo,
                       const char *sha256,
                       char      **out_value, /* inout Option<String> */
                       GError    **error)
{
  g_autoptr(GVariant) commit_v = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 sha256, &commit_v, error))
    return FALSE;

  g_autoptr(GVariant) commit_metadata = g_variant_get_child_value (commit_v, 0);
  g_assert (out_value);
  *out_value = NULL;
  g_variant_lookup (commit_metadata, "rpmostree.inputhash", "s", out_value);
  return TRUE;
}

static gboolean
install_packages (RpmOstreeTreeComposeContext  *self,
                  gboolean                     *out_unmodified,
                  char                        **out_new_inputhash,
                  GCancellable                 *cancellable,
                  GError                      **error)
{
  int rootfs_dfd = self->rootfs_dfd;
  DnfContext *dnfctx = rpmostree_context_get_dnf (self->corectx);
  if (opt_proxy)
    dnf_context_set_http_proxy (dnfctx, opt_proxy);

  /* Hack this here... see https://github.com/rpm-software-management/libhif/issues/53
   * but in the future we won't be using librpm at all for unpack/scripts, so it won't
   * matter.
   */
  { const char *debuglevel = getenv ("RPMOSTREE_RPM_VERBOSITY");
    if (!debuglevel)
      debuglevel = "info";
    dnf_context_set_rpm_verbosity (dnfctx, debuglevel);
    rpmlogSetFile(NULL);
  }

  { int tf_dfd = ror_treefile_get_dfd (self->treefile_rs);
    g_autofree char *abs_tf_path = glnx_fdrel_abspath (tf_dfd, ".");
    dnf_context_set_repo_dir (dnfctx, abs_tf_path);
  }

  /* By default, retain packages in addition to metadata with --cachedir, unless
   * we're doing unified core, in which case the pkgcache repo is the cache.  But
   * the rojigSet build still requires the original RPMs too.
   */
  if (opt_cachedir && !opt_unified_core)
    dnf_context_set_keep_cache (dnfctx, TRUE);
  /* For compose, always try to refresh metadata; we're used in build servers
   * where fetching should be cheap.  We also have --cache-only which is
   * used by coreos-assembler.  Today we don't expose the default, but we
   * could add --cache-default or something if someone wanted it.
   */
  rpmostree_context_set_dnf_caching (self->corectx,
                                     opt_cache_only ? RPMOSTREE_CONTEXT_DNF_CACHE_FOREVER :
                                     RPMOSTREE_CONTEXT_DNF_CACHE_NEVER);

  { g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (rootfs_dfd, ".");
    if (!rpmostree_context_setup (self->corectx, tmprootfs_abspath, NULL, self->treespec,
                                  cancellable, error))
      return FALSE;
  }

  /* For unified core, we have a pkgcache repo. This is auto-created under the cachedir. */
  if (opt_unified_core)
    {
      g_assert (self->pkgcache_repo);

      if (!opt_cachedir)
        {
          /* This is part of enabling rpm-ostree inside Docker/Kubernetes/OpenShift;
           * in this case we probably don't have access to FUSE as today it uses a
           * suid binary which doesn't have the capabilities it needs.
           *
           * So this magical bit tells the core to disable FUSE, which we only do
           * if --cachedir isn't specified.  Another way to say this is that
           * running inside an unprivileged container today requires turning off
           * some of the rpm-ostree intelligence around caching.
           *
           * We don't make this actually conditional somehow on running in a
           * container since if you're not using a persistent cache there's no
           * real advantage to taking the overhead of FUSE. If the hardlinks are
           * corrupted, it doesn't matter as they're going to be deleted
           * anyways.
           */
          rpmostree_context_disable_rofiles (self->corectx);
        }
      else
        {
          self->unified_core_and_fuse = TRUE;
          /* We also only enable the devino cache if we know we have the FUSE protection
           * against mutation of the underlying files.
           */
          self->devino_cache = ostree_repo_devino_cache_new ();
          rpmostree_context_set_devino_cache (self->corectx, self->devino_cache);
        }

      rpmostree_context_set_repos (self->corectx, self->build_repo, self->pkgcache_repo);
    }

  if (!rpmostree_context_prepare (self->corectx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (dnfctx);

  /* FIXME - just do a depsolve here before we compute download requirements */
  g_autofree char *ret_new_inputhash = NULL;
  if (!rpmostree_composeutil_checksum (dnf_context_get_goal (dnfctx),
                                       self->treefile_rs, self->treefile,
                                       &ret_new_inputhash, error))
    return FALSE;

  g_print ("Input state hash: %s\n", ret_new_inputhash);

  /* Only look for previous checksum if caller has passed *out_unmodified */
  if (self->previous_checksum && out_unmodified != NULL)
    {
      g_autofree char *previous_inputhash = NULL;
      if (!inputhash_from_commit (self->repo, self->previous_checksum,
                                  &previous_inputhash, error))
        return FALSE;

      if (previous_inputhash)
        {
          if (strcmp (previous_inputhash, ret_new_inputhash) == 0)
            {
              *out_unmodified = TRUE;
              return TRUE; /* NB: early return */
            }
        }
      else
        g_print ("Previous commit found, but without rpmostree.inputhash metadata key\n");
    }

  if (opt_dry_run)
    return TRUE; /* NB: early return */

  if (!rpmostree_composeutil_sanity_checks (self->treefile_rs,
                                            self->treefile,
                                            cancellable, error))
    return FALSE;

  /* --- Downloading packages --- */
  if (!rpmostree_context_download (self->corectx, cancellable, error))
    return FALSE;

  if (opt_download_only)
    {
      if (opt_unified_core)
        {
          if (!rpmostree_context_import (self->corectx, cancellable, error))
            return FALSE;
        }
      return TRUE; /* 🔚 Early return */
    }

  /* Before we install packages, inject /etc/{passwd,group} if configured. */
  if (!rpmostree_passwd_compose_prep (rootfs_dfd, opt_unified_core, self->treefile_rs,
                                      self->treefile, self->previous_root,
                                      cancellable, error))
    return FALSE;

  if (opt_unified_core)
    {
      if (!rpmostree_context_import (self->corectx, cancellable, error))
        return FALSE;
      rpmostree_context_set_tmprootfs_dfd (self->corectx, rootfs_dfd);
      if (!rpmostree_context_assemble (self->corectx, cancellable, error))
        return FALSE;

      /* Now reload the policy from the tmproot, and relabel the pkgcache - this
       * is the same thing done in rpmostree_context_commit(). But here we want
       * to ensure our pkgcache labels are accurate, since that will
       * be important for the ostree-rojig work.
       */
      g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
      if (sepolicy == NULL)
        return FALSE;

      rpmostree_context_set_sepolicy (self->corectx, sepolicy);

      if (!rpmostree_context_force_relabel (self->corectx, cancellable, error))
        return FALSE;
    }
  else
    {
      /* The non-unified core path */

      /* Before we install packages, drop a file to suppress the kernel.rpm dracut run.
       * <https://github.com/systemd/systemd/pull/4174> */
      const char *kernel_installd_path = "usr/lib/kernel/install.d";
      if (!glnx_shutil_mkdir_p_at (rootfs_dfd, kernel_installd_path, 0755, cancellable, error))
        return FALSE;
      const char skip_kernel_install_data[] = "#!/usr/bin/sh\nexit 77\n";
      const char *kernel_skip_path = glnx_strjoina (kernel_installd_path, "/00-rpmostree-skip.install");
      if (!glnx_file_replace_contents_with_perms_at (rootfs_dfd, kernel_skip_path,
                                                     (guint8*)skip_kernel_install_data,
                                                     strlen (skip_kernel_install_data),
                                                     0755, 0, 0,
                                                     GLNX_FILE_REPLACE_NODATASYNC,
                                                     cancellable, error))
        return FALSE;

      /* Now actually run through librpm to install the packages.  Note this bit
       * will be replaced in the future with a unified core:
       * https://github.com/projectatomic/rpm-ostree/issues/729
       */
      g_auto(GLnxConsoleRef) console = { 0, };
      g_autoptr(DnfState) hifstate = dnf_state_new ();

      guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                               G_CALLBACK (on_hifstate_percentage_changed),
                                               "Installing packages:");

      glnx_console_lock (&console);

      if (!rpmostree_composeutil_legacy_prep_dev (rootfs_dfd, error))
        return FALSE;

      if (!dnf_transaction_commit (dnf_context_get_transaction (dnfctx),
                                   dnf_context_get_goal (dnfctx),
                                   hifstate, error))
        return FALSE;

      g_signal_handler_disconnect (hifstate, progress_sigid);
    }

  if (out_unmodified)
    *out_unmodified = FALSE;
  *out_new_inputhash = g_steal_pointer (&ret_new_inputhash);
  return TRUE;
}

static gboolean
parse_treefile_to_json (const char    *treefile_path,
                        int            workdir_dfd,
                        const char    *arch,
                        RORTreefile  **out_treefile_rs,
                        JsonParser   **out_parser,
                        GError       **error)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(RORTreefile) treefile_rs = ror_treefile_new (treefile_path, arch, workdir_dfd, error);
  if (!treefile_rs)
    return glnx_prefix_error (error, "Failed to load YAML treefile");

  const char *serialized = ror_treefile_get_json_string (treefile_rs);
  if (!json_parser_load_from_data (parser, serialized, -1, error))
    return FALSE;

  *out_parser = g_steal_pointer (&parser);
  *out_treefile_rs = g_steal_pointer (&treefile_rs);
  return TRUE;
}

static gboolean
parse_metadata_keyvalue_strings (char             **strings,
                                 GHashTable        *metadata_hash,
                                 GError           **error)
{
  for (char **iter = strings; *iter; iter++)
    {
      const char *s = *iter;
      const char *eq = strchr (s, '=');
      if (!eq)
        return glnx_throw (error, "Missing '=' in KEY=VALUE metadata '%s'", s);

      g_hash_table_insert (metadata_hash, g_strndup (s, eq - s),
                           g_variant_ref_sink (g_variant_new_string (eq + 1)));
    }

  return TRUE;
}

static gboolean
process_touch_if_changed (GError **error)
{

  if (!opt_touch_if_changed)
    return TRUE;

  glnx_autofd int fd = open (opt_touch_if_changed, O_CREAT|O_WRONLY|O_NOCTTY, 0644);
  if (fd == -1)
    return glnx_throw_errno_prefix (error, "Updating '%s'", opt_touch_if_changed);
  if (futimens (fd, NULL) == -1)
    return glnx_throw_errno_prefix (error, "futimens");

  return TRUE;
}

/* Prepare a context - this does some generic pre-compose initialization from
 * the arguments such as loading the treefile and any specified metadata.
 */
static gboolean
rpm_ostree_compose_context_new (const char    *treefile_pathstr,
                                RpmOstreeTreeComposeContext **out_context,
                                GCancellable  *cancellable,
                                GError       **error)
{
  g_autoptr(RpmOstreeTreeComposeContext) self = g_new0 (RpmOstreeTreeComposeContext, 1);

  /* Init fds to -1 */
  self->workdir_dfd = self->rootfs_dfd = self->cachedir_dfd = -1;
  /* Test whether or not bwrap is going to work - we will fail inside e.g. a Docker
   * container without --privileged or userns exposed.
   */
  if (!rpmostree_bwrap_selftest (error))
    return FALSE;

  self->repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!self->repo)
    return FALSE;

  /* sanity check upfront we can even write to this repo; e.g. might be a mount */
  if (!ostree_repo_is_writable (self->repo, error))
    return glnx_prefix_error (error, "Cannot write to repository");

  if (opt_workdir_tmpfs)
    g_printerr ("note: --workdir-tmpfs is deprecated and will be ignored\n");

  if (opt_unified_core)
    {
      /* Unified mode works very differently. We ignore --workdir because we want to be sure
       * that we're going to get hardlinks. The only way to be sure of this is to place the
       * workdir underneath the cachedir; the same fs where the pkgcache repo is. */

      if (opt_workdir)
        g_printerr ("note: --workdir is ignored for --ex-unified-core\n");

      if (opt_cachedir)
        {
          if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
            return glnx_prefix_error (error, "Opening cachedir");

          /* Put workdir beneath cachedir, which is where the pkgcache repo also is */
          if (!glnx_mkdtempat (self->cachedir_dfd, "rpm-ostree-compose.XXXXXX", 0700,
                               &self->workdir_tmp, error))
            return FALSE;
        }
      else
        {
          /* Put cachedir under the target repo: makes things more efficient if it's
           * bare-user, and otherwise just restricts IO to within the same fs. If for
           * whatever reason users don't want to run the compose there (e.g. weird
           * filesystems that aren't fully POSIX compliant), they can just use --cachedir.
           */
          if (!glnx_mkdtempat (ostree_repo_get_dfd (self->repo),
                               "tmp/rpm-ostree-compose.XXXXXX", 0700,
                               &self->workdir_tmp, error))
            return FALSE;

          self->cachedir_dfd = self->workdir_tmp.fd;
        }

      self->pkgcache_repo = ostree_repo_create_at (self->cachedir_dfd, "pkgcache-repo",
                                                   OSTREE_REPO_MODE_BARE_USER, NULL,
                                                   cancellable, error);
      if (!self->pkgcache_repo)
        return FALSE;

      /* We use a temporary repo for building and committing on the same FS as the
       * pkgcache to guarantee links and devino caching. We then pull-local into the "real"
       * target repo. */
      self->build_repo = ostree_repo_create_at (self->cachedir_dfd, "repo-build",
                                          OSTREE_REPO_MODE_BARE_USER, NULL,
                                          cancellable, error);
      if (!self->build_repo)
        return glnx_prefix_error (error, "Creating repo-build");

      /* Note special handling of this aliasing in _finalize() */
      self->workdir_dfd = self->workdir_tmp.fd;
    }
  else
    {
      if (!opt_workdir)
        {
          if (!glnx_mkdtempat (AT_FDCWD, "/var/tmp/rpm-ostree.XXXXXX", 0700, &self->workdir_tmp, error))
            return FALSE;
          /* Note special handling of this aliasing in _finalize() */
          self->workdir_dfd = self->workdir_tmp.fd;
        }
      else
        {
          if (!glnx_opendirat (AT_FDCWD, opt_workdir, FALSE, &self->workdir_dfd, error))
            return FALSE;
        }

      if (opt_cachedir)
        {
          if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
            return glnx_prefix_error (error, "Opening cachedir");
        }
      else
        {
          self->cachedir_dfd = fcntl (self->workdir_dfd, F_DUPFD_CLOEXEC, 3);
          if (self->cachedir_dfd < 0)
            return glnx_throw_errno_prefix (error, "fcntl");
        }

      self->build_repo = g_object_ref (self->repo);
    }

  self->treefile_path = g_file_new_for_path (treefile_pathstr);

  self->metadata = rpmostree_composeutil_read_json_metadata (opt_metadata_json, error);
  if (!self->metadata)
    return FALSE;

  if (opt_metadata_strings)
    {
      if (!parse_metadata_keyvalue_strings (opt_metadata_strings, self->metadata, error))
        return FALSE;
    }

  self->corectx = rpmostree_context_new_tree (self->cachedir_dfd, self->build_repo,
                                              cancellable, error);
  if (!self->corectx)
    return FALSE;

  const char *arch = dnf_context_get_base_arch (rpmostree_context_get_dnf (self->corectx));
  if (!parse_treefile_to_json (gs_file_get_path_cached (self->treefile_path),
                               self->workdir_dfd, arch,
                               &self->treefile_rs, &self->treefile_parser,
                               error))
    return FALSE;

  self->treefile_rootval = json_parser_get_root (self->treefile_parser);
  if (!JSON_NODE_HOLDS_OBJECT (self->treefile_rootval))
    return glnx_throw (error, "Treefile root is not an object");
  self->treefile = json_node_get_object (self->treefile_rootval);
  self->treespec = rpmostree_composeutil_get_treespec (self->corectx,
                                                       self->treefile_rs,
                                                       self->treefile,
                                                       opt_unified_core,
                                                       error);
  if (!self->treespec)
    return FALSE;
  self->ref = rpmostree_treespec_get_ref (self->treespec);

  *out_context = g_steal_pointer (&self);
  return TRUE;
}

static gboolean
impl_install_tree (RpmOstreeTreeComposeContext *self,
                   gboolean        *out_changed,
                   GCancellable    *cancellable,
                   GError         **error)
{
  if (opt_print_only)
    {
      g_print ("%s\n", ror_treefile_get_json_string (self->treefile_rs));
      return TRUE; /* Note early return */
    }

  /* Print version number */
  g_printerr ("RPM-OSTree Version: %s\n", PACKAGE_VERSION);

  /* Without specifying --cachedir we'd just toss the data we download, so let's
   * catch that.
   */
  if (opt_download_only && !opt_unified_core && !opt_cachedir)
    return glnx_throw (error, "--download-only can only be used with --cachedir");

  if (getuid () != 0)
    {
      if (!opt_unified_core)
        return glnx_throw (error, "This command requires root privileges");
      g_printerr ("NOTICE: Running this command as non-root is currently known not to work completely.\n");
      g_printerr ("NOTICE: Proceeding anyways.\n");
    }

  /* This fchdir() call is...old, dates back to when rpm-ostree wrapped
   * running yum as a subprocess.  It shouldn't be necessary any more,
   * but let's be conservative and not do it in unified core mode.
   */
  if (!opt_unified_core)
    {
      if (fchdir (self->workdir_dfd) != 0)
        return glnx_throw_errno_prefix (error, "fchdir");
    }

  /* Set this early here, so we only have to set it one more time in the
   * complete exit path too.
   */
  *out_changed = FALSE;

  /* Read the previous commit */
  if (self->ref)
    {
      g_autoptr(GError) temp_error = NULL;
      if (!ostree_repo_read_commit (self->repo, self->ref, &self->previous_root, &self->previous_checksum,
                                    cancellable, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_print ("No previous commit for %s\n", self->ref);
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&temp_error));
              return FALSE;
            }
        }
      else
        g_print ("Previous commit: %s\n", self->previous_checksum);
    }

  const char rootfs_name[] = "rootfs.tmp";
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
    return FALSE;
  if (mkdirat (self->workdir_dfd, rootfs_name, 0755) < 0)
    return glnx_throw_errno_prefix (error, "mkdirat(%s)", rootfs_name);

  if (!glnx_opendirat (self->workdir_dfd, rootfs_name, TRUE,
                       &self->rootfs_dfd, error))
    return FALSE;

  g_autofree char *next_version = NULL;
  if (json_object_has_member (self->treefile, "automatic_version_prefix") &&
      /* let --add-metadata-string=version=... take precedence */
      !g_hash_table_contains (self->metadata, OSTREE_COMMIT_META_KEY_VERSION))
    {
      const char *ver_prefix =
        _rpmostree_jsonutil_object_require_string_member (self->treefile, "automatic_version_prefix", error);
      if (!ver_prefix)
        return FALSE;

      g_autofree char *last_version = NULL;
      if (self->previous_checksum)
        {
          g_autoptr(GVariant) previous_commit = NULL;
          if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                         self->previous_checksum, &previous_commit, error))
            return FALSE;

          g_autoptr(GVariant) previous_metadata = g_variant_get_child_value (previous_commit, 0);
          (void)g_variant_lookup (previous_metadata, OSTREE_COMMIT_META_KEY_VERSION, "s", &last_version);
        }

      next_version = _rpmostree_util_next_version (ver_prefix, last_version);
      g_hash_table_insert (self->metadata, g_strdup (OSTREE_COMMIT_META_KEY_VERSION),
                           g_variant_ref_sink (g_variant_new_string (next_version)));
    }
  else
    {
      GVariant *v = g_hash_table_lookup (self->metadata, OSTREE_COMMIT_META_KEY_VERSION);
      if (v)
        {
          g_assert (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING));
          next_version = g_variant_dup_string (v, NULL);
        }
    }

  /* Download rpm-md repos, packages, do install */
  g_autofree char *new_inputhash = NULL;
  { gboolean unmodified = FALSE;

    if (!install_packages (self, opt_force_nocache ? NULL : &unmodified,
                           &new_inputhash, cancellable, error))
      return FALSE;

    gboolean is_dry_run = opt_dry_run || opt_download_only;
    if (unmodified)
      {
        const char *force_nocache_msg = "; use --force-nocache to override";
        g_print ("No apparent changes since previous commit%s\n",
                 is_dry_run ? "." : force_nocache_msg);
        /* Note early return */
        return TRUE;
      }
    else if (is_dry_run)
      {
        g_print ("--dry-run complete");
        if (opt_touch_if_changed)
          g_print (", updating --touch-if-changed=%s", opt_touch_if_changed);
        g_print ("; exiting\n");
        if (!process_touch_if_changed (error))
          return FALSE;
        /* Note early return */
        return TRUE;
      }
  }

  /* Bind metadata from the libdnf context */
  if (!g_hash_table_contains (self->metadata, "rpmostree.rpmmd-repos"))
    {
      g_hash_table_insert (self->metadata, g_strdup ("rpmostree.rpmmd-repos"),
                           rpmostree_context_get_rpmmd_repo_commit_metadata (self->corectx));
    }

  /* Destroy this now so the libdnf stack won't have any references
   * into the filesystem before we manipulate it.
   */
  g_clear_object (&self->corectx);

  if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
    return FALSE;

  /* Start postprocessing */
  if (!rpmostree_treefile_postprocessing (self->rootfs_dfd, self->treefile_rs, self->treefile,
                                          next_version, self->unified_core_and_fuse,
                                          cancellable, error))
    return glnx_prefix_error (error, "Postprocessing");

  /* Until here, we targeted "rootfs.tmp" in the working directory. Most
   * user-configured postprocessing has run. Now, we need to perform required
   * conversions like handling /boot. We generate a new directory "rootfs" that
   * has just what we want using "rootfs.tmp", as a source. This implicitly
   * discards anything else that happens to be in rootfs.tmp, like the `/dev`
   * nodes we create for example.
   */
  const char final_rootfs_name[] = "rootfs";
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, final_rootfs_name, cancellable, error))
    return FALSE;
  if (!glnx_ensure_dir (self->workdir_dfd, final_rootfs_name, 0755, error))
    return FALSE;
  { glnx_autofd int target_rootfs_dfd = -1;
    if (!glnx_opendirat (self->workdir_dfd, final_rootfs_name, TRUE,
                         &target_rootfs_dfd, error))
      return FALSE;

    if (!rpmostree_prepare_rootfs_for_commit (self->rootfs_dfd, target_rootfs_dfd,
                                              self->treefile,
                                              cancellable, error))
      return glnx_prefix_error (error, "Preparing rootfs for commit");

    glnx_close_fd (&self->rootfs_dfd);

    /* Remove the old root, then retarget rootfs_dfd to the final one */
    if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
      return FALSE;

    self->rootfs_dfd = glnx_steal_fd (&target_rootfs_dfd);
  }

  /* Insert our input hash */
  g_hash_table_replace (self->metadata, g_strdup ("rpmostree.inputhash"),
                        g_variant_ref_sink (g_variant_new_string (new_inputhash)));

  *out_changed = TRUE;
  return TRUE;
}

/* https://pagure.io/atomic-wg/issue/387 */
static gboolean
repo_is_on_netfs (OstreeRepo  *repo)
{
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

  int dfd = ostree_repo_get_dfd (repo);
  struct statfs stbuf;
  if (fstatfs (dfd, &stbuf) != 0)
    return FALSE;
  switch (stbuf.f_type)
    {
    case NFS_SUPER_MAGIC:
    case FUSE_SUPER_MAGIC:
      return TRUE;
    default:
      return FALSE;
    }
}

/* See canonical version of this in ot-builtin-pull.c */
static void
noninteractive_console_progress_changed (OstreeAsyncProgress *progress,
                                         gpointer             user_data)
{
  /* We do nothing here - we just want the final status */
}

static gboolean
pull_local_into_target_repo (OstreeRepo   *src_repo,
                             OstreeRepo   *dest_repo,
                             const char   *checksum,
                             GCancellable *cancellable,
                             GError      **error)
{
  const char *refs[] = { checksum, NULL };

  /* really should enhance the pull API so we can just pass the src OstreeRepo directly */
  g_autofree char *src_repo_uri =
    g_strdup_printf ("file:///proc/self/fd/%d", ostree_repo_get_dfd (src_repo));

  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_console_lock (&console);
  g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new_and_connect (
      console.is_tty ? ostree_repo_pull_default_console_progress_changed
                     : noninteractive_console_progress_changed, &console);

  /* no fancy flags here, so just use the old school simpler API */
  if (!ostree_repo_pull (dest_repo, src_repo_uri, (char**)refs, 0, progress,
                         cancellable, error))
    return FALSE;

  if (!console.is_tty)
    {
      const char *status = ostree_async_progress_get_status (progress);
      if (status)
        g_print ("%s\n", status);
    }
  ostree_async_progress_finish (progress);

  return TRUE;
}

/* Perform required postprocessing, and invoke rpmostree_compose_commit(). */
static gboolean
impl_commit_tree (RpmOstreeTreeComposeContext *self,
                  GCancellable    *cancellable,
                  GError         **error)
{
  g_auto(GVariantBuilder) composemeta_builder;
  g_variant_builder_init (&composemeta_builder, G_VARIANT_TYPE ("a{sv}"));

  const char *gpgkey = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (self->treefile, "gpg_key", &gpgkey, error))
    return FALSE;

  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (self->treefile, "selinux", &selinux, error))
    return FALSE;

  /* Convert metadata hash to GVariant */
  g_autoptr(GVariant) metadata = rpmostree_composeutil_finalize_metadata (self->metadata, self->rootfs_dfd, error);
  if (!metadata)
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (self->rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (self->rootfs_dfd, self->treefile, self->unified_core_and_fuse,
                                    cancellable, error))
    return FALSE;

  if (self->treefile_rs)
    {
      if (!rpmostree_check_passwd (self->repo, self->rootfs_dfd, self->treefile_rs,
                                   self->treefile, self->previous_checksum,
                                   cancellable, error))
        return glnx_prefix_error (error, "Handling passwd db");

      if (!rpmostree_check_groups (self->repo, self->rootfs_dfd, self->treefile_rs,
                                   self->treefile, self->previous_checksum,
                                   cancellable, error))
        return glnx_prefix_error (error, "Handling group db");
    }

  /* See comment above */
  const gboolean txn_explicitly_disabled = (getenv ("RPMOSTREE_COMMIT_NO_TXN") != NULL);
  const gboolean using_netfs = repo_is_on_netfs (self->repo);
  if (txn_explicitly_disabled)
    g_print ("libostree transactions explicitly disabled\n");
  else if (using_netfs)
    g_print ("Network filesystem detected for repo; disabling transaction\n");
  const gboolean use_txn = !(txn_explicitly_disabled || using_netfs);

  if (use_txn)
    {
      if (!ostree_repo_prepare_transaction (self->build_repo, NULL, cancellable, error))
        return FALSE;
    }

  g_autofree char *parent_revision = NULL;
  if (self->ref)
    {
      if (!ostree_repo_resolve_rev (self->repo, self->ref, TRUE, &parent_revision, error))
        return FALSE;
    }

  /* The penultimate step, just basically `ostree commit` */
  g_autofree char *new_revision = NULL;
  if (!rpmostree_compose_commit (self->rootfs_dfd, self->build_repo, parent_revision,
                                 metadata, gpgkey, selinux, self->devino_cache,
                                 &new_revision, cancellable, error))
    return FALSE;

  OstreeRepoTransactionStats stats = { 0, };
  OstreeRepoTransactionStats *statsp = NULL;

  if (use_txn)
    {
      if (!ostree_repo_commit_transaction (self->build_repo, &stats, cancellable, error))
        return glnx_prefix_error (error, "Commit");
      statsp = &stats;
    }

  if (!opt_unified_core)
    g_assert (self->repo == self->build_repo);
  else
    {
      /* Now we actually pull it into the target repo specified by the user */
      g_assert (self->repo != self->build_repo);

      if (!pull_local_into_target_repo (self->build_repo, self->repo, new_revision,
                                        cancellable, error))
        return FALSE;
    }

  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (self->repo, new_revision, &new_commit, NULL, error))
    return FALSE;
  g_autoptr(GVariant) new_commit_inline_meta = g_variant_get_child_value (new_commit, 0);

  /* --write-commitid-to overrides writing the ref */
  if (self->ref && !opt_write_commitid_to)
    {
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, self->ref, new_revision,
                                          cancellable, error))
        return FALSE;
      g_print ("%s => %s\n", self->ref, new_revision);
      g_variant_builder_add (&composemeta_builder, "{sv}", "ref", g_variant_new_string (self->ref));
    }
  else
    g_print ("Wrote commit: %s\n", new_revision);

  if (!rpmostree_composeutil_write_composejson (self->repo,
                                                opt_write_composejson_to, statsp,
                                                new_revision, new_commit,
                                                &composemeta_builder,
                                                error))
    return FALSE;

  if (opt_write_commitid_to)
    {
      if (!g_file_set_contents (opt_write_commitid_to, new_revision, -1, error))
        return glnx_prefix_error (error, "While writing to '%s'", opt_write_commitid_to);
    }

  return TRUE;
}

gboolean
rpmostree_compose_builtin_install (int             argc,
                                   char          **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE DESTDIR");
  g_option_context_add_main_entries (context, common_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       install_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc != 3)
   {
      rpmostree_usage_error (context, "TREEFILE and DESTDIR required", error);
      return FALSE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  if (opt_workdir)
    {
      rpmostree_usage_error (context, "--workdir is ignored with install-root", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];
  /* Destination is turned into workdir */
  const char *destdir = argv[2];
  opt_workdir = g_strdup (destdir);

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  gboolean changed;
  if (!impl_install_tree (self, &changed, cancellable, error))
    return FALSE;
  g_print ("rootfs: %s/rootfs\n", destdir);

  return TRUE;
}

gboolean
rpmostree_compose_builtin_postprocess (int             argc,
                                       char          **argv,
                                       RpmOstreeCommandInvocation *invocation,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("postprocess ROOTFS [TREEFILE]");
  if (!rpmostree_option_context_parse (context,
                                       postprocess_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2 || argc > 3)
    {
      rpmostree_usage_error (context, "ROOTFS must be specified", error);
      return FALSE;
    }

  const char *rootfs_path = argv[1];
  /* Here we *optionally* process a treefile; some things like `tmp-is-dir` and
   * `boot_location` are configurable and relevant here, but a lot of users
   * will also probably be OK with the defaults, and part of the idea here is
   * to avoid at least some of the use cases requiring a treefile.
   */
  const char *treefile_path = argc > 2 ? argv[2] : NULL;
  glnx_unref_object JsonParser *treefile_parser = NULL;
  JsonObject *treefile = NULL; /* Owned by parser */
  g_autoptr(RORTreefile) treefile_rs = NULL;
  g_auto(GLnxTmpDir) workdir_tmp = { 0, };
  if (treefile_path)
    {
      if (!glnx_mkdtempat (AT_FDCWD, "/var/tmp/rpm-ostree.XXXXXX", 0700, &workdir_tmp, error))
        return FALSE;
      if (!parse_treefile_to_json (treefile_path, workdir_tmp.fd, NULL,
                                   &treefile_rs, &treefile_parser, error))
        return FALSE;

      JsonNode *treefile_rootval = json_parser_get_root (treefile_parser);
      if (!JSON_NODE_HOLDS_OBJECT (treefile_rootval))
        return glnx_throw (error, "Treefile root is not an object");
      treefile = json_node_get_object (treefile_rootval);
    }

  glnx_fd_close int rootfs_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, rootfs_path, TRUE, &rootfs_dfd, error))
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (rootfs_dfd, treefile, opt_unified_core,
                                    cancellable, error))
    return FALSE;
  return TRUE;
}

gboolean
rpmostree_compose_builtin_commit (int             argc,
                                  char          **argv,
                                  RpmOstreeCommandInvocation *invocation,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE ROOTFS");
  g_option_context_add_main_entries (context, common_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "TREEFILE must be specified", error);
      return FALSE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];
  const char *rootfs_path = argv[2];

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  if (!glnx_opendirat (AT_FDCWD, rootfs_path, TRUE, &self->rootfs_dfd, error))
    return FALSE;
  if (!impl_commit_tree (self, cancellable, error))
     return FALSE;
  return TRUE;
}

gboolean
rpmostree_compose_builtin_tree (int             argc,
                                char          **argv,
                                RpmOstreeCommandInvocation *invocation,
                                GCancellable   *cancellable,
                                GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE");
  g_option_context_add_main_entries (context, common_option_entries, NULL);
  g_option_context_add_main_entries (context, install_option_entries, NULL);
  g_option_context_add_main_entries (context, postprocess_option_entries, NULL);

  if (!rpmostree_option_context_parse (context,
                                       commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 2)
    {
      rpmostree_usage_error (context, "TREEFILE must be specified", error);
      return FALSE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];

  g_autoptr(RpmOstreeTreeComposeContext) self = NULL;
  if (!rpm_ostree_compose_context_new (treefile_path, &self, cancellable, error))
    return FALSE;
  gboolean changed;
  if (!impl_install_tree (self, &changed, cancellable, error))
    return FALSE;
  if (changed)
    {
      /* Do the ostree commit */
      if (!impl_commit_tree (self, cancellable, error))
        return FALSE;
      /* Finally process the --touch-if-changed option  */
      if (!process_touch_if_changed (error))
        return FALSE;
    }


  return TRUE;
}
