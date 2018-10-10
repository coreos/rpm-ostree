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
  OstreeRepo *repo;
  OstreeRepo *pkgcache_repo;
  OstreeRepoDevInoCache *devino_cache;
  char *ref;
  char *rojig_spec;
  char *previous_checksum;

  RORTreefile *treefile_rs;
  JsonParser *treefile_parser;
  JsonNode *treefile_rootval; /* Unowned */
  JsonObject *treefile; /* Unowned */

  GBytes *serialized_treefile;
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
  g_clear_object (&ctx->pkgcache_repo);
  g_clear_pointer (&ctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);
  g_free (ctx->ref);
  g_free (ctx->previous_checksum);
  g_clear_pointer (&ctx->treefile_rs, (GDestroyNotify) ror_treefile_free);
  g_clear_object (&ctx->treefile_parser);
  g_clear_pointer (&ctx->serialized_treefile, (GDestroyNotify)g_bytes_unref);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeTreeComposeContext, rpm_ostree_tree_compose_context_free)

static int
cachedir_dfd (RpmOstreeTreeComposeContext *self)
{
  return self->cachedir_dfd != -1 ? self->cachedir_dfd : self->workdir_dfd;
}

static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

static gboolean
set_keyfile_string_array_from_json (GKeyFile    *keyfile,
                                    const char  *keyfile_group,
                                    const char  *keyfile_key,
                                    JsonArray   *a,
                                    GError     **error)
{
  g_autoptr(GPtrArray) instlangs_v = g_ptr_array_new ();

  guint len = json_array_get_length (a);
  for (guint i = 0; i < len; i++)
    {
      const char *elt = _rpmostree_jsonutil_array_require_string_element (a, i, error);

      if (!elt)
        return FALSE;

      g_ptr_array_add (instlangs_v, (char*)elt);
    }

  g_key_file_set_string_list (keyfile, keyfile_group, keyfile_key,
                              (const char*const*)instlangs_v->pdata, instlangs_v->len);

  return TRUE;
}

/* Given a boolean value in JSON, add it to treespec
 * if it's not the default.
 */
static gboolean
treespec_bind_bool (JsonObject *treedata,
                    GKeyFile   *ts,
                    const char *name,
                    gboolean    default_value,
                    GError    **error)
{
  gboolean v = default_value;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata, name, &v, error))
    return FALSE;

  if (v != default_value)
    g_key_file_set_boolean (ts, "tree", name, v);

  return TRUE;
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
install_packages_in_root (RpmOstreeTreeComposeContext  *self,
                          JsonObject      *treedata,
                          int              rootfs_dfd,
                          char           **packages,
                          gboolean        *out_unmodified,
                          char           **out_new_inputhash,
                          GCancellable    *cancellable,
                          GError         **error)
{
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
   * where fetching should be cheap. Otherwise, if --cache-only is set, it's
   * likely an offline developer laptop case, so never refresh.
   */
  if (!opt_cache_only)
    dnf_context_set_cache_age (dnfctx, 0);
  else
    dnf_context_set_cache_age (dnfctx, G_MAXUINT);
  /* Without specifying --cachedir we'd just toss the data we download, so let's
   * catch that.
   */
  if (opt_download_only && !opt_unified_core && !opt_cachedir)
    return glnx_throw (error, "--download-only can only be used with --cachedir");

  g_autoptr(GKeyFile) treespec = g_key_file_new ();
  if (self->ref)
    g_key_file_set_string (treespec, "tree", "ref", self->ref);
  g_key_file_set_string_list (treespec, "tree", "packages", (const char *const*)packages, g_strv_length (packages));
  { const char *releasever;
    if (!_rpmostree_jsonutil_object_get_optional_string_member (treedata, "releasever",
                                                                &releasever, error))
      return FALSE;
    if (releasever)
      g_key_file_set_string (treespec, "tree", "releasever", releasever);
  }

  /* Some awful code to translate between JSON and GKeyFile */
  if (json_object_has_member (treedata, "install-langs"))
    {
      JsonArray *a = json_object_get_array_member (treedata, "install-langs");
      if (!set_keyfile_string_array_from_json (treespec, "tree", "instlangs", a, error))
        return FALSE;
    }

  /* Bind the json \"repos\" member to the hif state, which looks at the
   * enabled= member of the repos file.  By default we forcibly enable
   * only repos which are specified, ignoring the enabled= flag.
   */
  if (!json_object_has_member (treedata, "repos"))
    return glnx_throw (error, "Treefile is missing required \"repos\" member");

  JsonArray *enable_repos = json_object_get_array_member (treedata, "repos");

  if (!set_keyfile_string_array_from_json (treespec, "tree", "repos", enable_repos, error))
    return FALSE;

  if (!treespec_bind_bool (treedata, treespec, "documentation", TRUE, error))
    return FALSE;
  if (!treespec_bind_bool (treedata, treespec, "recommends", TRUE, error))
    return FALSE;

  { g_autoptr(GError) tmp_error = NULL;
    g_autoptr(RpmOstreeTreespec) treespec_value = rpmostree_treespec_new_from_keyfile (treespec, &tmp_error);
    g_assert_no_error (tmp_error);

    g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (rootfs_dfd, ".");
    if (!rpmostree_context_setup (self->corectx, tmprootfs_abspath, NULL, treespec_value,
                                  cancellable, error))
      return FALSE;
  }

  /* For unified core, we have a pkgcache repo. This may be auto-created under
   * the workdir, or live explicitly in the dir for --cache.
   */
  glnx_autofd int host_rootfs_dfd = -1;
  if (opt_unified_core)
    {
      self->pkgcache_repo = ostree_repo_create_at (cachedir_dfd (self), "pkgcache-repo",
                                                   OSTREE_REPO_MODE_BARE_USER, NULL,
                                                   cancellable, error);
      if (!self->pkgcache_repo)
        return FALSE;
      rpmostree_context_set_repos (self->corectx, self->repo, self->pkgcache_repo);
      self->devino_cache = ostree_repo_devino_cache_new ();
      rpmostree_context_set_devino_cache (self->corectx, self->devino_cache);

      /* Ensure that the imported packages are labeled with *a* policy if
       * possible, even if it's not the final one. This helps avoid duplicating
       * all of the content.
       */
      if (!glnx_opendirat (AT_FDCWD, "/", TRUE, &host_rootfs_dfd, error))
        return FALSE;
      g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (host_rootfs_dfd, cancellable, error);
      if (!sepolicy)
        return FALSE;
      if (ostree_sepolicy_get_name (sepolicy) == NULL)
        return glnx_throw (error, "Unable to load SELinux policy from /");
      rpmostree_context_set_sepolicy (self->corectx, sepolicy);
    }

  if (!rpmostree_context_prepare (self->corectx, cancellable, error))
    return FALSE;

  rpmostree_print_transaction (dnfctx);

  JsonArray *add_files = NULL;
  if (json_object_has_member (treedata, "add-files"))
    add_files = json_object_get_array_member (treedata, "add-files");

  /* FIXME - just do a depsolve here before we compute download requirements */
  g_autofree char *ret_new_inputhash = NULL;
  if (!rpmostree_composeutil_checksum (self->serialized_treefile,
                                       dnf_context_get_goal (dnfctx),
                                       self->treefile_rs, add_files,
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
  g_autoptr(GFile) treefile_dirpath = g_file_get_parent (self->treefile_path);
  gboolean generate_from_previous = TRUE;
  if (!_rpmostree_jsonutil_object_get_optional_boolean_member (treedata,
                                                               "preserve-passwd",
                                                               &generate_from_previous,
                                                               error))
    return FALSE;

  if (generate_from_previous)
    {
      const char *dest = opt_unified_core ? "usr/etc/" : "etc/";
      if (!rpmostree_generate_passwd_from_previous (self->repo, rootfs_dfd, dest,
                                                    treefile_dirpath,
                                                    self->previous_root, treedata,
                                                    cancellable, error))
        return FALSE;
    }

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

  glnx_fd_close int json_fd = ror_treefile_to_json (treefile_rs, error);
  if (json_fd < 0)
    return FALSE;
  g_autoptr(GInputStream) json_s = g_unix_input_stream_new (json_fd, FALSE);

  if (!json_parser_load_from_stream (parser, json_s, NULL, error))
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

  if (opt_workdir_tmpfs)
    g_print ("note: --workdir-tmpfs is deprecated and will be ignored\n");

  if (opt_unified_core)
    {
      if (opt_workdir)
        g_printerr ("note: --workdir is ignored for --ex-unified-core\n");

      /* For unified core, our workdir must be underneath the repo tmp/
       * in order to use hardlinks.  We also really want a bare-user repo.
       * We hard require that for now, but down the line we may automatically
       * do a pull-local from the bare-user repo to the archive.
       */
      if (ostree_repo_get_mode (self->repo) != OSTREE_REPO_MODE_BARE_USER)
        return glnx_throw (error, "--ex-unified-core requires a bare-user repository");
      if (!glnx_mkdtempat (ostree_repo_get_dfd (self->repo), "tmp/rpm-ostree-compose.XXXXXX", 0700,
                           &self->workdir_tmp, error))
        return FALSE;
      /* Note special handling of this aliasing in _finalize() */
      self->workdir_dfd = self->workdir_tmp.fd;

    }
  else if (!opt_workdir)
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

  self->treefile_path = g_file_new_for_path (treefile_pathstr);

  if (opt_cachedir)
    {
      if (!glnx_opendirat (AT_FDCWD, opt_cachedir, TRUE, &self->cachedir_dfd, error))
        {
          g_prefix_error (error, "Opening cachedir '%s': ", opt_cachedir);
          return FALSE;
        }
    }
  else
    {
      self->cachedir_dfd = fcntl (self->workdir_dfd, F_DUPFD_CLOEXEC, 3);
      if (self->cachedir_dfd < 0)
        return glnx_throw_errno_prefix (error, "fcntl");
    }

  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  if (opt_metadata_json)
    {
      glnx_unref_object JsonParser *jparser = json_parser_new ();
      if (!json_parser_load_from_file (jparser, opt_metadata_json, error))
        return FALSE;

      JsonNode *metarootval = json_parser_get_root (jparser);
      g_autoptr(GVariant) jsonmetav = json_gvariant_deserialize (metarootval, "a{sv}", error);
      if (!jsonmetav)
        {
          g_prefix_error (error, "Parsing %s: ", opt_metadata_json);
          return FALSE;
        }

      GVariantIter viter;
      g_variant_iter_init (&viter, jsonmetav);
      { char *key;
        GVariant *value;
        while (g_variant_iter_loop (&viter, "{sv}", &key, &value))
          g_hash_table_replace (self->metadata, g_strdup (key), g_variant_ref (value));
      }
    }

  if (opt_metadata_strings)
    {
      if (!parse_metadata_keyvalue_strings (opt_metadata_strings, self->metadata, error))
        return FALSE;
    }

  self->corectx = rpmostree_context_new_tree (self->cachedir_dfd, self->repo, cancellable, error);
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

  g_autoptr(GHashTable) varsubsts = rpmostree_dnfcontext_get_varsubsts (rpmostree_context_get_dnf (self->corectx));
  const char *input_ref = NULL;
  if (!_rpmostree_jsonutil_object_get_optional_string_member (self->treefile, "ref", &input_ref, error))
    return FALSE;
  if (input_ref)
    {
      self->ref = _rpmostree_varsubst_string (input_ref, varsubsts, error);
      if (!self->ref)
        return FALSE;
    }

  g_autoptr(GFile) treefile_dir = g_file_get_parent (self->treefile_path);

  *out_context = g_steal_pointer (&self);
  return TRUE;
}

static gboolean
impl_install_tree (RpmOstreeTreeComposeContext *self,
                   gboolean        *out_changed,
                   GCancellable    *cancellable,
                   GError         **error)
{
  if (getuid () != 0)
    {
      if (!opt_unified_core)
        return glnx_throw (error, "This command requires root privileges");
      g_printerr ("NOTICE: Running this command as non-root is currently known not to work completely.\n");
      g_printerr ("NOTICE: Proceeding anyways.\n");
    }

  if (!opt_unified_core)
    {
      /* This call is...old, dates back to when rpm-ostree wrapped
       * running yum as a subprocess.  It shouldn't be necessary
       * any more, but let's be conservative and not do it in
       * unified core mode.
       */
      if (fchdir (self->workdir_dfd) != 0)
        return glnx_throw_errno_prefix (error, "fchdir");
    }

  /* Set this early here, so we only have to set it one more time in the
   * complete exit path too.
   */
  *out_changed = FALSE;

  if (opt_print_only)
    {
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      g_autoptr(GOutputStream) stdout = g_unix_output_stream_new (1, FALSE);

      json_generator_set_pretty (generator, TRUE);
      json_generator_set_root (generator, self->treefile_rootval);
      (void) json_generator_to_stream (generator, stdout, NULL, NULL);

      /* Note early return */
      return TRUE;
    }

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

  g_autoptr(GPtrArray) packages = g_ptr_array_new_with_free_func (g_free);

  if (json_object_has_member (self->treefile, "bootstrap_packages"))
    {
      if (!_rpmostree_jsonutil_append_string_array_to (self->treefile, "bootstrap_packages", packages, error))
        return FALSE;
    }
  if (!_rpmostree_jsonutil_append_string_array_to (self->treefile, "packages", packages, error))
    return FALSE;

  { g_autofree char *thisarch_packages = g_strconcat ("packages-", dnf_context_get_base_arch (rpmostree_context_get_dnf (self->corectx)), NULL);

    if (json_object_has_member (self->treefile, thisarch_packages))
      {
        if (!_rpmostree_jsonutil_append_string_array_to (self->treefile, thisarch_packages, packages, error))
          return FALSE;
      }
  }

  if (packages->len == 0)
    return glnx_throw (error, "Missing 'packages' entry");

  /* make NULL-terminated */
  g_ptr_array_add (packages, NULL);

  { glnx_unref_object JsonGenerator *generator = json_generator_new ();
    char *treefile_buf = NULL;
    gsize len;

    json_generator_set_root (generator, self->treefile_rootval);
    json_generator_set_pretty (generator, TRUE);
    treefile_buf = json_generator_to_data (generator, &len);

    self->serialized_treefile = g_bytes_new_take (treefile_buf, len);
  }

  /* Download rpm-md repos, packages, do install */
  g_autofree char *new_inputhash = NULL;
  { gboolean unmodified = FALSE;

    if (!install_packages_in_root (self, self->treefile, self->rootfs_dfd,
                                   (char**)packages->pdata,
                                   opt_force_nocache ? NULL : &unmodified,
                                   &new_inputhash,
                                   cancellable, error))
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
  if (!rpmostree_treefile_postprocessing (self->rootfs_dfd, self->treefile_rs,
                                          self->serialized_treefile, self->treefile,
                                          next_version, opt_unified_core,
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
  g_autoptr(GVariant) metadata = NULL;
  { g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    GLNX_HASH_TABLE_FOREACH_KV (self->metadata, const char*, strkey, GVariant*, v)
      g_variant_builder_add (metadata_builder, "{sv}", strkey, v);

    /* include list of packages in rpmdb; this is used client-side for easily previewing
     * pending updates. once we only support unified core composes, this can easily be much
     * more readily injected during assembly */
    g_autoptr(GVariant) rpmdb_v = NULL;
    if (!rpmostree_create_rpmdb_pkglist_variant (self->rootfs_dfd, ".", &rpmdb_v,
                                                 cancellable, error))
      return FALSE;
    g_variant_builder_add (metadata_builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb_v);

    metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
    /* Canonicalize to big endian, like OSTree does. Without this, any numbers
     * we place in the metadata will be unreadable since clients won't know
     * their endianness.
     */
    if (G_BYTE_ORDER != G_BIG_ENDIAN)
      {
        GVariant *swapped = g_variant_byteswap (metadata);
        GVariant *orig = metadata;
        metadata = swapped;
        g_variant_unref (orig);
      }
  }

  if (!rpmostree_rootfs_postprocess_common (self->rootfs_dfd, cancellable, error))
    return FALSE;
  if (!rpmostree_postprocess_final (self->rootfs_dfd, self->treefile, opt_unified_core,
                                    cancellable, error))
    return FALSE;

  if (self->treefile)
    {
      g_autoptr(GFile) treefile_dirpath = g_file_get_parent (self->treefile_path);
      g_autoptr(GPtrArray) sysuser_entries = NULL;
      if (!rpmostree_check_passwd (self->repo, self->rootfs_dfd, treefile_dirpath, self->treefile,
                                   self->previous_checksum, &sysuser_entries,
                                   cancellable, error))
        return glnx_prefix_error (error, "Handling passwd db");

      if (!rpmostree_check_groups (self->repo, self->rootfs_dfd, treefile_dirpath, self->treefile,
                                   self->previous_checksum, &sysuser_entries,
                                   cancellable, error))
        return glnx_prefix_error (error, "Handling group db");

      if (sysuser_entries)
        {
          g_autofree gchar *sysuser_content = NULL;
          struct stat empty_stat;
          const char *sysuser_folder = "usr/lib/sysusers.d";

          if (!rpmostree_passwd_sysusers2char (sysuser_entries,
                                              &sysuser_content, error))
            return glnx_prefix_error (error, "Handling sysuser conversion");

          /* Do a deletion of original /usr/lib/sysusers.d/ to
           * avoid duplication of existing sysuser entries */
          if (fstatat (self->rootfs_dfd, sysuser_folder, &empty_stat, AT_SYMLINK_NOFOLLOW) == 0)
            if (!glnx_shutil_rm_rf_at (self->rootfs_dfd, sysuser_folder, cancellable, error))
              return FALSE;

          /* Creation of the converted sysuser entries into a conf file in
           * sysuser folder */
          if (!glnx_ensure_dir (self->rootfs_dfd, sysuser_folder, 0755, error))
            return FALSE;
          if (!glnx_file_replace_contents_at (self->rootfs_dfd, "usr/lib/sysusers.d/rpm-ostree-base.conf",
                                              (guint8*)sysuser_content, -1,
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            return FALSE;
        }
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
      if (!ostree_repo_prepare_transaction (self->repo, NULL, cancellable, error))
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
  if (!rpmostree_compose_commit (self->rootfs_dfd, self->repo, parent_revision,
                                 metadata, gpgkey, selinux, self->devino_cache,
                                 &new_revision, cancellable, error))
    return FALSE;

  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (self->repo, new_revision, &new_commit,
                                NULL, error))
    return FALSE;
  g_autoptr(GVariant) new_commit_inline_meta = g_variant_get_child_value (new_commit, 0);

  /* --write-commitid-to overrides writing the ref */
  if (self->ref && !opt_write_commitid_to)
    {
      if (use_txn)
        ostree_repo_transaction_set_ref (self->repo, NULL, self->ref, new_revision);
      else
        {
          if (!ostree_repo_set_ref_immediate (self->repo, NULL, self->ref, new_revision,
                                              cancellable, error))
            return FALSE;
        }
    }

  if (use_txn)
    {
      OstreeRepoTransactionStats stats = { 0, };
      if (!ostree_repo_commit_transaction (self->repo, &stats, cancellable, error))
        return glnx_prefix_error (error, "Commit");

      g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-n-metadata-total",
                             g_variant_new_uint32 (stats.metadata_objects_total));
      g_print ("Metadata Total: %u\n", stats.metadata_objects_total);

      g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-n-metadata-written",
                             g_variant_new_uint32 (stats.metadata_objects_written));
      g_print ("Metadata Written: %u\n", stats.metadata_objects_written);

      g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-n-content-total",
                             g_variant_new_uint32 (stats.content_objects_total));
      g_print ("Content Total: %u\n", stats.content_objects_total);

      g_print ("Content Written: %u\n", stats.content_objects_written);
      g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-n-content-written",
                             g_variant_new_uint32 (stats.content_objects_written));

      g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);
      g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-content-bytes-written",
                             g_variant_new_uint64 (stats.content_bytes_written));
    }
  g_print ("Wrote commit: %s\n", new_revision);
  g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-commit", g_variant_new_string (new_revision));
  /* Since JavaScript doesn't have 64 bit integers and hence neither does JSON,
   * store this as a string:
   * https://stackoverflow.com/questions/10286204/the-right-json-date-format
   * */
  { guint64 commit_ts = ostree_commit_get_timestamp (new_commit);
    g_autoptr(GDateTime) timestamp = g_date_time_new_from_unix_utc (commit_ts);
    /* If this fails...something went badly wrong */
    g_assert (timestamp);
    g_autofree char *commit_ts_iso_8601 = g_date_time_format (timestamp, "%FT%H:%M:%SZ");
    g_assert (commit_ts_iso_8601);
    g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-timestamp", g_variant_new_string (commit_ts_iso_8601));
  }
  const char *commit_version = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, OSTREE_COMMIT_META_KEY_VERSION, "&s", &commit_version);
  if (commit_version)
    g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-version", g_variant_new_string (commit_version));

  const char *inputhash = NULL;
  (void)g_variant_lookup (new_commit_inline_meta, "rpmostree.inputhash", "&s", &inputhash);
  /* We may not have the inputhash in the split-up installroot case */
  if (inputhash)
    g_variant_builder_add (&composemeta_builder, "{sv}", "rpm-ostree-inputhash", g_variant_new_string (inputhash));
  if (parent_revision)
    g_variant_builder_add (&composemeta_builder, "{sv}", "ostree-parent-commit", g_variant_new_string (parent_revision));

  if (opt_write_commitid_to)
    {
      if (!g_file_set_contents (opt_write_commitid_to, new_revision, -1, error))
        return glnx_prefix_error (error, "While writing to '%s'", opt_write_commitid_to);
    }
  else if (self->ref)
    {
      g_print ("%s => %s\n", self->ref, new_revision);
      g_variant_builder_add (&composemeta_builder, "{sv}", "ref", g_variant_new_string (self->ref));
    }

  if (opt_write_composejson_to && parent_revision)
    {
      g_autoptr(GVariant) diffv = NULL;
      if (!rpm_ostree_db_diff_variant (self->repo, parent_revision, new_revision,
                                       FALSE, &diffv, cancellable, error))
        return FALSE;
      g_variant_builder_add (&composemeta_builder, "{sv}", "pkgdiff", diffv);
    }

  if (opt_write_composejson_to)
    {
      g_autoptr(GVariant) composemeta_v = g_variant_builder_end (&composemeta_builder);
      JsonNode *composemeta_node = json_gvariant_serialize (composemeta_v);
      glnx_unref_object JsonGenerator *generator = json_generator_new ();
      json_generator_set_root (generator, composemeta_node);

      char *dnbuf = strdupa (opt_write_composejson_to);
      const char *dn = dirname (dnbuf);
      g_auto(GLnxTmpfile) tmpf = { 0, };
      if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dn, O_WRONLY | O_CLOEXEC,
                                          &tmpf, error))
        return FALSE;
      g_autoptr(GOutputStream) out = g_unix_output_stream_new (tmpf.fd, FALSE);
      /* See also similar code in status.c */
      if (json_generator_to_stream (generator, out, NULL, error) <= 0
          || (error != NULL && *error != NULL))
        return FALSE;

      /* World readable to match --write-commitid-to which uses umask */
      if (!glnx_fchmod (tmpf.fd, 0644, error))
        return FALSE;

      if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_REPLACE,
                                 AT_FDCWD, opt_write_composejson_to, error))
        return FALSE;
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
