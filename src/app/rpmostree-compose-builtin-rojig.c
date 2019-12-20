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
#include "rpmostree-bwrap.h"
#include "rpmostree-core.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-rojig-build.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-package-variants.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-composeutil.h"
#include "rpmostree-rust.h"

#include "libglnx.h"

static gboolean opt_force_commit;
static gboolean opt_cache_only;
static char *opt_cachedir;
static gboolean opt_download_only;
static gboolean opt_dry_run;
static char *opt_metadata_json;
static char *opt_write_composejson_to;

static GOptionEntry rojig_option_entries[] = {
  { "force-commit", 0, 0, G_OPTION_ARG_NONE, &opt_force_commit, "Always create a new rojig RPM, even if nothing appears to have changed", NULL },
  { "cache-only", 0, 0, G_OPTION_ARG_NONE, &opt_cache_only, "Assume cache is present, do not attempt to update it", NULL },
  { "cachedir", 0, 0, G_OPTION_ARG_STRING, &opt_cachedir, "Cached state", "CACHEDIR" },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Like --dry-run, but download RPMs as well; requires --cachedir", NULL },
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Just print the transaction and exit", NULL },
  { "add-metadata-from-json", 0, 0, G_OPTION_ARG_STRING, &opt_metadata_json, "Parse the given JSON file as object, convert to GVariant, append to OSTree commit", "JSON" },
  { "write-composejson-to", 0, 0, G_OPTION_ARG_STRING, &opt_write_composejson_to, "Write JSON to FILE containing information about the compose run", "FILE" },
  { NULL }
};

typedef struct {
  RpmOstreeContext *corectx;
  GHashTable *metadata;
  GLnxTmpDir workdir_tmp;
  int rootfs_dfd;
  int workdir_dfd; /* Note: may be an alias for workdir_tmp */
  int cachedir_dfd; /* Note: may be an alias for workdir_tmp */
  OstreeRepo *repo;
  OstreeRepo *pkgcache_repo;
  OstreeRepoDevInoCache *devino_cache;
  char *rojig_spec;
  char *previous_version;
  char *previous_inputhash;

  RORTreefile *treefile_rs;
  JsonParser *treefile_parser;
  JsonNode *treefile_rootval; /* Unowned */
  JsonObject *treefile; /* Unowned */
  RpmOstreeTreespec *treespec;
} RpmOstreeRojigCompose;

static void
rpm_ostree_rojig_compose_free (RpmOstreeRojigCompose *ctx)
{
  g_clear_object (&ctx->repo);
  g_clear_object (&ctx->pkgcache_repo);
  g_clear_object (&ctx->corectx);
  g_clear_pointer (&ctx->metadata, g_hash_table_unref);
  glnx_close_fd (&ctx->rootfs_dfd);
  if (ctx->workdir_dfd != ctx->workdir_tmp.fd)
    glnx_close_fd (&ctx->workdir_dfd);
  if (ctx->cachedir_dfd != ctx->workdir_tmp.fd)
    glnx_close_fd (&ctx->cachedir_dfd);
  if (g_getenv ("RPMOSTREE_PRESERVE_TMPDIR"))
    g_printerr ("Preserved workdir: %s\n", ctx->workdir_tmp.path);
  else
    (void)glnx_tmpdir_delete (&ctx->workdir_tmp, NULL, NULL);
  g_clear_pointer (&ctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);
  g_free (ctx->rojig_spec);
  g_free (ctx->previous_version);
  g_free (ctx->previous_inputhash);
  g_clear_pointer (&ctx->treefile_rs, (GDestroyNotify) ror_treefile_free);
  g_clear_object (&ctx->treefile_parser);
  g_clear_object (&ctx->treespec);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeRojigCompose, rpm_ostree_rojig_compose_free)

static gboolean
install_packages (RpmOstreeRojigCompose  *self,
                  gboolean        *out_unmodified,
                  char           **out_new_inputhash,
                  GCancellable    *cancellable,
                  GError         **error)
{
  DnfContext *dnfctx = rpmostree_context_get_dnf (self->corectx);

  { int tf_dfd = ror_treefile_get_dfd (self->treefile_rs);
    g_autofree char *abs_tf_path = glnx_fdrel_abspath (tf_dfd, ".");
    dnf_context_set_repo_dir (dnfctx, abs_tf_path);
  }

  /* For compose, always try to refresh metadata; we're used in build servers
   * where fetching should be cheap. Otherwise, if --cache-only is set, it's
   * likely an offline developer laptop case, so never refresh.
   */
  if (!opt_cache_only)
    dnf_context_set_cache_age (dnfctx, 0);
  else
    dnf_context_set_cache_age (dnfctx, G_MAXUINT);

  { g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (self->rootfs_dfd, ".");
    if (!rpmostree_context_setup (self->corectx, tmprootfs_abspath, NULL, self->treespec,
                                  cancellable, error))
      return FALSE;
  }

  /* For unified core, we have a pkgcache repo. This may be auto-created under
   * the workdir, or live explicitly in the dir for --cache.
   */
  self->pkgcache_repo = ostree_repo_create_at (self->cachedir_dfd, "pkgcache-repo",
                                               OSTREE_REPO_MODE_BARE_USER, NULL,
                                               cancellable, error);
  if (!self->pkgcache_repo)
    return FALSE;
  rpmostree_context_set_repos (self->corectx, self->repo, self->pkgcache_repo);
  self->devino_cache = ostree_repo_devino_cache_new ();
  rpmostree_context_set_devino_cache (self->corectx, self->devino_cache);

  if (!rpmostree_context_prepare (self->corectx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (dnfctx);

  g_autofree char *ret_new_inputhash = NULL;
  if (!rpmostree_composeutil_checksum (dnf_context_get_goal (dnfctx),
                                       self->repo,
                                       self->treefile_rs, self->treefile,
                                       &ret_new_inputhash, error))
    return FALSE;

  g_print ("Input state hash: %s\n", ret_new_inputhash);

  /* Only look for previous checksum if caller has passed *out_unmodified */
  if (self->previous_inputhash && out_unmodified != NULL)
    {
      if (strcmp (self->previous_inputhash, ret_new_inputhash) == 0)
        {
          *out_unmodified = TRUE;
          return TRUE; /* NB: early return */
        }
    }

  if (opt_dry_run)
    return TRUE; /* NB: early return */

  if (!rpmostree_composeutil_sanity_checks (self->treefile_rs, self->treefile,
                                            cancellable, error))
    return FALSE;

  /* --- Downloading packages --- */
  if (!rpmostree_context_download (self->corectx, cancellable, error))
    return FALSE;
  if (!rpmostree_context_import (self->corectx, cancellable, error))
    return FALSE;

  if (opt_download_only)
    return TRUE; /* 🔚 Early return */

  if (!rpmostree_passwd_compose_prep (self->rootfs_dfd, NULL, TRUE, self->treefile_rs,
                                      self->treefile, NULL, cancellable, error))
    return FALSE;

 rpmostree_context_set_tmprootfs_dfd (self->corectx, self->rootfs_dfd);
  if (!rpmostree_context_assemble (self->corectx, cancellable, error))
    return FALSE;

  /* Now reload the policy from the tmproot, and relabel the pkgcache - this
   * is the same thing done in rpmostree_context_commit(). But here we want
   * to ensure our pkgcache labels are accurate, since that will
   * be important for the ostree-rojig work.
   */
  { g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (self->rootfs_dfd, cancellable, error);
    rpmostree_context_set_sepolicy (self->corectx, sepolicy);

    if (!rpmostree_context_force_relabel (self->corectx, cancellable, error))
      return FALSE;
  }

  if (out_unmodified)
    *out_unmodified = FALSE;
  *out_new_inputhash = g_steal_pointer (&ret_new_inputhash);
  return TRUE;
}

/* Prepare a context - this does some generic pre-compose initialization from
 * the arguments such as loading the treefile and any specified metadata.
 */
static gboolean
rpm_ostree_rojig_compose_new (const char    *treefile_path,
                              RpmOstreeRojigCompose **out_context,
                              GCancellable  *cancellable,
                              GError       **error)
{
  g_autoptr(RpmOstreeRojigCompose) self = g_new0 (RpmOstreeRojigCompose, 1);

  /* Init fds to -1 */
  self->workdir_dfd = self->rootfs_dfd = self->cachedir_dfd = -1;
  /* Test whether or not bwrap is going to work - we will fail inside e.g. a Docker
   * container without --privileged or userns exposed.
   */
  if (!rpmostree_bwrap_selftest (error))
    return FALSE;

  if (opt_cachedir)
    {
      /* Put the workdir under the cachedir, so it's all on one filesystem;
       * this will let us do hardlinks.
       */
      if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
        return glnx_prefix_error (error, "Opening cachedir '%s'", opt_cachedir);
      if (!glnx_shutil_rm_rf_at (self->cachedir_dfd, "work", cancellable, error))
        return FALSE;
      if (!glnx_shutil_mkdir_p_at (self->cachedir_dfd, "work", 0755, cancellable, error))
        return FALSE;
      if (!glnx_opendirat (self->cachedir_dfd, "work", TRUE, &self->workdir_dfd, error))
        return FALSE;
    }
  else
    {
      /* No cache?  Then allocate a temporary workdir, and put the cachedir
       * under it.
       */
      g_autofree char *tmpdir = g_build_filename (g_getenv ("TMPDIR") ?: "/var/tmp", "rpm-ostree.XXXXXX", NULL);
      if (!glnx_mkdtempat (AT_FDCWD, tmpdir, 0700, &self->workdir_tmp, error))
        return FALSE;
      self->cachedir_dfd = self->workdir_dfd = self->workdir_tmp.fd;
    }

  /* In rojig mode, we have a temporary repo */
  self->repo = ostree_repo_create_at (self->workdir_dfd, "repo-build",
                                      OSTREE_REPO_MODE_BARE_USER, NULL,
                                      cancellable, error);
  if (!self->repo)
    return glnx_prefix_error (error, "Creating repo-build");

  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)g_variant_unref);
  if (opt_metadata_json)
    {
      if (!rpmostree_composeutil_read_json_metadata_from_file (opt_metadata_json,
                                                               self->metadata, error))
        return FALSE;
    }

  self->corectx = rpmostree_context_new_tree (self->cachedir_dfd, self->repo, cancellable, error);
  if (!self->corectx)
    return FALSE;

  const char *arch = dnf_context_get_base_arch (rpmostree_context_get_dnf (self->corectx));
  self->treefile_rs = ror_treefile_new (treefile_path, arch, self->workdir_dfd, error);
  if (!self->treefile_rs)
    return glnx_prefix_error (error, "Failed to load YAML treefile");

  self->treefile_parser = json_parser_new ();
  if (!json_parser_load_from_data (self->treefile_parser,
                                   ror_treefile_get_json_string (self->treefile_rs), -1,
                                   error))
    return FALSE;

  self->treefile_rootval = json_parser_get_root (self->treefile_parser);
  if (!JSON_NODE_HOLDS_OBJECT (self->treefile_rootval))
    return glnx_throw (error, "Treefile root is not an object");
  self->treefile = json_node_get_object (self->treefile_rootval);
  self->treespec = rpmostree_composeutil_get_treespec (self->corectx,
                                                       self->treefile_rs,
                                                       self->treefile,
                                                       TRUE,
                                                       error);

  *out_context = g_steal_pointer (&self);
  return TRUE;
}

static gboolean
impl_rojig_build (RpmOstreeRojigCompose *self,
                  const char            *outdir,
                  gboolean        *out_changed,
                  GCancellable    *cancellable,
                  GError         **error)
{
  *out_changed = FALSE;

  if (getuid () != 0)
    {
      g_printerr ("NOTICE: Running this command as non-root is currently known not to work completely.\n");
      g_printerr ("NOTICE: Proceeding anyways.\n");
      g_usleep (3 * G_USEC_PER_SEC);
    }

  const char *rojig_name = ror_treefile_get_rojig_name (self->treefile_rs);
  if (!rojig_name)
    return glnx_throw (error, "No `rojig` entry in manifest");
  g_autoptr(GKeyFile) tsk = g_key_file_new ();
  const char *rojig_output_repo_id = "rpmostree-rojig-output";
  g_autofree char *rojig_spec = g_strconcat (rojig_output_repo_id, ":", rojig_name, NULL);
  g_key_file_set_string (tsk, "tree", "rojig", rojig_spec);
  { const char *repos[] = { rojig_output_repo_id, };
    g_key_file_set_string_list (tsk, "tree", "repos", (const char*const*)repos, 1);
  }
  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_keyfile (tsk, error);
  if (!treespec)
    return FALSE;
  g_autoptr(RpmOstreeContext) corectx = rpmostree_context_new_tree (self->cachedir_dfd, self->repo, cancellable, error);
  if (!corectx)
    return FALSE;
  DnfContext *dnfctx = rpmostree_context_get_dnf (corectx);
  if (!glnx_shutil_mkdir_p_at (self->workdir_dfd, "rojig-repos", 0755, cancellable, error))
    return FALSE;
  { g_autofree char *repopath = g_strconcat ("rojig-repos/", rojig_output_repo_id, ".repo", NULL);
    g_autofree char *repo_contents = g_strdup_printf ("[%s]\n"
                                                      "baseurl=file://%s\n"
                                                      "gpgcheck=0\n",
                                                      rojig_output_repo_id,
                                                      outdir);
    if (!glnx_file_replace_contents_at (self->workdir_dfd, repopath,
                                        (guint8*)repo_contents, -1, 0,
                                        cancellable, error))
      return FALSE;
  }

  g_autofree char *reposdir_abspath = glnx_fdrel_abspath (self->workdir_dfd, "rojig-repos");
  dnf_context_set_repo_dir (dnfctx, reposdir_abspath);
  if (!rpmostree_context_setup (corectx, NULL, NULL, treespec, cancellable, error))
    return FALSE;
  if (!rpmostree_context_prepare_rojig (corectx, TRUE, cancellable, error))
    return FALSE;
  DnfPackage *rojig_pkg = rpmostree_context_get_rojig_pkg (corectx);
  if (rojig_pkg)
    {
      g_print ("Previous rojig: %s\n", dnf_package_get_nevra (rojig_pkg));
      self->previous_version = g_strdup (dnf_package_get_version (rojig_pkg));
      self->previous_inputhash = g_strdup (rpmostree_context_get_rojig_inputhash (corectx));
    }
  else
    {
      g_print ("No previous rojig package found: %s\n", rojig_name);
    }

  /* Set this early here, so we only have to set it one more time in the
   * complete exit path too.
   */

  const char rootfs_name[] = "rootfs.tmp";
  if (!glnx_shutil_rm_rf_at (self->workdir_dfd, rootfs_name, cancellable, error))
    return FALSE;
  if (mkdirat (self->workdir_dfd, rootfs_name, 0755) < 0)
    return glnx_throw_errno_prefix (error, "mkdirat(%s)", rootfs_name);

  if (!glnx_opendirat (self->workdir_dfd, rootfs_name, TRUE,
                       &self->rootfs_dfd, error))
    return FALSE;

  g_autofree char *next_version = NULL;
  if (json_object_has_member (self->treefile, "automatic-version-prefix") &&
      /* let --add-metadata-string=version=... take precedence */
      !g_hash_table_contains (self->metadata, OSTREE_COMMIT_META_KEY_VERSION))
    {
      const char *ver_prefix =
        _rpmostree_jsonutil_object_require_string_member (self->treefile, "automatic-version-prefix", error);
      if (!ver_prefix)
        return FALSE;
      const char *ver_suffix = NULL;
      if (!_rpmostree_jsonutil_object_get_optional_string_member (self->treefile, "automatic-version-suffix", &ver_suffix, error))
        return FALSE;

      next_version = _rpmostree_util_next_version (ver_prefix, ver_suffix, self->previous_version, error);
      if (!next_version)
        return FALSE;
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

    if (!install_packages (self, opt_force_commit ? NULL : &unmodified,
                           &new_inputhash, cancellable, error))
      return FALSE;

    gboolean is_dry_run = opt_dry_run || opt_download_only;
    if (unmodified)
      {
        const char *force_nocache_msg = "; use --force-commit to override";
        g_print ("No apparent changes since previous commit%s\n",
                 is_dry_run ? "." : force_nocache_msg);
        /* Note early return */
        return TRUE;
      }
    else if (is_dry_run)
      {
        g_print ("--dry-run complete");
        g_print ("; exiting\n");
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
                                          next_version, TRUE,
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

/* Perform required postprocessing, and invoke rpmostree_compose_commit(). */
static gboolean
impl_write_rojig (RpmOstreeRojigCompose *self,
                  const char            *outdir,
                  GCancellable          *cancellable,
                  GError               **error)
{
  g_auto(GVariantBuilder) composemeta_builder;
  g_variant_builder_init (&composemeta_builder, G_VARIANT_TYPE ("a{sv}"));

  gboolean selinux = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (self->treefile, "selinux", &selinux, error))
    return FALSE;

  /* Convert metadata hash to GVariant */
  g_autoptr(GVariant) metadata = rpmostree_composeutil_finalize_metadata (self->metadata, self->rootfs_dfd, error);
  if (!metadata)
    return FALSE;
  if (!rpmostree_rootfs_postprocess_common (self->rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (self->rootfs_dfd, self->treefile_rs, self->treefile, TRUE,
                                    cancellable, error))
    return FALSE;

  if (self->treefile)
    {
      if (!rpmostree_check_passwd (self->repo, self->rootfs_dfd, self->treefile_rs, self->treefile,
                                   NULL, cancellable, error))
        return glnx_prefix_error (error, "Handling passwd db");

      if (!rpmostree_check_groups (self->repo, self->rootfs_dfd, self->treefile_rs, self->treefile,
                                   NULL, cancellable, error))
        return glnx_prefix_error (error, "Handling group db");
    }

  if (!ostree_repo_prepare_transaction (self->repo, NULL, cancellable, error))
    return FALSE;

  /* The penultimate step, just basically `ostree commit` */
  g_autofree char *new_revision = NULL;
  if (!rpmostree_compose_commit (self->rootfs_dfd, self->repo, NULL,
                                 metadata, NULL, selinux, self->devino_cache,
                                 &new_revision, cancellable, error))
    return FALSE;

  if (!rpmostree_commit2rojig (self->repo, self->pkgcache_repo, new_revision,
                               self->workdir_dfd,
                               ror_treefile_get_rojig_spec_path (self->treefile_rs),
                               outdir, cancellable, error))
    return FALSE;

  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (self->repo, new_revision, &new_commit,
                                NULL, error))
    return FALSE;

  OstreeRepoTransactionStats stats = { 0, };
  if (!ostree_repo_commit_transaction (self->repo, &stats, cancellable, error))
    return glnx_prefix_error (error, "Commit");

  if (!rpmostree_composeutil_write_composejson (self->repo,
                                                opt_write_composejson_to, &stats,
                                                new_revision, new_commit, &composemeta_builder,
                                                cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
rpmostree_compose_builtin_rojig (int             argc,
                                 char          **argv,
                                 RpmOstreeCommandInvocation *invocation,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("TREEFILE OUTDIR");
  if (!rpmostree_option_context_parse (context,
                                       rojig_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc < 3)
    {
      rpmostree_usage_error (context, "TREEFILE and OUTDIR must be specified", error);
      return FALSE;
    }

  const char *treefile_path = argv[1];
  const char *outdir = argv[2];

  g_autoptr(RpmOstreeRojigCompose) self = NULL;
  if (!rpm_ostree_rojig_compose_new (treefile_path, &self, cancellable, error))
    return FALSE;
  g_assert (self); /* Pacify static analysis */
  gboolean changed;
  if (!impl_rojig_build (self, outdir, &changed, cancellable, error))
    return FALSE;
  if (changed)
    {
      /* Do the ostree commit, then generate rojig RPM */
      if (!impl_write_rojig (self, outdir, cancellable, error))
        return FALSE;
    }

  return TRUE;
}
