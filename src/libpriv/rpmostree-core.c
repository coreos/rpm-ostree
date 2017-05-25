/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <glib-unix.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmts.h>
#include <gio/gunixoutputstream.h>
#include <systemd/sd-journal.h>
#include <libdnf/libdnf.h>
#include <librepo/librepo.h>

#include "rpmostree-core.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-unpacker.h"
#include "rpmostree-output.h"

#define RPMOSTREE_MESSAGE_COMMIT_STATS SD_ID128_MAKE(e6,37,2e,38,41,21,42,a9,bc,13,b6,32,b3,f8,93,44)
#define RPMOSTREE_MESSAGE_SELINUX_RELABEL SD_ID128_MAKE(5a,e0,56,34,f2,d7,49,3b,b1,58,79,b7,0c,02,e6,5d)
#define RPMOSTREE_MESSAGE_PKG_REPOS SD_ID128_MAKE(0e,ea,67,9b,bf,a3,4d,43,80,2d,ec,99,b2,74,eb,e7)
#define RPMOSTREE_MESSAGE_PKG_IMPORT SD_ID128_MAKE(df,8b,b5,4f,04,fa,47,08,ac,16,11,1b,bf,4b,a3,52)

#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "solv"
#define RPMOSTREE_DIR_LOCK "lock"

static OstreeRepo * get_pkgcache_repo (RpmOstreeContext *self);

/***********************************************************
 *                    RpmOstreeTreespec                    *
 ***********************************************************
 */

struct _RpmOstreeTreespec {
  GObject parent;

  GVariant *spec;
  GVariantDict *dict;
};

G_DEFINE_TYPE (RpmOstreeTreespec, rpmostree_treespec, G_TYPE_OBJECT)

static void
rpmostree_treespec_finalize (GObject *object)
{
  RpmOstreeTreespec *self = RPMOSTREE_TREESPEC (object);

  g_clear_pointer (&self->spec, g_variant_unref);
  g_clear_pointer (&self->dict, g_variant_dict_unref);

  G_OBJECT_CLASS (rpmostree_treespec_parent_class)->finalize (object);
}

static void
rpmostree_treespec_class_init (RpmOstreeTreespecClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = rpmostree_treespec_finalize;
}

static void
rpmostree_treespec_init (RpmOstreeTreespec *self)
{
}

static int
qsort_cmpstr (const void*ap, const void *bp, gpointer data)
{
  const char *a = *((const char *const*) ap);
  const char *b = *((const char *const*) bp);
  return strcmp (a, b);
}

static void
add_canonicalized_string_array (GVariantBuilder *builder,
                                const char *key,
                                const char *notfound_key,
                                GKeyFile *keyfile)
{
  g_auto(GStrv) input = NULL;
  g_autoptr(GHashTable) set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autofree char **sorted = NULL;
  guint count;
  char **iter;

  input = g_key_file_get_string_list (keyfile, "tree", key, NULL, NULL);
  if (!(input && *input))
    {
      if (notfound_key)
        {
          g_variant_builder_add (builder, "{sv}", notfound_key, g_variant_new_boolean (TRUE));
          return;
        }
    }

  for (iter = input; iter && *iter; iter++)
    {
      g_autofree char *stripped = g_strdup (*iter);
      g_strchug (g_strchomp (stripped));
      g_hash_table_add (set, g_steal_pointer (&stripped));
    }

  sorted = (char**)g_hash_table_get_keys_as_array (set, &count);
  g_qsort_with_data (sorted, count, sizeof (void*), qsort_cmpstr, NULL);

  g_variant_builder_add (builder, "{sv}", key,
                         g_variant_new_strv ((const char*const*)sorted, g_strv_length (sorted)));
}


RpmOstreeTreespec *
rpmostree_treespec_new_from_keyfile (GKeyFile   *keyfile,
                                     GError    **error)
{
  g_autoptr(RpmOstreeTreespec) ret = g_object_new (RPMOSTREE_TYPE_TREESPEC, NULL);
  g_auto(GVariantBuilder) builder;

  g_variant_builder_init (&builder, (GVariantType*)"a{sv}");

  /* We allow the "ref" key to be missing for cases where we don't need one.
   * This is abusing the Treespec a bit, but oh well... */
  { g_autofree char *ref = g_key_file_get_string (keyfile, "tree", "ref", NULL);
    if (ref)
      g_variant_builder_add (&builder, "{sv}", "ref", g_variant_new_string (ref));
  }

  add_canonicalized_string_array (&builder, "packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "cached-packages", NULL, keyfile);

  /* We allow the "repo" key to be missing. This means that we rely on hif's
   * normal behaviour (i.e. look at repos in repodir with enabled=1). */
  { g_auto(GStrv) val = g_key_file_get_string_list (keyfile, "tree", "repos", NULL, NULL);
    if (val && *val)
      add_canonicalized_string_array (&builder, "repos", NULL, keyfile);
  }
  add_canonicalized_string_array (&builder, "instlangs", "instlangs-all", keyfile);
  add_canonicalized_string_array (&builder, "ignore-scripts", NULL, keyfile);

  { gboolean documentation = TRUE;
    g_autofree char *value = g_key_file_get_value (keyfile, "tree", "documentation", NULL);

    if (value)
      documentation = g_key_file_get_boolean (keyfile, "tree", "documentation", NULL);

    g_variant_builder_add (&builder, "{sv}", "documentation", g_variant_new_boolean (documentation));
  }

  ret->spec = g_variant_builder_end (&builder);
  ret->dict = g_variant_dict_new (ret->spec);

  return g_steal_pointer (&ret);
}

RpmOstreeTreespec *
rpmostree_treespec_new_from_path (const char *path, GError  **error)
{
  g_autoptr(GKeyFile) specdata = g_key_file_new();
  if (!g_key_file_load_from_file (specdata, path, 0, error))
    return NULL;
  return rpmostree_treespec_new_from_keyfile (specdata, error);
}

RpmOstreeTreespec *
rpmostree_treespec_new (GVariant   *variant)
{
  g_autoptr(RpmOstreeTreespec) ret = g_object_new (RPMOSTREE_TYPE_TREESPEC, NULL);
  ret->spec = g_variant_ref (variant);
  ret->dict = g_variant_dict_new (ret->spec);
  return g_steal_pointer (&ret);
}

const char *
rpmostree_treespec_get_ref (RpmOstreeTreespec    *spec)
{
  const char *r = NULL;
  g_variant_dict_lookup (spec->dict, "ref", "&s", &r);
  return r;
}

GVariant *
rpmostree_treespec_to_variant (RpmOstreeTreespec *spec)
{
  return g_variant_ref (spec->spec);
}

/***********************************************************
 *                    RpmOstreeInstall                     *
 ***********************************************************
 */

struct _RpmOstreeInstall {
  GObject parent;

  GPtrArray *packages_requested;

  /* Target state -- these are populated during prepare_install() */
  GPtrArray *packages_to_download;
  GPtrArray *packages_to_import;
  GPtrArray *packages_to_relabel;

  guint64 n_bytes_to_fetch;

  /* Current state */
  guint n_packages_fetched;
  guint64 n_bytes_fetched;
};

G_DEFINE_TYPE (RpmOstreeInstall, rpmostree_install, G_TYPE_OBJECT)

static void
rpmostree_install_finalize (GObject *object)
{
  RpmOstreeInstall *self = RPMOSTREE_INSTALL (object);

  g_clear_pointer (&self->packages_requested, g_ptr_array_unref);
  g_clear_pointer (&self->packages_to_download, g_ptr_array_unref);
  g_clear_pointer (&self->packages_to_import, g_ptr_array_unref);
  g_clear_pointer (&self->packages_to_relabel, g_ptr_array_unref);

  G_OBJECT_CLASS (rpmostree_install_parent_class)->finalize (object);
}

static void
rpmostree_install_class_init (RpmOstreeInstallClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = rpmostree_install_finalize;
}

static void
rpmostree_install_init (RpmOstreeInstall *self)
{
}

/***********************************************************
 *                    RpmOstreeContext                     *
 ***********************************************************
 */

struct _RpmOstreeContext {
  GObject parent;

  RpmOstreeTreespec *spec;
  gboolean empty;
  DnfContext *hifctx;
  GHashTable *ignore_scripts;
  OstreeRepo *ostreerepo;
  OstreeRepo *pkgcache_repo;
  gboolean unprivileged;
  char *dummy_instroot_path;
  OstreeSePolicy *sepolicy;
  char *passwd_dir;

  char *metadata_dir_path;
  int metadata_dir_fd;
};

G_DEFINE_TYPE (RpmOstreeContext, rpmostree_context, G_TYPE_OBJECT)

static void
rpmostree_context_finalize (GObject *object)
{
  RpmOstreeContext *rctx = RPMOSTREE_CONTEXT (object);

  g_clear_object (&rctx->spec);
  g_clear_object (&rctx->hifctx);

  if (rctx->dummy_instroot_path)
    {
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, rctx->dummy_instroot_path, NULL, NULL);
      g_clear_pointer (&rctx->dummy_instroot_path, g_free);
    }

  g_clear_object (&rctx->pkgcache_repo);
  g_clear_object (&rctx->ostreerepo);

  g_clear_object (&rctx->sepolicy);

  g_clear_pointer (&rctx->passwd_dir, g_free);

  if (rctx->metadata_dir_path)
    {
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, rctx->metadata_dir_path, NULL, NULL);
      g_clear_pointer (&rctx->metadata_dir_path, g_free);
    }

  if (rctx->metadata_dir_fd != -1)
    close (rctx->metadata_dir_fd);

  G_OBJECT_CLASS (rpmostree_context_parent_class)->finalize (object);
}

static void
rpmostree_context_class_init (RpmOstreeContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = rpmostree_context_finalize;
}

static void
rpmostree_context_init (RpmOstreeContext *self)
{
  self->metadata_dir_fd = -1;
}

static void
set_rpm_macro_define (const char *key, const char *value)
{
  g_autofree char *buf = g_strconcat ("%define ", key, " ", value, NULL);
  /* Calling expand with %define (ignoring the return
   * value) is apparently the way to change the global
   * macro context.
   */
  free (rpmExpand (buf, NULL));
}

RpmOstreeContext *
rpmostree_context_new_system (GCancellable *cancellable,
                              GError      **error)
{
  RpmOstreeContext *self = g_object_new (RPMOSTREE_TYPE_CONTEXT, NULL);

  /* We can always be control-c'd at any time; this is new API,
   * otherwise we keep calling _rpmostree_reset_rpm_sighandlers() in
   * various places.
   */
#if BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  rpmsqSetInterruptSafety (FALSE);
#endif

  self->hifctx = dnf_context_new ();
  g_auto(RpmSighandlerResetCleanup) rpmsigreset = { 0, };
  dnf_context_set_http_proxy (self->hifctx, g_getenv ("http_proxy"));

  dnf_context_set_repo_dir (self->hifctx, "/etc/yum.repos.d");
  /* Operating on stale metadata is too annoying -- but provide a way to
   * override this for testing. */
  if (g_getenv("RPMOSTREE_USE_CACHED_METADATA") == NULL)
      dnf_context_set_cache_age (self->hifctx, 0);
  dnf_context_set_cache_dir (self->hifctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_REPOMD);
  dnf_context_set_solv_dir (self->hifctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_SOLV);
  dnf_context_set_lock_dir (self->hifctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);
  dnf_context_set_user_agent (self->hifctx, PACKAGE_NAME "/" PACKAGE_VERSION);

  dnf_context_set_check_disk_space (self->hifctx, FALSE);
  dnf_context_set_check_transaction (self->hifctx, FALSE);
  dnf_context_set_yumdb_enabled (self->hifctx, FALSE);

  return self;
}

static RpmOstreeContext *
rpmostree_context_new_internal (int           userroot_dfd,
                                gboolean      unprivileged,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(RpmOstreeContext) ret =
    rpmostree_context_new_system (cancellable, error);
  if (!ret)
    return NULL;

  ret->unprivileged = unprivileged;

  { g_autofree char *reposdir = glnx_fdrel_abspath (userroot_dfd, "rpmmd.repos.d");
    dnf_context_set_repo_dir (ret->hifctx, reposdir);
  }
  { const char *cache_rpmmd = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_REPOMD);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_rpmmd);
    dnf_context_set_cache_dir (ret->hifctx, cachedir);
  }
  { const char *cache_solv = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_SOLV);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_solv);
    dnf_context_set_solv_dir (ret->hifctx, cachedir);
  }
  { const char *lock = glnx_strjoina ("cache/", RPMOSTREE_DIR_LOCK);
    g_autofree char *lockdir = glnx_fdrel_abspath (userroot_dfd, lock);
    dnf_context_set_lock_dir (ret->hifctx, lockdir);
  }

  /* open user root repo if exists (container path) */
  struct stat stbuf;
  if (fstatat (userroot_dfd, "repo", &stbuf, 0) < 0)
    {
      if (errno != ENOENT)
        return glnx_null_throw_errno_prefix (error, "fstat");
    }
  else
    {
      g_autofree char *repopath_str = glnx_fdrel_abspath (userroot_dfd, "repo");
      g_autoptr(GFile) repopath = g_file_new_for_path (repopath_str);

      ret->ostreerepo = ostree_repo_new (repopath);

      if (!ostree_repo_open (ret->ostreerepo, cancellable, error))
        return NULL;
    }

  return g_steal_pointer (&ret);
}

RpmOstreeContext *
rpmostree_context_new_compose (int basedir_dfd,
                               GCancellable *cancellable,
                               GError **error)
{
  return rpmostree_context_new_internal (basedir_dfd, FALSE, cancellable, error);
}

RpmOstreeContext *
rpmostree_context_new_unprivileged (int basedir_dfd,
                                    GCancellable *cancellable,
                                    GError **error)
{
  return rpmostree_context_new_internal (basedir_dfd, TRUE, cancellable, error);
}

/* Use this if no packages will be installed, and we just want a "dummy" run.
 */
void
rpmostree_context_set_is_empty (RpmOstreeContext *self)
{
  self->empty = TRUE;
}

/* XXX: or put this in new_system() instead? */
void
rpmostree_context_set_repos (RpmOstreeContext *self,
                             OstreeRepo       *base_repo,
                             OstreeRepo       *pkgcache_repo)
{
  g_set_object (&self->ostreerepo, base_repo);
  g_set_object (&self->pkgcache_repo, pkgcache_repo);
}

static OstreeRepo *
get_pkgcache_repo (RpmOstreeContext *self)
{
  return self->pkgcache_repo ?: self->ostreerepo;
}

/* I debated making this part of the treespec. Overall, I think it makes more
 * sense to define it outside since the policy to use depends on the context in
 * which the RpmOstreeContext is used, not something we can always guess on our
 * own correctly. */
void
rpmostree_context_set_sepolicy (RpmOstreeContext *self,
                                OstreeSePolicy   *sepolicy)
{
  g_set_object (&self->sepolicy, sepolicy);
}

void
rpmostree_context_set_passwd_dir (RpmOstreeContext *self,
                                  const char *passwd_dir)
{
  g_clear_pointer (&self->passwd_dir, g_free);
  self->passwd_dir = g_strdup (passwd_dir);
}

void
rpmostree_context_set_ignore_scripts (RpmOstreeContext *self,
                                      GHashTable   *ignore_scripts)
{
  g_clear_pointer (&self->ignore_scripts, g_hash_table_unref);
  if (ignore_scripts)
    self->ignore_scripts = g_hash_table_ref (ignore_scripts);
}

DnfContext *
rpmostree_context_get_hif (RpmOstreeContext *self)
{
  return self->hifctx;
}

GHashTable *
rpmostree_context_get_varsubsts (RpmOstreeContext *context)
{
  GHashTable *r = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_insert (r, g_strdup ("basearch"), g_strdup (dnf_context_get_base_arch (context->hifctx)));

  return r;
}

static void
require_enabled_repos (GPtrArray *sources)
{
  for (guint i = 0; i < sources->len; i++)
    {
      DnfRepo *src = sources->pdata[i];
      if (dnf_repo_get_enabled (src) != DNF_REPO_ENABLED_NONE)
        dnf_repo_set_required (src, TRUE);
    }
}

static gboolean
enable_one_repo (GPtrArray           *sources,
                 const char          *reponame,
                 GError             **error)
{
  for (guint i = 0; i < sources->len; i++)
    {
      DnfRepo *src = sources->pdata[i];
      const char *id = dnf_repo_get_id (src);

      if (strcmp (reponame, id) != 0)
        continue;

      dnf_repo_set_enabled (src, DNF_REPO_ENABLED_PACKAGES);
      return TRUE;
    }

  return glnx_throw (error, "Unknown rpm-md repository: %s", reponame);
}

static gboolean
context_repos_enable_only (RpmOstreeContext    *context,
                           const char    *const *enabled_repos,
                           GError       **error)
{
  GPtrArray *sources = dnf_context_get_repos (context->hifctx);
  for (guint i = 0; i < sources->len; i++)
    {
      DnfRepo *src = sources->pdata[i];
      dnf_repo_set_enabled (src, DNF_REPO_ENABLED_NONE);
    }

  for (const char *const *iter = enabled_repos; iter && *iter; iter++)
    {
      if (!enable_one_repo (sources, *iter, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_context_setup (RpmOstreeContext    *self,
                         const char    *install_root,
                         const char    *source_root,
                         RpmOstreeTreespec *spec,
                         GCancellable  *cancellable,
                         GError       **error)
{
  g_autofree char **enabled_repos = NULL;
  g_autofree char **instlangs = NULL;

  self->spec = g_object_ref (spec);

  if (install_root)
    dnf_context_set_install_root (self->hifctx, install_root);
  else
    {
      glnx_fd_close int dfd = -1; /* Auto close, we just use the path */
      if (!rpmostree_mkdtemp ("/tmp/rpmostree-dummy-instroot-XXXXXX",
                              &self->dummy_instroot_path,
                              &dfd, error))
        return FALSE;
      dnf_context_set_install_root (self->hifctx, self->dummy_instroot_path);
    }

  if (source_root)
    dnf_context_set_source_root (self->hifctx, source_root);

  if (g_variant_dict_lookup (self->spec->dict, "instlangs", "^a&s", &instlangs))
    {
      GString *opt = g_string_new ("");
      char **iter;
      gboolean first = TRUE;

      for (iter = instlangs; iter && *iter; iter++)
        {
          const char *v = *iter;
          if (!first)
            g_string_append_c (opt, ':');
          else
            first = FALSE;
          g_string_append (opt, v);
        }

      dnf_context_set_rpm_macro (self->hifctx, "_install_langs", opt->str);
      g_string_free (opt, TRUE);
    }

  /* This is what we use as default. */
  dnf_context_set_rpm_macro (self->hifctx, "_dbpath", "/usr/share/rpm");

  if (!dnf_context_setup (self->hifctx, cancellable, error))
    return FALSE;

  /* NB: missing "repos" --> let hif figure it out for itself */
  if (g_variant_dict_lookup (self->spec->dict, "repos", "^a&s", &enabled_repos))
    if (!context_repos_enable_only (self, (const char *const*)enabled_repos, error))
      return FALSE;

  GPtrArray *repos = dnf_context_get_repos (self->hifctx);
  if (repos->len == 0)
    {
      /* To be nice, let's only make this fatal if "packages" is empty (e.g. if
       * we're only installing local RPMs. Missing deps will cause the regular
       * 'not found' error from libdnf. */
      g_autofree char **pkgs = NULL;
      if (g_variant_dict_lookup (self->spec->dict, "packages", "^a&s", &pkgs) &&
          g_strv_length (pkgs) > 0)
        return glnx_throw (error, "No enabled repositories");
    }
  require_enabled_repos (repos);

  { gboolean docs;

    g_variant_dict_lookup (self->spec->dict, "documentation", "b", &docs);

    if (!docs)
        dnf_transaction_set_flags (dnf_context_get_transaction (self->hifctx),
                                   DNF_TRANSACTION_FLAG_NODOCS);
  }

  { const char *const *ignore_scripts = NULL;
    if (g_variant_dict_lookup (self->spec->dict, "ignore-scripts", "^a&s", &ignore_scripts))
      {
        g_autoptr(GHashTable) ignore_hash = NULL;

        if (!rpmostree_script_ignore_hash_from_strv (ignore_scripts, &ignore_hash, error))
          return FALSE;
        rpmostree_context_set_ignore_scripts (self, ignore_hash);
      }
  }

  return TRUE;
}


static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  rpmostree_output_percent_progress (text, percentage);
}

static gboolean
get_commit_metadata_string (GVariant    *commit,
                            const char  *key,
                            char       **out_str,
                            GError     **error)
{
  g_autoptr(GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) meta_dict = g_variant_dict_new (meta);
  g_autoptr(GVariant) str_variant = NULL;

  str_variant =
    _rpmostree_vardict_lookup_value_required (meta_dict, key,
                                              G_VARIANT_TYPE_STRING, error);
  if (!str_variant)
    return FALSE;

  g_assert (g_variant_is_of_type (str_variant, G_VARIANT_TYPE_STRING));
  *out_str = g_strdup (g_variant_get_string (str_variant, NULL));
  return TRUE;
}

static gboolean
get_commit_sepolicy_csum (GVariant *commit,
                          char    **out_csum,
                          GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.sepolicy",
                                     out_csum, error);
}

static gboolean
get_commit_header_sha256 (GVariant *commit,
                          char    **out_csum,
                          GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.metadata_sha256",
                                     out_csum, error);
}

static gboolean
get_commit_repodata_chksum_repr (GVariant *commit,
                            char    **out_csum,
                            GError  **error)
{
  return get_commit_metadata_string (commit, "rpmostree.repodata_checksum",
                                     out_csum, error);
}

static char *
get_nevra_relpath (const char *nevra)
{
  return g_strdup_printf ("%s.rpm", nevra);
}

static char *
get_package_relpath (DnfPackage *pkg)
{
  return get_nevra_relpath (dnf_package_get_nevra (pkg));
}

static gboolean
checkout_pkg_metadata (RpmOstreeContext *self,
                       const char       *nevra,
                       GVariant         *header,
                       GCancellable     *cancellable,
                       GError          **error)
{
  g_autofree char *path = NULL;
  struct stat stbuf;

  if (self->metadata_dir_path == NULL)
    {
      if (!rpmostree_mkdtemp ("/tmp/rpmostree-metadata-XXXXXX",
                              &self->metadata_dir_path,
                              &self->metadata_dir_fd,
                              error))
        return FALSE;
    }

  /* give it a .rpm extension so we can fool the libdnf stack */
  path = get_nevra_relpath (nevra);

  /* we may have already written the header out for this one */
  if (fstatat (self->metadata_dir_fd, path, &stbuf, 0) == 0)
    return TRUE;

  if (errno != ENOENT)
    return glnx_throw_errno (error);

  return glnx_file_replace_contents_at (self->metadata_dir_fd, path,
                                        g_variant_get_data (header),
                                        g_variant_get_size (header),
                                        GLNX_FILE_REPLACE_NODATASYNC,
                                        cancellable, error);
}

static gboolean
get_header_variant (OstreeRepo       *repo,
                    const char       *cachebranch,
                    GVariant        **out_header,
                    GCancellable     *cancellable,
                    GError          **error)
{
  g_autofree char *cached_rev = NULL;
  g_autoptr(GVariant) pkg_commit = NULL;
  g_autoptr(GVariant) pkg_meta = NULL;
  g_autoptr(GVariantDict) pkg_meta_dict = NULL;
  g_autoptr(GVariant) header = NULL;

  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &cached_rev, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 cached_rev, &pkg_commit, error))
    return FALSE;

  pkg_meta = g_variant_get_child_value (pkg_commit, 0);
  pkg_meta_dict = g_variant_dict_new (pkg_meta);

  header = _rpmostree_vardict_lookup_value_required (pkg_meta_dict,
                                                     "rpmostree.metadata",
                                                     (GVariantType*)"ay",
                                                     error);
  if (!header)
    {
      g_autofree char *nevra = rpmostree_cache_branch_to_nevra (cachebranch);
      g_prefix_error (error, "In commit %s of %s: ", cached_rev, nevra);
      return FALSE;
    }

  *out_header = g_steal_pointer (&header);
  return TRUE;
}

static gboolean
checkout_pkg_metadata_by_dnfpkg (RpmOstreeContext *self,
                                 DnfPackage       *pkg,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  const char *nevra = dnf_package_get_nevra (pkg);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_autoptr(GVariant) header = NULL;

  if (!get_header_variant (pkgcache_repo, cachebranch, &header,
                           cancellable, error))
    return FALSE;

  return checkout_pkg_metadata (self, nevra, header, cancellable, error);
}

gboolean
rpmostree_pkgcache_find_pkg_header (OstreeRepo    *pkgcache,
                                    const char    *nevra,
                                    const char    *expected_sha256,
                                    GVariant     **out_header,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  g_autoptr(GVariant) header = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter it;
  gpointer k;

  /* there's no safe way to convert a nevra string to its
   * cache branch, so let's just do a dumb lookup */

  if (!ostree_repo_list_refs_ext (pkgcache, "rpmostree/pkg", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable,
                                  error))
    return FALSE;

  g_hash_table_iter_init (&it, refs);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      const char *ref = k;
      g_autofree char *cache_nevra = rpmostree_cache_branch_to_nevra (ref);

      if (!g_str_equal (nevra, cache_nevra))
        continue;

      if (expected_sha256 != NULL)
        {
          g_autofree char *commit_csum = NULL;
          g_autoptr(GVariant) commit = NULL;
          g_autofree char *actual_sha256 = NULL;

          if (!ostree_repo_resolve_rev (pkgcache, ref, TRUE,
                                        &commit_csum, error))
            return FALSE;

          if (!ostree_repo_load_commit (pkgcache, commit_csum,
                                        &commit, NULL, error))
            return FALSE;

          if (!get_commit_header_sha256 (commit, &actual_sha256, error))
            return FALSE;

          if (!g_str_equal (expected_sha256, actual_sha256))
            return glnx_throw (error, "Checksum mismatch for package %s", nevra);
        }

      if (!get_header_variant (pkgcache, ref, &header, cancellable, error))
        return FALSE;

      *out_header = g_steal_pointer (&header);
      return TRUE;
    }

  return glnx_throw (error, "Failed to find cached pkg for %s", nevra);
}

static gboolean
checkout_pkg_metadata_by_nevra (RpmOstreeContext *self,
                                const char       *nevra,
                                const char       *sha256,
                                GCancellable     *cancellable,
                                GError          **error)
{
  g_autoptr(GVariant) header = NULL;

  if (!rpmostree_pkgcache_find_pkg_header (self->pkgcache_repo, nevra, sha256,
                                           &header, cancellable, error))
    return FALSE;

  return checkout_pkg_metadata (self, nevra, header,
                                cancellable, error);
}

gboolean
rpmostree_context_download_metadata (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  g_assert (!self->empty);

  g_autoptr(DnfState) hifstate = dnf_state_new ();

  guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed),
                                           "Downloading metadata:");

  g_auto(RpmSighandlerResetCleanup) rpmsigreset = { 0, };
  if (!dnf_context_setup_sack (self->hifctx, hifstate, error))
    return FALSE;

  g_signal_handler_disconnect (hifstate, progress_sigid);
  rpmostree_output_percent_progress_end ();
  return TRUE;
}

static void
append_quoted (GString *r, const char *value)
{
  const char *p;

  for (p = value; *p; p++)
    {
      const char c = *p;
      switch (c)
        {
        case '.':
        case '-':
          g_string_append_c (r, c);
          continue;
        }
      if (g_ascii_isalnum (c))
        {
          g_string_append_c (r, c);
          continue;
        }
      if (c == '_')
        {
          g_string_append (r, "__");
          continue;
        }

      g_string_append_printf (r, "_%02X", c);
    }
}

static char *
cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch)
{
  GString *r = g_string_new ("rpmostree/pkg/");
  append_quoted (r, name);
  g_string_append_c (r, '/');
  /* Work around the fact that libdnf and librpm have different handling
   * for an explicit Epoch header of zero.  libdnf right now ignores it,
   * but librpm will add an explicit 0:.  Since there's no good reason
   * to have it, let's follow the lead of libdnf.
   *
   * https://github.com/projectatomic/rpm-ostree/issues/349
   */
  if (g_str_has_prefix (evr, "0:"))
    evr += 2;
  append_quoted (r, evr);
  g_string_append_c (r, '.');
  append_quoted (r, arch);
  return g_string_free (r, FALSE);
}

char *
rpmostree_get_cache_branch_header (Header hdr)
{
  g_autofree char *name = headerGetAsString (hdr, RPMTAG_NAME);
  g_autofree char *evr = headerGetAsString (hdr, RPMTAG_EVR);
  g_autofree char *arch = headerGetAsString (hdr, RPMTAG_ARCH);
  return cache_branch_for_n_evr_a (name, evr, arch);
}

char *
rpmostree_get_cache_branch_pkg (DnfPackage *pkg)
{
  return cache_branch_for_n_evr_a (dnf_package_get_name (pkg),
                                   dnf_package_get_evr (pkg),
                                   dnf_package_get_arch (pkg));
}

static gboolean
pkg_is_local (DnfPackage *pkg)
{
  const char *reponame = dnf_package_get_reponame (pkg);
  return (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0 ||
          dnf_repo_is_local (dnf_package_get_repo (pkg)));
}

static gboolean
commit_has_matching_sepolicy (OstreeRepo     *repo,
                              const char     *head,
                              OstreeSePolicy *sepolicy,
                              gboolean       *out_matches,
                              GError        **error)
{
  const char *sepolicy_csum_wanted = ostree_sepolicy_get_csum (sepolicy);
  g_autoptr(GVariant) commit = NULL;
  g_autofree char *sepolicy_csum = NULL;

  if (!ostree_repo_load_commit (repo, head, &commit, NULL, error))
    return FALSE;
  g_assert (commit);

  if (!get_commit_sepolicy_csum (commit, &sepolicy_csum, error))
    return FALSE;

  *out_matches = g_str_equal (sepolicy_csum, sepolicy_csum_wanted);

  return TRUE;
}

static gboolean
commit_has_matching_repodata_chksum_repr (OstreeRepo  *repo,
                                          const char  *rev,
                                          const char  *expected,
                                          gboolean    *out_matches,
                                          GError     **error)
{
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, rev, &commit, NULL, error))
    return FALSE;

  g_assert (commit);

  g_autofree char *actual = NULL;
  g_autoptr(GError) tmp_error = NULL;
  if (!get_commit_repodata_chksum_repr (commit, &actual, &tmp_error))
    {
      /* never match pkgs unpacked with older versions that didn't embed csum */
      if (g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          *out_matches = FALSE;
          return TRUE;
        }
      g_propagate_error (error, g_steal_pointer (&tmp_error));
      return FALSE;
    }

  *out_matches = g_str_equal (expected, actual);
  return TRUE;
}

static gboolean
pkg_is_cached (DnfPackage *pkg)
{
  if (pkg_is_local (pkg))
    return TRUE;

  /* Right now we're not re-checksumming cached RPMs, we
   * assume they are valid.  This is a change from the current
   * libhif behavior, but I think it's right.  We should
   * record validity once, then ensure it's immutable after
   * that - which is what happens with the ostree commits
   * above.
   */
  return g_file_test (dnf_package_get_filename (pkg), G_FILE_TEST_EXISTS);
}

static gboolean
find_pkg_in_ostree (OstreeRepo     *repo,
                    DnfPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    gboolean       *out_in_ostree,
                    gboolean       *out_selinux_match,
                    GError        **error)
{
  gboolean in_ostree = FALSE;
  gboolean selinux_match = FALSE;
  g_autofree char *cached_rev = NULL;
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);

  /* NB: we're not using a pkgcache yet in the compose path */
  if (repo == NULL)
    goto done; /* Note early happy return */

  if (!ostree_repo_resolve_rev (repo, cachebranch, TRUE,
                                &cached_rev, error))
    return FALSE;

  if (!cached_rev)
    goto done; /* Note early happy return */

  /* NB: we do an exception for LocalPackages here; we've already checked that
   * its cache is valid and matches what's in the origin. We never want to fetch
   * newer versions of LocalPackages from the repos. But we do want to check
   * whether a relabel will be necessary. */

  const char *reponame = dnf_package_get_reponame (pkg);
  if (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) != 0)
    {
      g_autofree char *expected_chksum_repr = NULL;
      if (!rpmostree_get_repodata_chksum_repr (pkg, &expected_chksum_repr,
                                               error))
        return FALSE;

      gboolean same_pkg_chksum = FALSE;
      if (!commit_has_matching_repodata_chksum_repr (repo, cached_rev,
                                                     expected_chksum_repr,
                                                     &same_pkg_chksum, error))
        return FALSE;

      if (!same_pkg_chksum)
        goto done; /* Note early happy return */
    }

  in_ostree = TRUE;
  if (sepolicy)
    {
      if (!commit_has_matching_sepolicy (repo, cached_rev, sepolicy,
                                         &selinux_match, error))
        return FALSE;
    }

done:
  *out_in_ostree = in_ostree;
  *out_selinux_match = selinux_match;
  return TRUE;
}

/* determine of all the marked packages, which ones we'll need to download,
 * which ones we'll need to import, and which ones we'll need to relabel */
static gboolean
sort_packages (DnfContext       *hifctx,
               OstreeRepo       *ostreerepo,
               OstreeSePolicy   *sepolicy,
               RpmOstreeInstall *install,
               GError          **error)
{
  g_autoptr(GPtrArray) packages = NULL;
  GPtrArray *sources = dnf_context_get_repos (hifctx);

  g_clear_pointer (&install->packages_to_download, g_ptr_array_unref);
  install->packages_to_download
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_clear_pointer (&install->packages_to_import, g_ptr_array_unref);
  install->packages_to_import
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_clear_pointer (&install->packages_to_relabel, g_ptr_array_unref);
  install->packages_to_relabel
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  packages = dnf_goal_get_packages (dnf_context_get_goal (hifctx),
                                    DNF_PACKAGE_INFO_INSTALL, -1);

  for (guint i = 0; i < packages->len; i++)
    {
      DnfPackage *pkg = packages->pdata[i];
      const char *reponame = dnf_package_get_reponame (pkg);
      gboolean is_locally_cached =
        (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0);

      /* make sure all the non-cached pkgs have their repos set */
      if (!is_locally_cached)
        {
          DnfRepo *src = NULL;

          /* Hackily look up the source...we need a hash table */
          for (guint j = 0; j < sources->len && !src; j++)
            {
              DnfRepo *tmpsrc = sources->pdata[j];
              if (g_strcmp0 (reponame, dnf_repo_get_id (tmpsrc)) == 0)
                src = tmpsrc;
            }

          g_assert (src);
          dnf_package_set_repo (pkg, src);
        }

      /* NB: We're assuming here that the presence of an ostree repo means that
       * the user intends to import the pkg vs e.g. installing it like during a
       * treecompose. Even though in the treecompose case, an ostree repo *is*
       * given, since it shouldn't have imported pkgs in there, the logic below
       * will work. (So e.g. pkgs might still get added to the import array in
       * the treecompose path, but since it will never call import(), that
       * doesn't matter). In the future, we might want to allow the user of
       * RpmOstreeContext to express *why* they are calling prepare_install().
       * */

      {
        gboolean in_ostree = FALSE;
        gboolean selinux_match = FALSE;
        gboolean cached = pkg_is_cached (pkg);

        if (!find_pkg_in_ostree (ostreerepo, pkg, sepolicy,
                                 &in_ostree, &selinux_match, error))
          return FALSE;

        if (is_locally_cached)
          g_assert (in_ostree);

        if (!in_ostree && !cached)
          g_ptr_array_add (install->packages_to_download, g_object_ref (pkg));
        if (!in_ostree)
          g_ptr_array_add (install->packages_to_import, g_object_ref (pkg));
        if (in_ostree && !selinux_match)
          g_ptr_array_add (install->packages_to_relabel, g_object_ref (pkg));
      }
    }

  return TRUE;
}

static char *
join_package_list (const char *prefix, char **pkgs, int len)
{
  GString *ret = g_string_new (prefix);

  if (len < 0)
    len = g_strv_length (pkgs);

  gboolean first = TRUE;
  for (guint i = 0; i < len; i++)
    {
      if (!first)
        g_string_append (ret, ", ");
      g_string_append (ret, pkgs[i]);
      first = FALSE;
    }

  return g_string_free (ret, FALSE);
}

static gboolean
check_goal_solution (HyGoal    goal,
                     GError  **error)
{
  g_autoptr(GPtrArray) packages = NULL;

  /* Now we need to make sure that none of the pkgs in the base are marked for
   * removal. The issue is that marking a package for DNF_INSTALL could
   * uninstall a base pkg if it's an update or obsoletes it. There doesn't seem
   * to be a way to tell libsolv to never touch pkgs in the base layer, so we
   * just inspect its solution in retrospect. libdnf has the concept of
   * protected packages, but it still allows updating protected packages. */

  packages = dnf_goal_get_packages (goal,
                                    DNF_PACKAGE_INFO_REMOVE,
                                    DNF_PACKAGE_INFO_OBSOLETE,
                                    -1);
  if (packages->len > 0)
    {
      g_autofree char *msg = join_package_list (
          "The following base packages would be removed: ",
          (char**)packages->pdata, packages->len);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
      return FALSE;
    }

  g_ptr_array_unref (packages);

  packages = dnf_goal_get_packages (goal,
                                    DNF_PACKAGE_INFO_REINSTALL,
                                    DNF_PACKAGE_INFO_UPDATE,
                                    DNF_PACKAGE_INFO_DOWNGRADE,
                                    -1);
  if (packages->len > 0)
    {
      g_autoptr(GHashTable) nevras = g_hash_table_new (g_str_hash, g_str_equal);
      g_autofree char **keys = NULL;

      for (guint i = 0; i < packages->len; i++)
        {
          DnfPackage *pkg = packages->pdata[i];
          g_autoptr(GPtrArray) old =
            hy_goal_list_obsoleted_by_package (goal, pkg);
          for (guint j = 0; j < old->len; j++)
            g_hash_table_add (nevras,
                              (gpointer)dnf_package_get_nevra (old->pdata[j]));
        }

      keys = (char**)g_hash_table_get_keys_as_array (nevras, NULL);
      g_autofree char *msg = join_package_list (
          "The following base packages would be replaced: ", keys, -1);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
      return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_context_prepare_install (RpmOstreeContext    *self,
                                   RpmOstreeInstall    **out_install,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  DnfContext *hifctx = self->hifctx;
  g_autofree char **pkgnames = NULL;
  g_autofree char **cached_pkgnames = NULL;
  DnfSack *sack = dnf_context_get_sack (hifctx);
  HyGoal goal = dnf_context_get_goal (hifctx);

  g_autoptr(RpmOstreeInstall) ret_install = g_object_new (RPMOSTREE_TYPE_INSTALL, NULL);

  g_assert (g_variant_dict_lookup (self->spec->dict, "packages", "^a&s", &pkgnames));
  g_assert (g_variant_dict_lookup (self->spec->dict, "cached-packages", "^a&s", &cached_pkgnames));
  g_assert (!self->empty);

  ret_install->packages_requested = g_ptr_array_new_with_free_func (g_free);

  for (char **it = cached_pkgnames; it && *it; it++)
    {
      const char *nevra = *it;
      DnfPackage *pkg = NULL;
      g_autofree char *path = NULL;
      g_autofree char *sha256 = NULL;

      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      if (!checkout_pkg_metadata_by_nevra (self, nevra, sha256,
                                           cancellable, error))
        return FALSE;

      /* This is the great lie: we make libdnf et al. think that they're
       * dealing with a full RPM, all while crossing our fingers that they
       * don't try to look past the header. Ideally, it would be best if
       * libdnf could learn to treat the pkgcache repo as another DnfRepo.
       * */
      path = g_strdup_printf ("%s/%s.rpm", self->metadata_dir_path, nevra);
      pkg = dnf_sack_add_cmdline_package (sack, path);
      if (!pkg)
        return glnx_throw (error, "Failed to add local pkg %s to sack", nevra);

      hy_goal_install (goal, pkg);
    }

  { GPtrArray *repos = dnf_context_get_repos (hifctx);
    g_autoptr(GString) enabled_repos = g_string_new ("");
    g_autoptr(GString) enabled_repos_solvables = g_string_new ("");
    g_autoptr(GString) enabled_repos_timestamps = g_string_new ("");
    guint total_solvables = 0;
    gboolean first = TRUE;

    for (guint i = 0; i < repos->len; i++)
      {
        DnfRepo *repo = repos->pdata[i];
        if ((dnf_repo_get_enabled (repo) & DNF_REPO_ENABLED_PACKAGES) == 0)
          continue;

        if (first)
          first = FALSE;
        else
          {
            g_string_append (enabled_repos, ", ");
            g_string_append (enabled_repos_solvables, ", ");
            g_string_append (enabled_repos_timestamps, ", ");
          }
        g_autofree char *quoted = g_shell_quote (dnf_repo_get_id (repo));
        g_string_append (enabled_repos, quoted);
        total_solvables += dnf_repo_get_n_solvables (repo);
        g_string_append_printf (enabled_repos_solvables, "%u", dnf_repo_get_n_solvables (repo));
        g_string_append_printf (enabled_repos_timestamps, "%" G_GUINT64_FORMAT, dnf_repo_get_timestamp_generated (repo));
      }

    sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PKG_REPOS),
                     "MESSAGE=Preparing pkg txn; enabled repos: [%s] solvables: %u", enabled_repos->str, total_solvables,
                     "SACK_N_SOLVABLES=%i", dnf_sack_count (dnf_context_get_sack (hifctx)),
                     "ENABLED_REPOS=[%s]", enabled_repos->str,
                     "ENABLED_REPOS_SOLVABLES=[%s]", enabled_repos_solvables->str,
                     "ENABLED_REPOS_TIMESTAMPS=[%s]", enabled_repos_timestamps->str,
                     NULL);
  }

  { const char *const*strviter = (const char *const*)pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!dnf_context_install (hifctx, pkgname, error))
          return FALSE;
        g_ptr_array_add (ret_install->packages_requested, g_strdup (pkgname));
      }
  }

  rpmostree_output_task_begin ("Resolving dependencies");

  if (!dnf_goal_depsolve (goal, DNF_INSTALL, error) ||
      !check_goal_solution (goal, error))
    {
      g_print ("failed\n");
      return FALSE;
    }

  rpmostree_output_task_end ("done");

  if (!sort_packages (hifctx, get_pkgcache_repo (self), self->sepolicy,
                      ret_install, error))
    return FALSE;

  *out_install = g_steal_pointer (&ret_install);
  return TRUE;
}

/* Generate a checksum from a goal in a repeatable fashion -
 * we checksum an ordered array of the checksums of individual
 * packages.  We *used* to just checksum the NEVRAs but that
 * breaks with RPM gpg signatures.
 *
 * This can be used to efficiently see if the goal has changed from a
 * previous one.
 *
 * It can also handle goals in which only dummy header-only pkgs are
 * available. In such cases, it uses the ostree checksum of the pkg
 * instead.
 */
void
rpmostree_dnf_add_checksum_goal (GChecksum   *checksum,
                                 HyGoal       goal)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  guint i;
  g_autoptr(GPtrArray) pkg_checksums = g_ptr_array_new_with_free_func (g_free);

  pkglist = hy_goal_list_installs (goal, NULL);
  g_assert (pkglist);
  for (i = 0; i < pkglist->len; i++)
    {
      DnfPackage *pkg = pkglist->pdata[i];
      g_autofree char* chksum_repr = NULL;
      g_assert (rpmostree_get_repodata_chksum_repr (pkg, &chksum_repr, NULL));
      g_ptr_array_add (pkg_checksums, g_steal_pointer (&chksum_repr));
    }

  g_ptr_array_sort (pkg_checksums, rpmostree_ptrarray_sort_compare_strings);

  for (guint i = 0; i < pkg_checksums->len; i++)
    {
      const char *pkg_checksum = pkg_checksums->pdata[i];
      g_checksum_update (checksum, (guint8*)pkg_checksum, strlen (pkg_checksum));
    }
}

char *
rpmostree_context_get_state_sha512 (RpmOstreeContext *self)
{
  g_autoptr(GChecksum) state_checksum = g_checksum_new (G_CHECKSUM_SHA512);
  g_checksum_update (state_checksum, g_variant_get_data (self->spec->spec),
                     g_variant_get_size (self->spec->spec));

  if (!self->empty)
    rpmostree_dnf_add_checksum_goal (state_checksum, dnf_context_get_goal (self->hifctx));
  return g_strdup (g_checksum_get_string (state_checksum));
}

static GHashTable *
gather_source_to_packages (DnfContext *hifctx,
                           RpmOstreeInstall *install)
{
  guint i;
  g_autoptr(GHashTable) source_to_packages =
    g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  for (i = 0; i < install->packages_to_download->len; i++)
    {
      DnfPackage *pkg = install->packages_to_download->pdata[i];
      DnfRepo *src = dnf_package_get_repo (pkg);
      GPtrArray *source_packages;

      g_assert (src);

      source_packages = g_hash_table_lookup (source_to_packages, src);
      if (!source_packages)
        {
          source_packages = g_ptr_array_new ();
          g_hash_table_insert (source_to_packages, src, source_packages);
        }
      g_ptr_array_add (source_packages, pkg);
    }

  return g_steal_pointer (&source_to_packages);
}

gboolean
rpmostree_context_download (RpmOstreeContext *ctx,
                            RpmOstreeInstall *install,
                            GCancellable     *cancellable,
                            GError          **error)
{
  DnfContext *hifctx = ctx->hifctx;
  int n = install->packages_to_download->len;

  if (n > 0)
    {
      guint64 size = dnf_package_array_get_download_size (install->packages_to_download);
      g_autofree char *sizestr = g_format_size (size);
      g_print ("Will download: %u package%s (%s)\n", n, n > 1 ? "s" : "", sizestr);
    }
  else
    return TRUE;

  { guint progress_sigid;
    GHashTableIter hiter;
    gpointer key, value;
    g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (hifctx, install);

    g_hash_table_iter_init (&hiter, source_to_packages);
    while (g_hash_table_iter_next (&hiter, &key, &value))
      {
        g_autofree char *target_dir = NULL;
        DnfRepo *src = key;
        GPtrArray *src_packages = value;
        glnx_unref_object DnfState *hifstate = dnf_state_new ();

        g_autofree char *prefix
          = g_strdup_printf ("  Downloading from %s:", dnf_repo_get_id (src));

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed),
                                           prefix);

        target_dir = g_build_filename (dnf_repo_get_location (src), "/packages/", NULL);
        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
          return FALSE;

        if (!dnf_repo_download_packages (src, src_packages, target_dir,
                                         hifstate, error))
          return FALSE;

        g_signal_handler_disconnect (hifstate, progress_sigid);
        rpmostree_output_percent_progress_end ();
      }
  }

  return TRUE;
}

static gboolean
import_one_package (RpmOstreeContext *self,
                    DnfContext     *hifctx,
                    DnfPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    GCancellable   *cancellable,
                    GError        **error)
{
  OstreeRepo *ostreerepo = get_pkgcache_repo (self);
  g_autofree char *ostree_commit = NULL;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  g_autofree char *pkg_path;
  DnfRepo *pkg_repo;

  int flags = 0;

  pkg_repo = dnf_package_get_repo (pkg);

  if (pkg_is_local (pkg))
    pkg_path = g_strdup (dnf_package_get_filename (pkg));
  else
    {
      const char *pkg_location = dnf_package_get_location (pkg);
      pkg_path =
        g_build_filename (dnf_repo_get_location (pkg_repo),
                          "packages", glnx_basename (pkg_location), NULL);
    }

  /* Verify signatures if enabled */
  if (!dnf_transaction_gpgcheck_package (dnf_context_get_transaction (hifctx), pkg, error))
    return FALSE;

  flags = RPMOSTREE_UNPACKER_FLAGS_OSTREE_CONVENTION;
  if (self->unprivileged)
    flags |= RPMOSTREE_UNPACKER_FLAGS_UNPRIVILEGED;

  /* TODO - tweak the unpacker flags for containers */
  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, pkg_path, pkg, flags, error);
  if (!unpacker)
    return FALSE;

  if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostreerepo, sepolicy,
                                            &ostree_commit, cancellable, error))
    return glnx_prefix_error (error, "Unpacking %s",
                              dnf_package_get_nevra (pkg));

  if (!pkg_is_local (pkg))
    {
      if (TEMP_FAILURE_RETRY (unlinkat (AT_FDCWD, pkg_path, 0)) < 0)
        return glnx_throw_errno_prefix (error, "Deleting %s", pkg_path);
    }

  return TRUE;
}

static inline void
dnf_state_assert_done (DnfState *hifstate)
{
  gboolean r;
  r = dnf_state_done (hifstate, NULL);
  g_assert (r);
}

gboolean
rpmostree_context_import (RpmOstreeContext *self,
                          RpmOstreeInstall *install,
                          GCancellable     *cancellable,
                          GError          **error)
{
  DnfContext *hifctx = self->hifctx;
  guint progress_sigid;
  int n = install->packages_to_import->len;

  if (n == 0)
    return TRUE;

  g_return_val_if_fail (get_pkgcache_repo (self) != NULL, FALSE);

  if (!dnf_transaction_import_keys (dnf_context_get_transaction (hifctx), error))
    return FALSE;

  {
    glnx_unref_object DnfState *hifstate = dnf_state_new ();
    dnf_state_set_number_steps (hifstate, install->packages_to_import->len);
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed),
                                       "Importing:");

    for (guint i = 0; i < install->packages_to_import->len; i++)
      {
        DnfPackage *pkg = install->packages_to_import->pdata[i];
        if (!import_one_package (self, hifctx, pkg,
                                 self->sepolicy, cancellable, error))
          return FALSE;
        dnf_state_assert_done (hifstate);
      }

    g_signal_handler_disconnect (hifstate, progress_sigid);
    rpmostree_output_percent_progress_end ();
  }

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PKG_IMPORT),
                   "MESSAGE=Imported %u pkg%s", n, n > 1 ? "s" : "",
                   "IMPORTED_N_PKGS=%u", n, NULL);

  return TRUE;
}

static gboolean
checkout_package (OstreeRepo   *repo,
                  DnfPackage   *pkg,
                  int           dfd,
                  const char   *path,
                  OstreeRepoDevInoCache *devino_cache,
                  const char   *pkg_commit,
                  GCancellable *cancellable,
                  GError      **error)
{
  OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

  /* We want the checkout to match the repo type so that we get hardlinks. */
  if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
    opts.mode = OSTREE_REPO_CHECKOUT_MODE_NONE;

  opts.devino_to_csum_cache = devino_cache;

  /* Always want hardlinks */
  opts.no_copy_fallback = TRUE;

  if (!ostree_repo_checkout_at (repo, &opts, dfd, path,
                                pkg_commit, cancellable, error))
    return glnx_prefix_error (error, "Checking out %s",
                              dnf_package_get_nevra (pkg));
  return TRUE;
}

static gboolean
checkout_package_into_root (RpmOstreeContext *self,
                            DnfPackage   *pkg,
                            int           dfd,
                            const char   *path,
                            OstreeRepoDevInoCache *devino_cache,
                            const char   *pkg_commit,
                            GCancellable *cancellable,
                            GError      **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);

  if (pkgcache_repo != self->ostreerepo)
    {
      if (!rpmostree_pull_content_only (self->ostreerepo, pkgcache_repo, pkg_commit,
                                        cancellable, error))
        {
          g_prefix_error (error, "Linking cached content for %s: ", dnf_package_get_nevra (pkg));
          return FALSE;
        }
    }

  if (!checkout_package (pkgcache_repo, pkg, dfd, path,
                         devino_cache, pkg_commit,
                         cancellable, error))
    return FALSE;

  return TRUE;
}

/* Given a path to a file/symlink, make a copy (reflink if possible)
 * of it if it's a hard link.  We need this for three places right now:
 *  - The RPM database
 *  - SELinux policy "denormalization" where a label changes
 *  - Upon applying rpmfi overrides during assembly
 */
static gboolean
break_single_hardlink_at (int           dfd,
                          const char   *path,
                          GCancellable *cancellable,
                          GError      **error)
{
  struct stat stbuf;

  if (fstatat (dfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    return glnx_throw_errno_prefix (error, "fstatat");

  if (!S_ISLNK (stbuf.st_mode) && !S_ISREG (stbuf.st_mode))
    return glnx_throw (error, "Unsupported type for entry '%s'", path);

  if (stbuf.st_nlink > 1)
    {
      guint count;
      gboolean copy_success = FALSE;
      char *path_tmp = glnx_strjoina (path, ".XXXXXX");

      for (count = 0; count < 100; count++)
        {
          g_autoptr(GError) tmp_error = NULL;

          glnx_gen_temp_name (path_tmp);

          if (!glnx_file_copy_at (dfd, path, &stbuf, dfd, path_tmp, 0,
                                  cancellable, &tmp_error))
            {
              if (g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                continue;
              g_propagate_error (error, g_steal_pointer (&tmp_error));
              return FALSE;
            }

          copy_success = TRUE;
          break;
        }

      if (!copy_success)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                       "Exceeded limit of %u file creation attempts", count);
          return FALSE;
        }

      if (renameat (dfd, path_tmp, dfd, path) != 0)
        return glnx_throw_errno_prefix (error, "rename(%s)", path);
    }

  return TRUE;
}

/* Given a directory referred to by @dfd and @dirpath, ensure that physical (or
 * reflink'd) copies of all files are done. */
static gboolean
break_hardlinks_at (int             dfd,
                    const char     *dirpath,
                    GCancellable   *cancellable,
                    GError        **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  if (!glnx_dirfd_iterator_init_at (dfd, dirpath, TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (!break_single_hardlink_at (dfd_iter.fd, dent->d_name,
                                     cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static const char*
get_selinux_label (GVariant *xattrs)
{
  for (int i = 0; i < g_variant_n_children (xattrs); i++)
    {
      const char *name, *value;
      g_variant_get_child (xattrs, i, "(^&ay^&ay)", &name, &value);
      if (strcmp (name, "security.selinux") == 0)
        return value;
    }
  return NULL;
}

static GVariant*
set_selinux_label (GVariant   *xattrs,
                   const char *new_label)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  /* copy all the other xattrs */
  for (int i = 0; i < g_variant_n_children (xattrs); i++)
    {
      const char *name;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (xattrs, i, "(^&ay@ay)", &name, &value);
      if (strcmp (name, "security.selinux") != 0)
        g_variant_builder_add (&builder, "(^ay@ay)", name, value);
    }

  /* add the label if any */
  if (new_label != NULL)
    g_variant_builder_add (&builder, "(^ay^ay)", "security.selinux", new_label);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
relabel_dir_recurse_at (OstreeRepo        *repo,
                        int                dfd,
                        const char        *path,
                        const char        *prefix,
                        OstreeSePolicy    *sepolicy,
                        guint             *inout_n_changed,
                        GCancellable      *cancellable,
                        GError           **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      g_autofree char *fullpath = NULL;

      const char *cur_label = NULL;
      g_autofree char *new_label = NULL;
      g_autoptr(GVariant) cur_xattrs = NULL;
      g_autoptr(GVariant) new_xattrs = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent,
                                                       cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR &&
          dent->d_type != DT_REG &&
          dent->d_type != DT_LNK)
        continue;

      if (!glnx_dfd_name_get_all_xattrs (dfd_iter.fd, dent->d_name, &cur_xattrs,
                                         cancellable, error))
        return FALSE;

      /* may return NULL */
      cur_label = get_selinux_label (cur_xattrs);

      /* build the new full path to use for label lookup (we can't just use
       * glnx_fdrel_abspath() since that will just give a new /proc/self/fd/$fd
       * on each recursion) */
      fullpath = g_build_filename (prefix, dent->d_name, NULL);

      {
        struct stat stbuf;

        if (fstatat (dfd_iter.fd, dent->d_name, &stbuf,
                     AT_SYMLINK_NOFOLLOW) != 0)
          return glnx_throw_errno_prefix (error, "fstatat");

        /* may be NULL */
        if (!ostree_sepolicy_get_label (sepolicy, fullpath, stbuf.st_mode,
                                        &new_label, cancellable, error))
          return FALSE;
      }

      if (g_strcmp0 (cur_label, new_label) != 0)
        {
          if (dent->d_type != DT_DIR)
            if (!break_single_hardlink_at (dfd_iter.fd, dent->d_name,
                                           cancellable, error))
              return FALSE;

          new_xattrs = set_selinux_label (cur_xattrs, new_label);

          if (!glnx_dfd_name_set_all_xattrs (dfd_iter.fd, dent->d_name,
                                             new_xattrs, cancellable, error))
            return FALSE;

          (*inout_n_changed)++;
        }

      if (dent->d_type == DT_DIR)
        if (!relabel_dir_recurse_at (repo, dfd_iter.fd, dent->d_name, fullpath,
                                     sepolicy, inout_n_changed, cancellable, error))
          return FALSE;
    }

  return TRUE;
}

static gboolean
relabel_rootfs (OstreeRepo        *repo,
                int                dfd,
                OstreeSePolicy    *sepolicy,
                guint             *inout_n_changed,
                GCancellable      *cancellable,
                GError           **error)
{
  /* NB: this does mean that / itself will not be labeled properly, but that
   * doesn't matter since it will always exist during overlay */
  return relabel_dir_recurse_at (repo, dfd, ".", "/", sepolicy,
                                 inout_n_changed, cancellable, error);
}

static gboolean
relabel_one_package (OstreeRepo     *repo,
                     DnfPackage     *pkg,
                     OstreeSePolicy *sepolicy,
                     guint          *inout_n_changed,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;

  g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-relabel-XXXXXX");
  glnx_fd_close int tmprootfs_dfd = -1;
  g_autoptr(OstreeRepoDevInoCache) cache = NULL;
  g_autoptr(OstreeRepoCommitModifier) modifier = NULL;
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit_csum = NULL;
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);

  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &commit_csum, error))
    goto out;

  /* create a tmprootfs in ostree tmp dir */
  {
    int repo_dfd = ostree_repo_get_dfd (repo); /* borrowed */

    if (!glnx_mkdtempat (repo_dfd, tmprootfs, 00755, error))
      goto out;

    if (!glnx_opendirat (repo_dfd, tmprootfs, FALSE, &tmprootfs_dfd, error))
      goto out;
  }

  /* checkout the pkg and relabel, breaking hardlinks */

  cache = ostree_repo_devino_cache_new ();

  if (!checkout_package (repo, pkg, tmprootfs_dfd, ".", cache,
                         commit_csum, cancellable, error))
    goto out;

  /* This is where the magic happens. We traverse the tree and relabel stuff,
   * making sure to break hardlinks if needed. */
  if (!relabel_rootfs (repo, tmprootfs_dfd, sepolicy, inout_n_changed, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  /* write to the tree */
  {
    glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

    modifier =
      ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE,
                                       NULL, NULL, NULL);

    ostree_repo_commit_modifier_set_devino_cache (modifier, cache);

    if (!ostree_repo_write_dfd_to_mtree (repo, tmprootfs_dfd, ".", mtree,
                                         modifier, cancellable, error))
      goto out;

    if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
      goto out;

  }

  /* build metadata and commit */
  {
    g_autoptr(GVariant) commit_var = NULL;
    g_autoptr(GVariantDict) meta_dict = NULL;

    if (!ostree_repo_load_commit (repo, commit_csum, &commit_var, NULL, error))
      goto out;

    /* let's just copy the metadata from the previous commit and only change the
     * rpmostree.sepolicy value */
    {
      g_autoptr(GVariant) meta = g_variant_get_child_value (commit_var, 0);
      meta_dict = g_variant_dict_new (meta);

      g_variant_dict_insert (meta_dict, "rpmostree.sepolicy", "s",
                             ostree_sepolicy_get_csum (sepolicy));
    }

    {
      g_autofree char *new_commit_csum = NULL;
      if (!ostree_repo_write_commit (repo, NULL, "", "",
                                     g_variant_dict_end (meta_dict),
                                     OSTREE_REPO_FILE (root), &new_commit_csum,
                                     cancellable, error))
        goto out;

      ostree_repo_transaction_set_ref (repo, NULL, cachebranch,
                                       new_commit_csum);
    }
  }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
out:
  if (tmprootfs_dfd != -1)
    glnx_shutil_rm_rf_at (tmprootfs_dfd, ".", cancellable, NULL);
  return ret;
}

gboolean
rpmostree_context_relabel (RpmOstreeContext *self,
                           RpmOstreeInstall *install,
                           GCancellable     *cancellable,
                           GError          **error)
{
  guint progress_sigid;
  int n = install->packages_to_relabel->len;
  OstreeRepo *ostreerepo = get_pkgcache_repo (self);

  if (n == 0)
    return TRUE;

  g_assert (self->sepolicy);

  g_return_val_if_fail (ostreerepo != NULL, FALSE);

  glnx_unref_object DnfState *hifstate = dnf_state_new ();
  g_autofree char *prefix = g_strdup_printf ("Relabeling %d package%s:",
                                             n, n>1 ? "s" : "");

  dnf_state_set_number_steps (hifstate, install->packages_to_relabel->len);
  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed),
                                     prefix);

  guint n_changed_files = 0;
  guint n_changed_pkgs = 0;
  const guint n_to_relabel = install->packages_to_relabel->len;
  for (guint i = 0; i < n_to_relabel; i++)
    {
        DnfPackage *pkg = install->packages_to_relabel->pdata[i];
        guint pkg_n_changed = 0;
        if (!relabel_one_package (ostreerepo, pkg, self->sepolicy,
                                  &pkg_n_changed, cancellable, error))
          return FALSE;
        if (pkg_n_changed > 0)
          {
            n_changed_files += pkg_n_changed;
            n_changed_pkgs++;
          }
        dnf_state_assert_done (hifstate);
    }

  g_signal_handler_disconnect (hifstate, progress_sigid);
  rpmostree_output_percent_progress_end ();

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_SELINUX_RELABEL),
                   "MESSAGE=Relabeled %u/%u pkgs, %u files changed", n_changed_pkgs, n_to_relabel, n_changed_files,
                   "RELABELED_PKGS=%u/%u", n_changed_pkgs, n_to_relabel,
                   "RELABELED_N_CHANGED_FILES=%u", n_changed_files,
                   NULL);

  return TRUE;
}

typedef struct {
  FD_t current_trans_fd;
  int tmp_metadata_dfd;
} TransactionData;

static void *
ts_callback (const void * h,
             const rpmCallbackType what,
             const rpm_loff_t amount,
             const rpm_loff_t total,
             fnpyKey key,
             rpmCallbackData data)
{
  TransactionData *tdata = data;

  switch (what)
    {
    case RPMCALLBACK_INST_OPEN_FILE:
      {
        DnfPackage *pkg = (void*)key;
        /* just bypass fdrel_abspath here since we know the dfd is not ATCWD and
         * it saves us an allocation */
        g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d/%s.rpm",
                                                 tdata->tmp_metadata_dfd,
                                                 dnf_package_get_nevra (pkg));
        g_assert (tdata->current_trans_fd == NULL);
        tdata->current_trans_fd = Fopen (path, "r.ufdio");
        return tdata->current_trans_fd;
      }
      break;

    case RPMCALLBACK_INST_CLOSE_FILE:
      g_clear_pointer (&tdata->current_trans_fd, Fclose);
      break;

    default:
      break;
    }

  return NULL;
}

static gboolean
get_package_metainfo (int tmp_metadata_dfd,
                      const char *path,
                      Header *out_header,
                      rpmfi *out_fi,
                      GError **error)
{
  glnx_fd_close int metadata_fd = -1;

  if ((metadata_fd = openat (tmp_metadata_dfd, path, O_RDONLY | O_CLOEXEC)) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return rpmostree_unpacker_read_metainfo (metadata_fd, out_header, NULL,
                                           out_fi, error);
}

static gboolean
add_to_transaction (rpmts  ts,
                    DnfPackage *pkg,
                    int tmp_metadata_dfd,
                    gboolean noscripts,
                    GHashTable *ignore_scripts,
                    GCancellable *cancellable,
                    GError **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);
  int r;

  if (!get_package_metainfo (tmp_metadata_dfd, path, &hdr, NULL, error))
    return FALSE;

  if (!noscripts)
    {
      if (!rpmostree_script_txn_validate (pkg, hdr, ignore_scripts, cancellable, error))
        return FALSE;
    }

  r = rpmtsAddInstallElement (ts, hdr, pkg, TRUE, NULL);
  if (r != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to add install element for %s",
                   dnf_package_get_filename (pkg));
      return FALSE;
    }

  return TRUE;
}

static gboolean
run_posttrans_sync (int tmp_metadata_dfd,
                    int rootfs_dfd,
                    DnfPackage *pkg,
                    GHashTable *ignore_scripts,
                    GCancellable *cancellable,
                    GError    **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (tmp_metadata_dfd, path, &hdr, NULL, error))
    return FALSE;

  if (!rpmostree_posttrans_run_sync (pkg, hdr, ignore_scripts, rootfs_dfd,
                                     cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
run_pre_sync (int tmp_metadata_dfd,
              int rootfs_dfd,
              DnfPackage *pkg,
              GHashTable *ignore_scripts,
              GCancellable *cancellable,
              GError    **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (tmp_metadata_dfd, path, &hdr, NULL, error))
    return FALSE;

  if (!rpmostree_pre_run_sync (pkg, hdr, ignore_scripts,
                               rootfs_dfd, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
apply_rpmfi_overrides (int            tmp_metadata_dfd,
                       int            tmprootfs_dfd,
                       DnfPackage    *pkg,
                       GHashTable    *passwdents,
                       GHashTable    *groupents,
                       GCancellable  *cancellable,
                       GError       **error)
{
  int i;
  g_auto(rpmfi) fi = NULL;
  gboolean emitted_nonusr_warning = FALSE;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (tmp_metadata_dfd, path, NULL, &fi, error))
    return FALSE;

  while ((i = rpmfiNext (fi)) >= 0)
    {
      const char *fn = rpmfiFN (fi);
      g_autofree char *modified_fn = NULL;  /* May be used to override fn */
      const char *user = rpmfiFUser (fi) ?: "root";
      const char *group = rpmfiFGroup (fi) ?: "root";
      const char *fcaps = rpmfiFCaps (fi) ?: '\0';
      rpm_mode_t mode = rpmfiFMode (fi);
      rpmfileAttrs fattrs = rpmfiFFlags (fi);
      const gboolean is_ghost = fattrs & RPMFILE_GHOST;
      struct stat stbuf;
      uid_t uid = 0;
      gid_t gid = 0;

      if (g_str_equal (user, "root") &&
          g_str_equal (group, "root"))
        continue;

      /* In theory, RPMs could contain block devices or FIFOs; we would normally
       * have rejected that at the import time, but let's also be sure here.
       */
      if (!(S_ISREG (mode) ||
            S_ISLNK (mode) ||
            S_ISDIR (mode)))
        continue;

      g_assert (fn != NULL);
      fn += strspn (fn, "/");
      g_assert (fn[0]);

      /* /run and /var paths have already been translated to tmpfiles during
       * unpacking */
      if (g_str_has_prefix (fn, "run/") ||
          g_str_has_prefix (fn, "var/"))
        continue;
      else if (g_str_has_prefix (fn, "etc/"))
        {
          /* The tree uses usr/etc */
          fn = modified_fn = g_strconcat ("usr/", fn, NULL);
        }
      else if (!g_str_has_prefix (fn, "usr/"))
        {
          /* TODO: query whether Fedora has anything in this category we care about */
          if (!emitted_nonusr_warning)
            {
              sd_journal_print (LOG_WARNING, "Ignoring rpm mode for non-/usr content: %s", fn);
              emitted_nonusr_warning = TRUE;
            }
          continue;
        }

      if (fstatat (tmprootfs_dfd, fn, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
        {
          /* Not early loop skip; in the ghost case, we expect it to not
           * exist.
           */
          if (errno == ENOENT && is_ghost)
            continue;
          return glnx_throw_errno_prefix (error, "fstatat(%s)", fn);
        }

      if ((S_IFMT & stbuf.st_mode) != (S_IFMT & mode))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Inconsistent file type between RPM and checkout "
                       "for file '%s' in package '%s'", fn,
                       dnf_package_get_name (pkg));
          return FALSE;
        }

      if (!S_ISDIR (stbuf.st_mode))
        {
          if (!break_single_hardlink_at (tmprootfs_dfd, fn, cancellable, error))
            return FALSE;
        }

      if ((!g_str_equal (user, "root") && !passwdents) ||
          (!g_str_equal (group, "root") && !groupents))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Missing passwd/group files for chown");
          return FALSE;
        }

      if (!g_str_equal (user, "root"))
        {
          struct conv_passwd_ent *passwdent =
            g_hash_table_lookup (passwdents, user);

          if (!passwdent)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find user '%s' in passwd file", user);
              return FALSE;
            }

          uid = passwdent->uid;
        }

      if (!g_str_equal (group, "root"))
        {
          struct conv_group_ent *groupent =
            g_hash_table_lookup (groupents, group);

          if (!groupent)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not find group '%s' in group file",
                           group);
              return FALSE;
            }

          gid = groupent->gid;
        }

      if (fchownat (tmprootfs_dfd, fn, uid, gid, AT_SYMLINK_NOFOLLOW) != 0)
        {
          glnx_set_prefix_error_from_errno (error, "fchownat: %s", fn);
          return FALSE;
        }

      /* the chown clears away file caps, so reapply it here */
      if (fcaps[0] != '\0')
        {
          g_autoptr(GVariant) xattrs = rpmostree_fcap_to_xattr_variant (fcaps);
          if (!glnx_dfd_name_set_all_xattrs (tmprootfs_dfd, fn, xattrs,
                                             cancellable, error))
            return FALSE;
        }

      /* also reapply chmod since e.g. at least the setuid gets taken off */
      if (S_ISREG (stbuf.st_mode))
        {
          if (fchmodat (tmprootfs_dfd, fn, stbuf.st_mode, 0) != 0)
            {
              glnx_set_prefix_error_from_errno (error, "fchmodat: %s", fn);
              return FALSE;
            }
        }
    }

  return TRUE;
}

/* FIXME: This is a copy of ot_admin_checksum_version */
static char *
checksum_version (GVariant *checksum)
{
  g_autoptr(GVariant) metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

gboolean
rpmostree_context_assemble_tmprootfs (RpmOstreeContext      *self,
                                      int                    tmprootfs_dfd,
                                      OstreeRepoDevInoCache *devino_cache,
                                      RpmOstreeAssembleType  assemble_type,
                                      gboolean               noscripts,
                                      GCancellable          *cancellable,
                                      GError               **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  DnfContext *hifctx = self->hifctx;
  TransactionData tdata = { 0, -1 };
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, (GDestroyNotify)g_free);
  DnfPackage *filesystem_package = NULL;   /* It's special... */

  g_auto(rpmts) ordering_ts = rpmtsCreate ();
  rpmtsSetRootDir (ordering_ts, dnf_context_get_install_root (hifctx));
  /* First for the ordering TS, set the dbpath to relative, which will also gain
   * the root dir.
   */
  set_rpm_macro_define ("_dbpath", "/usr/share/rpm");

  /* Don't verify checksums here (we should have done this on ostree
   * import).  Also, avoid updating the database or anything by
   * flagging it as a test.  We'll do the database next.
   */
  rpmtsSetVSFlags (ordering_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS | RPMTRANS_FLAG_TEST);

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */
  { g_autoptr(GPtrArray) package_list = NULL;

    package_list = dnf_goal_get_packages (dnf_context_get_goal (hifctx),
                                          DNF_PACKAGE_INFO_INSTALL,
                                          -1);

    if (package_list->len == 0)
      return glnx_throw (error, "No packages in installation set");

    for (guint i = 0; i < package_list->len; i++)
      {
        DnfPackage *pkg = package_list->pdata[i];
        g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
        g_autofree char *cached_rev = NULL;
        gboolean sepolicy_matches;

        if (!ostree_repo_resolve_rev (pkgcache_repo, cachebranch, FALSE,
                                      &cached_rev, error))
          return FALSE;

        if (self->sepolicy)
          {
            if (!commit_has_matching_sepolicy (pkgcache_repo, cached_rev,
                                               self->sepolicy, &sepolicy_matches,
                                               error))
              return FALSE;

            /* We already did any relabeling/reimporting above */
            g_assert (sepolicy_matches);
          }

        if (!checkout_pkg_metadata_by_dnfpkg (self, pkg, cancellable, error))
          return FALSE;

        if (!add_to_transaction (ordering_ts, pkg, self->metadata_dir_fd,
                                 noscripts, self->ignore_scripts,
                                 cancellable, error))
          return FALSE;

        g_hash_table_insert (pkg_to_ostree_commit, g_object_ref (pkg), g_steal_pointer (&cached_rev));

        if (strcmp (dnf_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = g_object_ref (pkg);
      }
  }

  { g_auto(RpmSighandlerResetCleanup) rpmsigreset = { 0, };
    rpmtsOrder (ordering_ts);
  }

  rpmostree_output_task_begin ("Overlaying");

  guint n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);

  /* Okay so what's going on in Fedora with incestuous relationship
   * between the `filesystem`, `setup`, `libgcc` RPMs is actively
   * ridiculous.  If we unpack libgcc first it writes to /lib64 which
   * is really /usr/lib64, then filesystem blows up since it wants to symlink
   * /lib64 -> /usr/lib64.
   *
   * Really `filesystem` should be first but it depends on `setup` for
   * stupid reasons which is hacked around in `%pretrans` which we
   * don't run.  Just forcibly unpack it first.
   */
  if (filesystem_package)
    {
      if (!checkout_package_into_root (self, filesystem_package,
                                       tmprootfs_dfd, ".", devino_cache,
                                       g_hash_table_lookup (pkg_to_ostree_commit,
                                                            filesystem_package),
                                       cancellable, error))
        return FALSE;
    }
  else
    {
      /* Otherwise, we unpack the first package to get the initial
       * rootfs dir.
       */
      rpmte te = rpmtsElement (ordering_ts, 0);
      DnfPackage *pkg = (void*)rpmteKey (te);

      g_assert (pkg);

      if (!checkout_package_into_root (self, pkg,
                                       tmprootfs_dfd, ".", devino_cache,
                                       g_hash_table_lookup (pkg_to_ostree_commit,
                                                            pkg),
                                       cancellable, error))
        return FALSE;

      filesystem_package = g_object_ref (pkg);
    }

  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      DnfPackage *pkg = (void*)rpmteKey (te);

      g_assert (pkg);

      if (pkg == filesystem_package)
        continue;

      if (!checkout_package_into_root (self, pkg,
                                       tmprootfs_dfd, ".", devino_cache,
                                       g_hash_table_lookup (pkg_to_ostree_commit,
                                                            pkg),
                                       cancellable, error))
        return FALSE;
    }

  rpmostree_output_task_end ("done");

  if (!rpmostree_rootfs_prepare_links (tmprootfs_dfd, cancellable, error))
    return FALSE;

  if (!noscripts)
    {
      gboolean have_passwd;
      gboolean have_systemctl;
      g_autoptr(GPtrArray) passwdents_ptr = NULL;
      g_autoptr(GPtrArray) groupents_ptr = NULL;

      /* since here a hash table is more appropriate than a ptr array, we'll
       * just populate the table from the ptrarray, rather than making
       * data2passwdents support both outputs */
      g_autoptr(GHashTable) passwdents = g_hash_table_new (g_str_hash,
                                                           g_str_equal);
      g_autoptr(GHashTable) groupents = g_hash_table_new (g_str_hash,
                                                          g_str_equal);

      if (!rpmostree_passwd_prepare_rpm_layering (tmprootfs_dfd,
                                                  self->passwd_dir,
                                                  &have_passwd,
                                                  cancellable,
                                                  error))
        return FALSE;

      /* Also neuter systemctl - at least glusterfs calls it
       * in %post without disallowing errors.  Anyways,
       */
      if (renameat (tmprootfs_dfd, "usr/bin/systemctl",
                    tmprootfs_dfd, "usr/bin/systemctl.rpmostreesave") < 0)
        {
          if (errno == ENOENT)
            have_systemctl = FALSE;
          else
            return glnx_throw_errno_prefix (error, "rename(usr/bin/systemctl)");
        }
      else
        {
          have_systemctl = TRUE;
          if (symlinkat ("true", tmprootfs_dfd, "usr/bin/systemctl") < 0)
            return glnx_throw_errno_prefix (error, "symlinkat(usr/bin/systemctl)");
        }

      /* We're technically deviating from RPM here by running all the %pre's
       * beforehand, rather than each package's %pre & %post in order. Though I
       * highly doubt this should cause any issues. The advantage of doing it
       * this way is that we only need to read the passwd/group files once
       * before applying the overrides, rather than after each %pre.
       */
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          DnfPackage *pkg = (void*)rpmteKey (te);

          g_assert (pkg);

          if (!run_pre_sync (self->metadata_dir_fd, tmprootfs_dfd, pkg,
                             self->ignore_scripts,
                             cancellable, error))
            return FALSE;
        }

      if (have_passwd &&
          faccessat (tmprootfs_dfd, "usr/etc/passwd", F_OK, 0) == 0)
        {
          g_autofree char *contents =
            glnx_file_get_contents_utf8_at (tmprootfs_dfd, "usr/etc/passwd",
                                            NULL, cancellable, error);
          if (!contents)
            return FALSE;

          passwdents_ptr = rpmostree_passwd_data2passwdents (contents);
          for (guint i = 0; i < passwdents_ptr->len; i++)
            {
              struct conv_passwd_ent *ent = passwdents_ptr->pdata[i];
              g_hash_table_insert (passwdents, ent->name, ent);
            }
        }

      if (have_passwd &&
          faccessat (tmprootfs_dfd, "usr/etc/group", F_OK, 0) == 0)
        {
          g_autofree char *contents =
            glnx_file_get_contents_utf8_at (tmprootfs_dfd, "usr/etc/group",
                                            NULL, cancellable, error);
          if (!contents)
            return FALSE;

          groupents_ptr = rpmostree_passwd_data2groupents (contents);
          for (guint i = 0; i < groupents_ptr->len; i++)
            {
              struct conv_group_ent *ent = groupents_ptr->pdata[i];
              g_hash_table_insert (groupents, ent->name, ent);
            }
        }

      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          DnfPackage *pkg = (void*)rpmteKey (te);

          g_assert (pkg);

          if (!apply_rpmfi_overrides (self->metadata_dir_fd, tmprootfs_dfd,
                                      pkg, passwdents, groupents,
                                      cancellable, error))
            return glnx_prefix_error (error, "While applying overrides for pkg %s: ",
                                      dnf_package_get_name (pkg));

          if (!run_posttrans_sync (self->metadata_dir_fd, tmprootfs_dfd, pkg,
                                   self->ignore_scripts,
                                   cancellable, error))
            return FALSE;
        }

      if (have_systemctl)
        {
          if (renameat (tmprootfs_dfd, "usr/bin/systemctl.rpmostreesave",
                        tmprootfs_dfd, "usr/bin/systemctl") < 0)
            return glnx_throw_errno_prefix (error, "renameat(usr/bin/systemctl)");
        }

      if (have_passwd)
        {
          if (!rpmostree_passwd_complete_rpm_layering (tmprootfs_dfd, error))
            return FALSE;
        }
    }

  g_clear_pointer (&ordering_ts, rpmtsFree);

  rpmostree_output_task_begin ("Writing rpmdb");

  if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, "usr/share/rpm", 0755,
                               cancellable, error))
    return FALSE;

  /* Now, we use the separate rpmdb ts which *doesn't* have a rootdir set,
   * because if it did rpmtsRun() would try to chroot which it won't be able to
   * if we're unprivileged, even though we're not trying to run %post scripts
   * now.
   *
   * Instead, this rpmts has the dbpath as absolute.
   */
  { g_autofree char *rpmdb_abspath = glnx_fdrel_abspath (tmprootfs_dfd,
                                                         "usr/share/rpm");

    /* if we were passed an existing tmprootfs, and that tmprootfs already has
     * an rpmdb, we have to make sure to break its hardlinks as librpm mutates
     * the db in place */
    if (!break_hardlinks_at (AT_FDCWD, rpmdb_abspath, cancellable, error))
      return FALSE;

    set_rpm_macro_define ("_dbpath", rpmdb_abspath);
  }

  g_auto(rpmts) rpmdb_ts = rpmtsCreate ();
  rpmtsSetVSFlags (rpmdb_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
  rpmtsSetFlags (rpmdb_ts, RPMTRANS_FLAG_JUSTDB);

  tdata.tmp_metadata_dfd = self->metadata_dir_fd;
  rpmtsSetNotifyCallback (rpmdb_ts, ts_callback, &tdata);

  { gpointer k;
    GHashTableIter hiter;

    g_hash_table_iter_init (&hiter, pkg_to_ostree_commit);
    while (g_hash_table_iter_next (&hiter, &k, NULL))
      {
        DnfPackage *pkg = k;

        /* Set noscripts since we already validated them above */
        if (!add_to_transaction (rpmdb_ts, pkg, self->metadata_dir_fd, TRUE, NULL,
                                 cancellable, error))
          return FALSE;
      }
  }

  rpmtsOrder (rpmdb_ts);

  /* NB: Because we're using the real root here (see above for reason why), rpm
   * will see the read-only /usr mount and think that there isn't any disk space
   * available for install. For now, we just tell rpm to ignore space
   * calculations, but then we lose that nice check. What we could do is set a
   * root dir at least if we have CAP_SYS_CHROOT, or maybe do the space req
   * check ourselves if rpm makes that information easily accessible (doesn't
   * look like it from a quick glance). */
  int r = rpmtsRun (rpmdb_ts, NULL, RPMPROB_FILTER_DISKSPACE);
  if (r < 0)
    return glnx_throw (error, "Failed to update rpmdb (rpmtsRun code %d)", r);
  if (r > 0)
    {
      if (!dnf_rpmts_look_for_problems (rpmdb_ts, error))
        return FALSE;
    }

  rpmostree_output_task_end ("done");

  return TRUE;
}

gboolean
rpmostree_context_commit_tmprootfs (RpmOstreeContext      *self,
                                    int                    tmprootfs_dfd,
                                    OstreeRepoDevInoCache *devino_cache,
                                    const char            *parent,
                                    RpmOstreeAssembleType  assemble_type,
                                    char                 **out_commit,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  g_autoptr(OstreeRepoCommitModifier) commit_modifier = NULL;
  g_autofree char *ret_commit_checksum = NULL;

  rpmostree_output_task_begin ("Writing OSTree commit");

  if (!ostree_repo_prepare_transaction (self->ostreerepo, NULL, cancellable, error))
    return FALSE;

  { glnx_unref_object OstreeMutableTree *mtree = NULL;
    g_autoptr(GFile) root = NULL;
    g_auto(GVariantBuilder) metadata_builder;
    g_autofree char *state_checksum = NULL;

    g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

    if (assemble_type == RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING)
      {
        g_autoptr(GVariant) commit = NULL;
        g_autofree char *parent_version = NULL;

        g_assert (parent != NULL);

        if (!ostree_repo_load_commit (self->ostreerepo, parent, &commit, NULL, error))
          return FALSE;

        parent_version = checksum_version (commit);

        /* copy the version tag */
        if (parent_version)
          g_variant_builder_add (&metadata_builder, "{sv}", "version",
                                 g_variant_new_string (parent_version));

        /* Flag the commit as a client layer */
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.clientlayer",
                               g_variant_new_boolean (TRUE));

        if (!self->empty)
          {
            g_autoptr(GVariant) pkgs =
              g_variant_dict_lookup_value (self->spec->dict, "packages",
                                           G_VARIANT_TYPE ("as"));
            g_assert (pkgs);
            g_variant_builder_add (&metadata_builder, "{sv}",
                                   "rpmostree.packages", pkgs);
          }
        else
          {
            const char *const p[] = { NULL };
            g_variant_builder_add (&metadata_builder, "{sv}",
                                   "rpmostree.packages",
                                   g_variant_new_strv (p, -1));
          }

        /* be nice to our future selves */
        g_variant_builder_add (&metadata_builder, "{sv}",
                               "rpmostree.clientlayer_version",
                               g_variant_new_uint32 (1));
      }
    else if (assemble_type == RPMOSTREE_ASSEMBLE_TYPE_SERVER_BASE)
      {
        g_autoptr(GVariant) spec_v =
          g_variant_ref_sink (rpmostree_treespec_to_variant (self->spec));

        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.spec",
                              spec_v);

        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.serverbase",
                               g_variant_new_uint32 (1));
      }
    else
      {
        g_assert_not_reached ();
      }

    state_checksum = rpmostree_context_get_state_sha512 (self);

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.state-sha512",
                           g_variant_new_string (state_checksum));

    commit_modifier =
      ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE,
                                       NULL, NULL, NULL);

    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

    mtree = ostree_mutable_tree_new ();

    if (!ostree_repo_write_dfd_to_mtree (self->ostreerepo, tmprootfs_dfd, ".",
                                         mtree, commit_modifier,
                                         cancellable, error))
      return FALSE;

    if (!ostree_repo_write_mtree (self->ostreerepo, mtree, &root, cancellable, error))
      return FALSE;

    { g_autoptr(GVariant) metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
      if (!ostree_repo_write_commit (self->ostreerepo, parent, "", "",
                                     metadata,
                                     OSTREE_REPO_FILE (root),
                                     &ret_commit_checksum, cancellable, error))
      return FALSE;
    }

    { const char * ref = rpmostree_treespec_get_ref (self->spec);
      if (ref != NULL)
        ostree_repo_transaction_set_ref (self->ostreerepo, NULL, ref,
                                         ret_commit_checksum);
    }

    { OstreeRepoTransactionStats stats;
      g_autofree char *bytes_written_formatted = NULL;

      if (!ostree_repo_commit_transaction (self->ostreerepo, &stats, cancellable, error))
        return FALSE;

      bytes_written_formatted = g_format_size (stats.content_bytes_written);

      /* TODO: abstract a variant of this into libglnx which does
       *
       * if (journal)
       *   journal_send ()
       * else
       *   print ()
       *
       * Or possibly:
       *
       * if (journal)
       *   journal_send ()
       * if (!journal || getppid () != 1)
       *   print ()
       *
       * https://github.com/projectatomic/rpm-ostree/pull/661
       */
      sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_COMMIT_STATS),
                       "MESSAGE=Wrote commit: %s; New objects: meta:%u content:%u totaling %s)",
                       ret_commit_checksum, stats.metadata_objects_written,
                       stats.content_objects_written, bytes_written_formatted,
                       "OSTREE_METADATA_OBJECTS_WRITTEN=%u", stats.metadata_objects_written,
                       "OSTREE_CONTENT_OBJECTS_WRITTEN=%u", stats.content_objects_written,
                       "OSTREE_CONTENT_BYTES_WRITTEN=%" G_GUINT64_FORMAT, stats.content_bytes_written,
                       NULL);
    }
  }

  rpmostree_output_task_end ("done");

  if (out_commit)
    *out_commit = g_steal_pointer (&ret_commit_checksum);
  return TRUE;
}

gboolean
rpmostree_context_assemble_commit (RpmOstreeContext      *self,
                                   int                    tmprootfs_dfd,
                                   OstreeRepoDevInoCache *devino_cache,
                                   const char            *parent,
                                   RpmOstreeAssembleType  assemble_type,
                                   gboolean               noscripts,
                                   char                 **out_commit,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  g_autoptr(OstreeRepoDevInoCache) devino_owned = NULL;

  /* Auto-synthesize a cache if not provided */
  if (devino_cache == NULL)
    devino_cache = devino_owned = ostree_repo_devino_cache_new ();
  else
    devino_cache = devino_owned = ostree_repo_devino_cache_ref (devino_cache);

  if (!rpmostree_context_assemble_tmprootfs (self, tmprootfs_dfd, devino_cache,
                                             assemble_type, noscripts,
                                             cancellable, error))
    return FALSE;


  if (!rpmostree_rootfs_postprocess_common (tmprootfs_dfd, cancellable, error))
    return FALSE;

  if (!rpmostree_context_commit_tmprootfs (self, tmprootfs_dfd, devino_cache,
                                           parent, assemble_type,
                                           out_commit,
                                           cancellable, error))
    return FALSE;

  return TRUE;
}
