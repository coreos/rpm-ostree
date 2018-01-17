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

#include "rpmostree-core-private.h"
#include "rpmostree-jigdo-core.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-passwd-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-importer.h"
#include "rpmostree-output.h"

#define RPMOSTREE_MESSAGE_COMMIT_STATS SD_ID128_MAKE(e6,37,2e,38,41,21,42,a9,bc,13,b6,32,b3,f8,93,44)
#define RPMOSTREE_MESSAGE_SELINUX_RELABEL SD_ID128_MAKE(5a,e0,56,34,f2,d7,49,3b,b1,58,79,b7,0c,02,e6,5d)
#define RPMOSTREE_MESSAGE_PKG_REPOS SD_ID128_MAKE(0e,ea,67,9b,bf,a3,4d,43,80,2d,ec,99,b2,74,eb,e7)
#define RPMOSTREE_MESSAGE_PKG_IMPORT SD_ID128_MAKE(df,8b,b5,4f,04,fa,47,08,ac,16,11,1b,bf,4b,a3,52)

#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "solv"
#define RPMOSTREE_DIR_LOCK "lock"

static OstreeRepo * get_pkgcache_repo (RpmOstreeContext *self);

/* Given a string, look for ostree:// or rojig:// prefix and
 * return its type and the remainder of the string.
 */
gboolean
rpmostree_refspec_classify (const char *refspec,
                            RpmOstreeRefspecType *out_type,
                            const char **out_remainder,
                            GError     **error)
{
  if (g_str_has_prefix (refspec, RPMOSTREE_REFSPEC_OSTREE_PREFIX))
    {
      *out_type = RPMOSTREE_REFSPEC_TYPE_OSTREE;
      if (out_remainder)
        *out_remainder = refspec + strlen (RPMOSTREE_REFSPEC_OSTREE_PREFIX);
      return TRUE;
    }
  else if (g_str_has_prefix (refspec, RPMOSTREE_REFSPEC_ROJIG_PREFIX))
    {
      *out_type = RPMOSTREE_REFSPEC_TYPE_ROJIG;
      if (out_remainder)
        *out_remainder = refspec + strlen (RPMOSTREE_REFSPEC_ROJIG_PREFIX);
      return TRUE;
    }

  /* Fallback case when we have no explicit prefix - treat this as ostree://
   * for compatibility.  In the future we may do some error checking here,
   * i.e. trying to parse the refspec.
   */
  *out_type = RPMOSTREE_REFSPEC_TYPE_OSTREE;
  if (out_remainder)
    *out_remainder = refspec;

  return TRUE;
}

char*
rpmostree_refspec_to_string (RpmOstreeRefspecType  reftype,
                             const char           *data)
{
  const char *prefix = NULL;
  switch (reftype)
    {
    case RPMOSTREE_REFSPEC_TYPE_OSTREE:
      prefix = RPMOSTREE_REFSPEC_OSTREE_PREFIX;
      break;
    case RPMOSTREE_REFSPEC_TYPE_ROJIG:
      prefix = RPMOSTREE_REFSPEC_ROJIG_PREFIX;
      break;
    }
  g_assert (prefix);
  return g_strconcat (prefix, data, NULL);
}


/* Primarily this makes sure that ostree refspecs start with `ostree://`.
 */
char*
rpmostree_refspec_canonicalize (const char *orig_refspec,
                                GError    **error)
{
  RpmOstreeRefspecType refspectype;
  const char *refspec_data;
  if (!rpmostree_refspec_classify (orig_refspec, &refspectype, &refspec_data, error))
    return NULL;
  return rpmostree_refspec_to_string (refspectype, refspec_data);
}

static int
compare_pkgs (gconstpointer ap,
              gconstpointer bp)
{
  DnfPackage **a = (gpointer)ap;
  DnfPackage **b = (gpointer)bp;
  return dnf_package_cmp (*a, *b);
}

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

/* Handle converting "treespec" bits to GVariant.
 * Look for @key in @keyfile (under the section "tree"), and
 * if found, strip leading/trailing whitespace from each entry,
 * sort them, and add it to @builder under the same key name.
 * If there are no entries for it, and @notfound_key is provided,
 * set @notfound_key in the variant to TRUE.
 */
static void
add_canonicalized_string_array (GVariantBuilder *builder,
                                const char *key,
                                const char *notfound_key,
                                GKeyFile *keyfile)
{
  g_autoptr(GHashTable) set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_auto(GStrv) input = g_key_file_get_string_list (keyfile, "tree", key, NULL, NULL);
  if (!(input && *input))
    {
      if (notfound_key)
        {
          g_variant_builder_add (builder, "{sv}", notfound_key, g_variant_new_boolean (TRUE));
          return;
        }
    }

  /* Iterate over strings, strip leading/trailing whitespace */
  for (char ** iter = input; iter && *iter; iter++)
    {
      g_autofree char *stripped = g_strdup (*iter);
      g_strchug (g_strchomp (stripped));
      g_hash_table_add (set, g_steal_pointer (&stripped));
    }

  /* Ensure sorted */
  guint count;
  g_autofree char **sorted = (char**)g_hash_table_get_keys_as_array (set, &count);
  if (count > 1)
    g_qsort_with_data (sorted, count, sizeof (void*), qsort_cmpstr, NULL);

  g_variant_builder_add (builder, "{sv}", key,
                         g_variant_new_strv ((const char*const*)sorted, count));
}

static GPtrArray *
get_enabled_rpmmd_repos (DnfContext *dnfctx, DnfRepoEnabled enablement)
{
  g_autoptr(GPtrArray) ret = g_ptr_array_new ();
  GPtrArray *repos = dnf_context_get_repos (dnfctx);

  for (guint i = 0; i < repos->len; i++)
    {
      DnfRepo *repo = repos->pdata[i];
      if (dnf_repo_get_enabled (repo) & enablement)
        g_ptr_array_add (ret, repo);
    }
  return g_steal_pointer (&ret);
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

  /* See if we're using jigdo */
  { g_autofree char *jigdo = g_key_file_get_string (keyfile, "tree", "jigdo", NULL);
    if (jigdo)
      g_variant_builder_add (&builder, "{sv}", "jigdo", g_variant_new_string (jigdo));
  }

  add_canonicalized_string_array (&builder, "packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "cached-packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "removed-base-packages", NULL, keyfile);
  add_canonicalized_string_array (&builder, "cached-replaced-base-packages", NULL, keyfile);

  /* We allow the "repo" key to be missing. This means that we rely on hif's
   * normal behaviour (i.e. look at repos in repodir with enabled=1). */
  { g_auto(GStrv) val = g_key_file_get_string_list (keyfile, "tree", "repos", NULL, NULL);
    if (val && *val)
      add_canonicalized_string_array (&builder, "repos", NULL, keyfile);
  }
  add_canonicalized_string_array (&builder, "instlangs", "instlangs-all", keyfile);

  if (g_key_file_get_boolean (keyfile, "tree", "skip-sanity-check", NULL))
    g_variant_builder_add (&builder, "{sv}", "skip-sanity-check", g_variant_new_boolean (TRUE));

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
 *                    RpmOstreeContext                     *
 ***********************************************************
 */
G_DEFINE_TYPE (RpmOstreeContext, rpmostree_context, G_TYPE_OBJECT)

static void
rpmostree_context_finalize (GObject *object)
{
  RpmOstreeContext *rctx = RPMOSTREE_CONTEXT (object);

  g_clear_object (&rctx->spec);
  g_clear_object (&rctx->dnfctx);

  g_clear_object (&rctx->jigdo_pkg);
  g_free (rctx->jigdo_checksum);

  g_clear_object (&rctx->pkgcache_repo);
  g_clear_object (&rctx->ostreerepo);
  g_clear_pointer (&rctx->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&rctx->sepolicy);

  g_clear_pointer (&rctx->passwd_dir, g_free);

  g_clear_pointer (&rctx->pkgs, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_download, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_import, g_ptr_array_unref);
  g_clear_pointer (&rctx->pkgs_to_relabel, g_ptr_array_unref);

  g_clear_pointer (&rctx->pkgs_to_remove, g_hash_table_unref);
  g_clear_pointer (&rctx->pkgs_to_replace, g_hash_table_unref);

  (void)glnx_tmpdir_delete (&rctx->tmpdir, NULL, NULL);
  (void)glnx_tmpdir_delete (&rctx->repo_tmpdir, NULL, NULL);

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
  self->tmprootfs_dfd = -1;
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
rpmostree_context_new_system (OstreeRepo   *repo,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_return_val_if_fail (repo != NULL, FALSE);

  RpmOstreeContext *self = g_object_new (RPMOSTREE_TYPE_CONTEXT, NULL);
  self->ostreerepo = g_object_ref (repo);

  /* We can always be control-c'd at any time; this is new API,
   * otherwise we keep calling _rpmostree_reset_rpm_sighandlers() in
   * various places.
   */
#ifdef BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  rpmsqSetInterruptSafety (FALSE);
#endif

  self->dnfctx = dnf_context_new ();
  DECLARE_RPMSIGHANDLER_RESET;
  dnf_context_set_http_proxy (self->dnfctx, g_getenv ("http_proxy"));

  dnf_context_set_repo_dir (self->dnfctx, "/etc/yum.repos.d");
  /* TODO: re cache_age https://github.com/rpm-software-management/libdnf/issues/291
   * We override the one-week default since it's too slow for Fedora.
   */
  dnf_context_set_cache_age (self->dnfctx, 60 * 60 * 24);
  dnf_context_set_cache_dir (self->dnfctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_REPOMD);
  dnf_context_set_solv_dir (self->dnfctx, RPMOSTREE_CORE_CACHEDIR RPMOSTREE_DIR_CACHE_SOLV);
  dnf_context_set_lock_dir (self->dnfctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);
  dnf_context_set_user_agent (self->dnfctx, PACKAGE_NAME "/" PACKAGE_VERSION);

  dnf_context_set_check_disk_space (self->dnfctx, FALSE);
  dnf_context_set_check_transaction (self->dnfctx, FALSE);
  dnf_context_set_yumdb_enabled (self->dnfctx, FALSE);

  return self;
}

/* Create a context that assembles a new filesystem tree, possibly without root
 * privileges. Some behavioral distinctions are made by looking at the undelying
 * OstreeRepoMode.  In particular, an OSTREE_REPO_MODE_BARE_USER_ONLY repo
 * is assumed to be for unprivileged containers/buildroots.
 */
RpmOstreeContext *
rpmostree_context_new_tree (int               userroot_dfd,
                            OstreeRepo       *repo,
                            GCancellable     *cancellable,
                            GError          **error)
{
  g_autoptr(RpmOstreeContext) ret = rpmostree_context_new_system (repo, cancellable, error);
  if (!ret)
    return NULL;

  { g_autofree char *reposdir = glnx_fdrel_abspath (userroot_dfd, "rpmmd.repos.d");
    dnf_context_set_repo_dir (ret->dnfctx, reposdir);
  }
  { const char *cache_rpmmd = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_REPOMD);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_rpmmd);
    dnf_context_set_cache_dir (ret->dnfctx, cachedir);
  }
  { const char *cache_solv = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_SOLV);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_solv);
    dnf_context_set_solv_dir (ret->dnfctx, cachedir);
  }
  { const char *lock = glnx_strjoina ("cache/", RPMOSTREE_DIR_LOCK);
    g_autofree char *lockdir = glnx_fdrel_abspath (userroot_dfd, lock);
    dnf_context_set_lock_dir (ret->dnfctx, lockdir);
  }

  return g_steal_pointer (&ret);
}

static gboolean
rpmostree_context_ensure_tmpdir (RpmOstreeContext *self,
                                 const char       *subdir,
                                 GError          **error)
{
  if (!self->tmpdir.initialized)
    {
      if (!glnx_mkdtemp ("rpmostree-core-XXXXXX", 0700, &self->tmpdir, error))
        return FALSE;
    }
  g_assert (self->tmpdir.initialized);

  if (!glnx_shutil_mkdir_p_at (self->tmpdir.fd, subdir, 0755, NULL, error))
    return FALSE;

  return TRUE;
}

void
rpmostree_context_set_pkgcache_only (RpmOstreeContext *self,
                                     gboolean          pkgcache_only)
{
  /* if called, must always be before setup() */
  g_assert (!self->spec);
  self->pkgcache_only = pkgcache_only;
}

/* Pick up repos dir and passwd from @cfg_deployment. */
void
rpmostree_context_configure_from_deployment (RpmOstreeContext *self,
                                             OstreeSysroot    *sysroot,
                                             OstreeDeployment *cfg_deployment)
{
  g_autofree char *cfg_deployment_root =
    rpmostree_get_deployment_root (sysroot, cfg_deployment);
  g_autofree char *reposdir =
    g_build_filename (cfg_deployment_root, "etc/yum.repos.d", NULL);

  /* point libhif to the yum.repos.d and os-release of the merge deployment */
  dnf_context_set_repo_dir (self->dnfctx, reposdir);

  /* point the core to the passwd & group of the merge deployment */
  g_assert (!self->passwd_dir);
  self->passwd_dir = g_build_filename (cfg_deployment_root, "etc", NULL);
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
rpmostree_context_set_devino_cache (RpmOstreeContext *self,
                                    OstreeRepoDevInoCache *devino_cache)
{
  if (self->devino_cache)
    ostree_repo_devino_cache_unref (self->devino_cache);
  self->devino_cache = devino_cache ? ostree_repo_devino_cache_ref (devino_cache) : NULL;
}

DnfContext *
rpmostree_context_get_dnf (RpmOstreeContext *self)
{
  return self->dnfctx;
}

/* Add rpmmd repo information, since it's very useful for determining
 * state.  See also:
 *
 * - https://github.com/projectatomic/rpm-ostree/issues/774
 * - The repo_metadata_for_package() function in rpmostree-unpacker.c

 * This returns a value of key of type aa{sv} - the key should be
 * `rpmostree.rpmmd-repos`.
 */
GVariant *
rpmostree_context_get_rpmmd_repo_commit_metadata (RpmOstreeContext  *self)
{
  g_auto(GVariantBuilder) repo_list_builder;
  g_variant_builder_init (&repo_list_builder, (GVariantType*)"aa{sv}");
  g_autoptr(GPtrArray) repos = get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
  for (guint i = 0; i < repos->len; i++)
    {
      g_auto(GVariantBuilder) repo_builder;
      g_variant_builder_init (&repo_builder, (GVariantType*)"a{sv}");
      DnfRepo *repo = repos->pdata[i];
      const char *id = dnf_repo_get_id (repo);
     g_variant_builder_add (&repo_builder, "{sv}", "id", g_variant_new_string (id));
      guint64 ts = dnf_repo_get_timestamp_generated (repo);
      g_variant_builder_add (&repo_builder, "{sv}", "timestamp", g_variant_new_uint64 (ts));
      g_variant_builder_add (&repo_list_builder, "@a{sv}", g_variant_builder_end (&repo_builder));
    }
  return g_variant_ref_sink (g_variant_builder_end (&repo_list_builder));
}

GHashTable *
rpmostree_dnfcontext_get_varsubsts (DnfContext *context)
{
  GHashTable *r = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_insert (r, g_strdup ("basearch"), g_strdup (dnf_context_get_base_arch (context)));

  return r;
}

/* Look for a repo named @reponame, and ensure it is enabled for package
 * downloads.
 */
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

/* Enable all repos in @enabled_repos, and disable everything else */
static gboolean
context_repos_enable_only (RpmOstreeContext    *context,
                           const char    *const *enabled_repos,
                           GError       **error)
{
  GPtrArray *sources = dnf_context_get_repos (context->dnfctx);
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

/* Wraps `dnf_context_setup()`, and initializes state based on the treespec
 * @spec. Another way to say it is we pair `DnfContext` with an
 * `RpmOstreeTreespec`. For example, we handle "instlangs", set the rpmdb root
 * to `/usr/share/rpm`, and handle the required rpm-md repos. The "packages"
 * entry from @spec is processed later.
 *
 * If either @install_root or @source_root are `NULL`, `/usr/share/empty` is
 * used. Typically @source_root being `NULL` is for "from scratch" root
 * filesystems.
 */
gboolean
rpmostree_context_setup (RpmOstreeContext    *self,
                         const char    *install_root,
                         const char    *source_root,
                         RpmOstreeTreespec *spec,
                         GCancellable  *cancellable,
                         GError       **error)
{
  const char *releasever = NULL;
  g_autofree char **enabled_repos = NULL;
  g_autofree char **instlangs = NULL;
  /* This exists (as a canonically empty dir) at least on RHEL7+ */
  static const char emptydir_path[] = "/usr/share/empty";

  /* allow NULL for treespec, but canonicalize to an empty keyfile for cleaner queries */
  if (!spec)
    {
      g_autoptr(GKeyFile) kf = g_key_file_new ();
      self->spec = rpmostree_treespec_new_from_keyfile (kf, NULL);
    }
  else
    self->spec = g_object_ref (spec);

  g_variant_dict_lookup (self->spec->dict, "releasever", "&s", &releasever);

  if (!install_root)
    install_root = emptydir_path;
  if (!source_root)
    {
      source_root = emptydir_path;
      /* Hackaround libdnf's auto-introspection semantics. For e.g.
       * treecompose/container, we currently require using .repo files which
       * don't reference $releasever.
       */
      dnf_context_set_release_ver (self->dnfctx, releasever ?: "rpmostree-unset-releasever");
    }
  else if (releasever)
    dnf_context_set_release_ver (self->dnfctx, releasever);

  dnf_context_set_install_root (self->dnfctx, install_root);
  dnf_context_set_source_root (self->dnfctx, source_root);

  /* Set the RPM _install_langs macro, which gets processed by librpm; this is
   * currently only referenced in the traditional or non-"unified core" code.
   */
  if (g_variant_dict_lookup (self->spec->dict, "instlangs", "^a&s", &instlangs))
    {
      g_autoptr(GString) opt = g_string_new ("");

      gboolean first = TRUE;
      for (char **iter = instlangs; iter && *iter; iter++)
        {
          const char *v = *iter;
          if (!first)
            g_string_append_c (opt, ':');
          else
            first = FALSE;
          g_string_append (opt, v);
        }

      dnf_context_set_rpm_macro (self->dnfctx, "_install_langs", opt->str);
    }

  /* This is what we use as default. */
  dnf_context_set_rpm_macro (self->dnfctx, "_dbpath", "/" RPMOSTREE_RPMDB_LOCATION);

  if (!dnf_context_setup (self->dnfctx, cancellable, error))
    return FALSE;

  /* disable all repos in pkgcache-only mode, otherwise obey "repos" key */
  if (self->pkgcache_only)
    {
      if (!context_repos_enable_only (self, NULL, error))
        return FALSE;
    }
  else
    {
      /* NB: missing "repos" --> let hif figure it out for itself */
      if (g_variant_dict_lookup (self->spec->dict, "repos", "^a&s", &enabled_repos))
        if (!context_repos_enable_only (self, (const char *const*)enabled_repos, error))
          return FALSE;
    }

  g_autoptr(GPtrArray) repos = get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
  if (repos->len == 0 && !self->pkgcache_only)
    {
      /* To be nice, let's only make this fatal if "packages" is empty (e.g. if
       * we're only installing local RPMs. Missing deps will cause the regular
       * 'not found' error from libdnf. */
      g_autofree char **pkgs = NULL;
      if (g_variant_dict_lookup (self->spec->dict, "packages", "^a&s", &pkgs) &&
          g_strv_length (pkgs) > 0)
        return glnx_throw (error, "No enabled repositories");
    }

  /* Keep a handy pointer to the jigdo source if specified, since it influences
   * a lot of things here.
   */
  g_variant_dict_lookup (self->spec->dict, "jigdo", "&s", &self->jigdo_spec);

  /* Ensure that each repo that's enabled is marked as required; this should be
   * the default, but we make sure.  This is a bit of a messy topic, but for
   * rpm-ostree we're being more strict about requiring repos.
   */
  for (guint i = 0; i < repos->len; i++)
    dnf_repo_set_required (repos->pdata[i], TRUE);

  { gboolean docs;

    g_assert (g_variant_dict_lookup (self->spec->dict, "documentation", "b", &docs));

    if (!docs)
        dnf_transaction_set_flags (dnf_context_get_transaction (self->dnfctx),
                                   DNF_TRANSACTION_FLAG_NODOCS);
  }

  /* We could likely delete this, but I'm keeping a log message just in case */
  if (g_variant_dict_contains (self->spec->dict, "ignore-scripts"))
    sd_journal_print (LOG_INFO, "ignore-scripts is no longer supported");

  return TRUE;
}

static void
on_hifstate_percentage_changed (DnfState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  rpmostree_output_progress_percent (text, percentage);
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
  return g_strdup_printf ("metarpm/%s.rpm", nevra);
}

static char *
get_package_relpath (DnfPackage *pkg)
{
  return get_nevra_relpath (dnf_package_get_nevra (pkg));
}

/* We maintain a temporary copy on disk of the RPM header value; librpm will
 * load this dynamically as the txn progresses.
 */
static gboolean
checkout_pkg_metadata (RpmOstreeContext *self,
                       const char       *nevra,
                       GVariant         *header,
                       GCancellable     *cancellable,
                       GError          **error)
{
  if (!rpmostree_context_ensure_tmpdir (self, "metarpm", error))
    return FALSE;

  /* give it a .rpm extension so we can fool the libdnf stack */
  g_autofree char *path = get_nevra_relpath (nevra);

  if (!glnx_fstatat_allow_noent (self->tmpdir.fd, path, NULL, 0, error))
    return FALSE;
  /* we may have already written the header out for this one */
  if (errno == 0)
    return TRUE;

  return glnx_file_replace_contents_at (self->tmpdir.fd, path,
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

  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &cached_rev, error))
    return FALSE;

  g_autoptr(GVariant) pkg_commit = NULL;
  if (!ostree_repo_load_commit (repo, cached_rev, &pkg_commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) pkg_meta = g_variant_get_child_value (pkg_commit, 0);
  g_autoptr(GVariantDict) pkg_meta_dict = g_variant_dict_new (pkg_meta);

  g_autoptr(GVariant) header =
    _rpmostree_vardict_lookup_value_required (pkg_meta_dict, "rpmostree.metadata",
                                              (GVariantType*)"ay", error);
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
rpmostree_find_cache_branch_by_nevra (OstreeRepo    *pkgcache,
                                      const char    *nevra,
                                      char         **out_cache_branch,
                                      GCancellable  *cancellable,
                                      GError       **error)

{
  /* there's no safe way to convert a nevra string to its cache branch, so let's
   * just do a dumb lookup */
  /* TODO: parse the refs once at core init, this function is itself
   * called in loops */

  g_autoptr(GHashTable) refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache, "rpmostree/pkg", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH (refs, const char*, ref)
    {
      g_autofree char *cache_nevra = rpmostree_cache_branch_to_nevra (ref);
      if (g_str_equal (nevra, cache_nevra))
        {
          *out_cache_branch = g_strdup (ref);
          return TRUE;
        }
    }

  return glnx_throw (error, "Failed to find cached pkg for %s", nevra);
}

gboolean
rpmostree_pkgcache_find_pkg_header (OstreeRepo    *pkgcache,
                                    const char    *nevra,
                                    const char    *expected_sha256,
                                    GVariant     **out_header,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  g_autofree char *cache_branch = NULL;
  if (!rpmostree_find_cache_branch_by_nevra (pkgcache, nevra, &cache_branch,
                                             cancellable, error))
    return FALSE;

  if (expected_sha256 != NULL)
    {
      g_autofree char *commit_csum = NULL;
      g_autoptr(GVariant) commit = NULL;
      g_autofree char *actual_sha256 = NULL;

      if (!ostree_repo_resolve_rev (pkgcache, cache_branch, TRUE, &commit_csum, error))
        return FALSE;

      if (!ostree_repo_load_commit (pkgcache, commit_csum, &commit, NULL, error))
        return FALSE;

      if (!get_commit_header_sha256 (commit, &actual_sha256, error))
        return FALSE;

      if (!g_str_equal (expected_sha256, actual_sha256))
        return glnx_throw (error, "Checksum mismatch for package %s", nevra);
    }

  return get_header_variant (pkgcache, cache_branch, out_header, cancellable, error);
}

static gboolean
checkout_pkg_metadata_by_nevra (RpmOstreeContext *self,
                                const char       *nevra,
                                const char       *sha256,
                                GCancellable     *cancellable,
                                GError          **error)
{
  g_autoptr(GVariant) header = NULL;

  if (!rpmostree_pkgcache_find_pkg_header (get_pkgcache_repo (self), nevra, sha256,
                                           &header, cancellable, error))
    return FALSE;

  return checkout_pkg_metadata (self, nevra, header, cancellable, error);
}

/* Fetches decomposed NEVRA information from pkgcache for a given nevra string. Requires the
 * package to have been unpacked with unpack_version 1.4+ */
gboolean
rpmostree_get_nevra_from_pkgcache (OstreeRepo  *repo,
                                   const char  *nevra,
                                   char       **out_name,
                                   guint64     *out_epoch,
                                   char       **out_version,
                                   char       **out_release,
                                   char       **out_arch,
                                   GCancellable *cancellable,
                                   GError  **error)
{
  g_autofree char *ref = NULL;
  if (!rpmostree_find_cache_branch_by_nevra (repo, nevra, &ref, cancellable, error))
    return FALSE;

  g_autofree char *rev = NULL;
  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &rev, error))
    return FALSE;

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, rev, &commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) dict = g_variant_dict_new (meta);

  g_autofree char *actual_nevra = NULL;
  if (!g_variant_dict_lookup (dict, "rpmostree.nevra", "(sstsss)", &actual_nevra,
                              out_name, out_epoch, out_version, out_release, out_arch))
    return glnx_throw (error, "Cannot get nevra variant from commit metadata");

  g_assert (g_str_equal (nevra, actual_nevra));
  return TRUE;
}

/* Initiate download of rpm-md */
gboolean
rpmostree_context_download_metadata (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  g_assert (!self->empty);

  g_autoptr(GPtrArray) rpmmd_repos =
    get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);

  if (self->pkgcache_only)
    {
      g_assert_cmpint (rpmmd_repos->len, ==, 0);

      /* this is essentially a no-op */
      g_autoptr(DnfState) hifstate = dnf_state_new ();
      if (!dnf_context_setup_sack (self->dnfctx, hifstate, error))
        return FALSE;

      /* Note early return; no repos to fetch. */
      return TRUE;
    }

  g_autoptr(GString) enabled_repos = g_string_new ("Enabled rpm-md repositories:");
  for (guint i = 0; i < rpmmd_repos->len; i++)
    {
      DnfRepo *repo = rpmmd_repos->pdata[i];
      g_string_append_printf (enabled_repos, " %s", dnf_repo_get_id (repo));
    }
  rpmostree_output_message ("%s", enabled_repos->str);

  /* Update each repo individually, and print its timestamp, so users can keep
   * track of repo up-to-dateness more easily.
   */
  for (guint i = 0; i < rpmmd_repos->len; i++)
    {
      DnfRepo *repo = rpmmd_repos->pdata[i];
      g_autoptr(DnfState) hifstate = dnf_state_new ();

      /* Until libdnf speaks GCancellable: https://github.com/projectatomic/rpm-ostree/issues/897 */
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      gboolean did_update = FALSE;
      if (!dnf_repo_check(repo,
                          dnf_context_get_cache_age (self->dnfctx),
                          hifstate,
                          NULL))
        {
          dnf_state_reset (hifstate);
          g_autofree char *prefix = g_strdup_printf ("Updating metadata for '%s':",
                                                     dnf_repo_get_id (repo));
          guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                                   G_CALLBACK (on_hifstate_percentage_changed),
                                                   prefix);

          if (!dnf_repo_update (repo, DNF_REPO_UPDATE_FLAG_FORCE, hifstate, error))
            return FALSE;

          did_update = TRUE;

          g_signal_handler_disconnect (hifstate, progress_sigid);
          rpmostree_output_progress_end ();
        }

      guint64 ts = dnf_repo_get_timestamp_generated (repo);
      g_autoptr(GDateTime) repo_ts = g_date_time_new_from_unix_utc (ts);
      g_autofree char *repo_ts_str = NULL;

      if (repo_ts != NULL)
        repo_ts_str = g_date_time_format (repo_ts, "%Y-%m-%d %T");
      else
        repo_ts_str = g_strdup_printf ("(invalid timestamp)");

      rpmostree_output_message ("rpm-md repo '%s'%s; generated: %s",
                                dnf_repo_get_id (repo), !did_update ? " (cached)" : "",
                                repo_ts_str);
    }

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* The _setup_sack function among other things imports the metadata into libsolv */
  { g_autoptr(DnfState) hifstate = dnf_state_new ();
    guint progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                             G_CALLBACK (on_hifstate_percentage_changed),
                                             "Importing metadata");
    /* This will check the metadata again, but it *should* hit the cache; down
     * the line we should really improve the libdnf API around all of this.
     */
    DECLARE_RPMSIGHANDLER_RESET;
    if (!dnf_context_setup_sack (self->dnfctx, hifstate, error))
      return FALSE;
    g_signal_handler_disconnect (hifstate, progress_sigid);
    rpmostree_output_progress_end ();
  }

  /* A lot of code to simply log a message to the systemd journal with the state
   * of the rpm-md repos. This is intended to aid system admins with determining
   * system ""up-to-dateness"".
   */
  { g_autoptr(GPtrArray) repos = get_enabled_rpmmd_repos (self->dnfctx, DNF_REPO_ENABLED_PACKAGES);
    g_autoptr(GString) enabled_repos = g_string_new ("");
    g_autoptr(GString) enabled_repos_solvables = g_string_new ("");
    g_autoptr(GString) enabled_repos_timestamps = g_string_new ("");
    guint total_solvables = 0;
    gboolean first = TRUE;

    for (guint i = 0; i < repos->len; i++)
      {
        DnfRepo *repo = repos->pdata[i];

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
                     "SACK_N_SOLVABLES=%i", dnf_sack_count (dnf_context_get_sack (self->dnfctx)),
                     "ENABLED_REPOS=[%s]", enabled_repos->str,
                     "ENABLED_REPOS_SOLVABLES=[%s]", enabled_repos_solvables->str,
                     "ENABLED_REPOS_TIMESTAMPS=[%s]", enabled_repos_timestamps->str,
                     NULL);
  }

  return TRUE;
}

/* Quote/squash all non-(alphanumeric plus `.` and `-`); this
 * is used to map RPM package NEVRAs into ostree branch names.
 */
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

/* Return the ostree cache branch for a nevra */
char *
rpmostree_get_cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch)
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

/* Return the ostree cache branch from a Header */
char *
rpmostree_get_cache_branch_header (Header hdr)
{
  g_autofree char *name = headerGetAsString (hdr, RPMTAG_NAME);
  g_autofree char *evr = headerGetAsString (hdr, RPMTAG_EVR);
  g_autofree char *arch = headerGetAsString (hdr, RPMTAG_ARCH);
  return rpmostree_get_cache_branch_for_n_evr_a (name, evr, arch);
}

/* Return the ostree cache branch from a libdnf Package */
char *
rpmostree_get_cache_branch_pkg (DnfPackage *pkg)
{
  return rpmostree_get_cache_branch_for_n_evr_a (dnf_package_get_name (pkg),
                                                 dnf_package_get_evr (pkg),
                                                 dnf_package_get_arch (pkg));
}

static gboolean
commit_has_matching_sepolicy (GVariant       *commit,
                              OstreeSePolicy *sepolicy,
                              gboolean       *out_matches,
                              GError        **error)
{
  const char *sepolicy_csum_wanted = ostree_sepolicy_get_csum (sepolicy);
  g_autofree char *sepolicy_csum = NULL;

  if (!get_commit_sepolicy_csum (commit, &sepolicy_csum, error))
    return FALSE;

  *out_matches = g_str_equal (sepolicy_csum, sepolicy_csum_wanted);

  return TRUE;
}

static gboolean
get_pkgcache_repodata_chksum_repr (GVariant    *commit,
                                   char       **out_chksum_repr,
                                   gboolean     allow_noent,
                                   GError     **error)
{
  g_autofree char *chksum_repr = NULL;
  g_autoptr(GError) tmp_error = NULL;
  if (!get_commit_repodata_chksum_repr (commit, &chksum_repr, &tmp_error))
    {
      if (g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && allow_noent)
        {
          *out_chksum_repr = NULL;
          return TRUE;
        }
      g_propagate_error (error, g_steal_pointer (&tmp_error));
      return FALSE;
    }

  *out_chksum_repr = g_steal_pointer (&chksum_repr);
  return TRUE;
}

static gboolean
commit_has_matching_repodata_chksum_repr (GVariant    *commit,
                                          const char  *expected,
                                          gboolean    *out_matches,
                                          GError     **error)
{
  g_autofree char *chksum_repr = NULL;
  if (!get_pkgcache_repodata_chksum_repr (commit, &chksum_repr, TRUE, error))
    return FALSE;

  /* never match pkgs unpacked with older versions that didn't embed chksum_repr */
  if (chksum_repr == NULL)
    {
      *out_matches = FALSE;
      return TRUE;
    }

  *out_matches = g_str_equal (expected, chksum_repr);
  return TRUE;
}

static gboolean
pkg_is_cached (DnfPackage *pkg)
{
  if (rpmostree_pkg_is_local (pkg))
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

/* Given @pkg, return its state in the pkgcache repo. It could be not present,
 * or present but have been imported with a different SELinux policy version
 * (and hence in need of relabeling).
 */
static gboolean
find_pkg_in_ostree (RpmOstreeContext *self,
                    DnfPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    gboolean       *out_in_ostree,
                    gboolean       *out_selinux_match,
                    GError        **error)
{
  OstreeRepo *repo = get_pkgcache_repo (self);
  /* Init output here, since we have several early returns */
  *out_in_ostree = FALSE;
  /* If there's no sepolicy, then we always match */
  *out_selinux_match = (sepolicy == NULL);

  /* NB: we're not using a pkgcache yet in the compose path */
  if (repo == NULL)
    return TRUE; /* Note early return */

  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  g_autofree char *cached_rev = NULL;
  if (!ostree_repo_resolve_rev (repo, cachebranch, TRUE,
                                &cached_rev, error))
    return FALSE;

  if (!cached_rev)
    return TRUE; /* Note early return */

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, cached_rev, &commit, NULL, error))
    return FALSE;
  g_assert (commit);
  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) metadata_dict = g_variant_dict_new (metadata);

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
      if (!commit_has_matching_repodata_chksum_repr (commit,
                                                     expected_chksum_repr,
                                                     &same_pkg_chksum, error))
        return FALSE;

      if (!same_pkg_chksum)
        return TRUE; /* Note early return */

      /* We need to handle things like the nodocs flag changing; in that case we
       * have to redownload.
       */
      gboolean global_docs;
      g_variant_dict_lookup (self->spec->dict, "documentation", "b", &global_docs);
      const gboolean global_nodocs = !global_docs;

      gboolean pkgcache_commit_is_nodocs;
      if (!g_variant_dict_lookup (metadata_dict, "rpmostree.nodocs", "b", &pkgcache_commit_is_nodocs))
        pkgcache_commit_is_nodocs = FALSE;

      /* We treat a mismatch of documentation state as simply not being
       * imported at all.
       */
      if (global_nodocs != pkgcache_commit_is_nodocs)
        return TRUE;
    }

  /* We found an import, let's load the sepolicy state */
  *out_in_ostree = TRUE;
  if (sepolicy)
    {
      if (!commit_has_matching_sepolicy (commit, sepolicy,
                                         out_selinux_match, error))
        return FALSE;
    }

  return TRUE;
}

/* determine of all the marked packages, which ones we'll need to download,
 * which ones we'll need to import, and which ones we'll need to relabel */
static gboolean
sort_packages (RpmOstreeContext *self,
               GPtrArray        *packages,
               GCancellable     *cancellable,
               GError          **error)
{
  DnfContext *dnfctx = self->dnfctx;

  g_assert (!self->pkgs_to_download);
  self->pkgs_to_download = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  g_assert (!self->pkgs_to_import);
  self->pkgs_to_import = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  self->n_async_pkgs_imported = 0;
  g_assert (!self->pkgs_to_relabel);
  self->pkgs_to_relabel = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  self->n_async_pkgs_relabeled = 0;

  GPtrArray *sources = dnf_context_get_repos (dnfctx);
  for (guint i = 0; i < packages->len; i++)
    {
      DnfPackage *pkg = packages->pdata[i];

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

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
       * RpmOstreeContext to express *why* they are calling prepare().
       * */

      {
        gboolean in_ostree = FALSE;
        gboolean selinux_match = FALSE;
        gboolean cached = pkg_is_cached (pkg);

        if (!find_pkg_in_ostree (self, pkg, self->sepolicy,
                                 &in_ostree, &selinux_match, error))
          return FALSE;

        if (is_locally_cached)
          g_assert (in_ostree);

        if (!in_ostree && !cached)
          g_ptr_array_add (self->pkgs_to_download, g_object_ref (pkg));
        if (!in_ostree)
          g_ptr_array_add (self->pkgs_to_import, g_object_ref (pkg));
        /* This logic is equivalent to that in rpmostree_context_force_relabel() */
        if (in_ostree && !selinux_match)
          g_ptr_array_add (self->pkgs_to_relabel, g_object_ref (pkg));
      }
    }

  return TRUE;
}

/* Set @error with a string containing an error relating to @pkgs */
static gboolean
throw_package_list (GError **error, const char *suffix, GPtrArray *pkgs)
{
  if (!error)
    return FALSE; /* Note early simultaneously happy and sad return */

  g_autoptr(GString) msg = g_string_new ("The following base packages ");
  g_string_append (msg, suffix);
  g_string_append (msg, ": ");

  gboolean first = TRUE;
  for (guint i = 0; i < pkgs->len; i++)
    {
      if (!first)
        g_string_append (msg, ", ");
      g_string_append (msg, pkgs->pdata[i]);
      first = FALSE;
    }

  /* need a glnx_set_error_steal */
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg->str);
  return FALSE;
}

static GVariant*
gv_nevra_from_pkg (DnfPackage *pkg)
{
  return g_variant_ref_sink (g_variant_new ("(sstsss)",
                             dnf_package_get_nevra (pkg),
                             dnf_package_get_name (pkg),
                             dnf_package_get_epoch (pkg),
                             dnf_package_get_version (pkg),
                             dnf_package_get_release (pkg),
                             dnf_package_get_arch (pkg)));
}

/* g_variant_hash can't handle container types */
static guint
gv_nevra_hash (gconstpointer v)
{
  g_autoptr(GVariant) nevra = g_variant_get_child_value ((GVariant*)v, 0);
  return g_str_hash (g_variant_get_string (nevra, NULL));
}

/* We need to make sure that only the pkgs in the base allowed to be
 * removed are removed. The issue is that marking a package for DNF_INSTALL
 * could uninstall a base pkg if it's an update or obsoletes it. There doesn't
 * seem to be a way to tell libsolv to not touch some pkgs in the base layer,
 * so we just inspect its solution in retrospect. libdnf has the concept of
 * protected packages, but it still allows updating protected packages.
 */
static gboolean
check_goal_solution (RpmOstreeContext *self,
                     GPtrArray        *removed_pkgnames,
                     GPtrArray        *replaced_nevras,
                     GError          **error)
{
  HyGoal goal = dnf_context_get_goal (self->dnfctx);

  /* check that we're not removing anything we didn't expect */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new_with_free_func (g_free);

    /* also collect info about those we're removing */
    g_assert (!self->pkgs_to_remove);
    self->pkgs_to_remove = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify)g_variant_unref);
    g_autoptr(GPtrArray) packages = dnf_goal_get_packages (goal,
                                                           DNF_PACKAGE_INFO_REMOVE,
                                                           DNF_PACKAGE_INFO_OBSOLETE, -1);
    for (guint i = 0; i < packages->len; i++)
      {
        DnfPackage *pkg = packages->pdata[i];
        const char *name = dnf_package_get_name (pkg);
        const char *nevra = dnf_package_get_nevra (pkg);

        /* did we expect this package to be removed? */
        if (rpmostree_str_ptrarray_contains (removed_pkgnames, name))
          g_hash_table_insert (self->pkgs_to_remove, g_strdup (name),
                                                     gv_nevra_from_pkg (pkg));
        else
          g_ptr_array_add (forbidden, g_strdup (nevra));
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "would be removed", forbidden);
  }

  /* check that all the pkgs we expect to remove are marked for removal */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new ();

    for (guint i = 0; i < removed_pkgnames->len; i++)
      {
        const char *pkgname = removed_pkgnames->pdata[i];
        if (!g_hash_table_contains (self->pkgs_to_remove, pkgname))
          g_ptr_array_add (forbidden, (gpointer)pkgname);
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "are not marked to be removed", forbidden);
  }

  /* REINSTALLs should never happen since it doesn't make sense in the rpm-ostree flow, and
   * we check very early whether a package is already in the rootfs or not, but let's check
   * for it anyway so that we get a bug report in case it somehow happens. */
  { g_autoptr(GPtrArray) packages =
      dnf_goal_get_packages (goal, DNF_PACKAGE_INFO_REINSTALL, -1);
    g_assert_cmpint (packages->len, ==, 0);
  }

  /* Look at UPDATE and DOWNGRADE, and see whether they're doing what we expect */
  { g_autoptr(GHashTable) forbidden_replacements =
      g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref,
                                         (GDestroyNotify)g_object_unref);

    /* also collect info about those we're replacing */
    g_assert (!self->pkgs_to_replace);
    self->pkgs_to_replace = g_hash_table_new_full (gv_nevra_hash, g_variant_equal,
                                                   (GDestroyNotify)g_variant_unref,
                                                   (GDestroyNotify)g_variant_unref);
    g_autoptr(GPtrArray) packages = dnf_goal_get_packages (goal,
                                                           DNF_PACKAGE_INFO_UPDATE,
                                                           DNF_PACKAGE_INFO_DOWNGRADE, -1);
    for (guint i = 0; i < packages->len; i++)
      {
        DnfPackage *pkg = packages->pdata[i];
        const char *nevra = dnf_package_get_nevra (pkg);

        /* just pick the first pkg */
        g_autoptr(GPtrArray) old = hy_goal_list_obsoleted_by_package (goal, pkg);
        g_assert_cmpint (old->len, >, 0);
        DnfPackage *old_pkg = old->pdata[0];

        /* did we expect this nevra to replace a base pkg? */
        if (rpmostree_str_ptrarray_contains (replaced_nevras, nevra))
          g_hash_table_insert (self->pkgs_to_replace, gv_nevra_from_pkg (pkg),
                                                      gv_nevra_from_pkg (old_pkg));
        else
          g_hash_table_insert (forbidden_replacements, g_object_ref (old_pkg),
                                                       g_object_ref (pkg));
      }

    if (g_hash_table_size (forbidden_replacements) > 0)
      {
        rpmostree_output_message ("Forbidden base package replacements:");
        GLNX_HASH_TABLE_FOREACH_KV (forbidden_replacements, DnfPackage*, old_pkg,
                                                            DnfPackage*, new_pkg)
          {
            const char *old_name = dnf_package_get_name (old_pkg);
            const char *new_name = dnf_package_get_name (new_pkg);
            const char *new_repo = dnf_package_get_reponame (new_pkg);
            if (g_str_equal (old_name, new_name))
              rpmostree_output_message ("  %s %s -> %s (%s)", old_name,
                                        dnf_package_get_evr (old_pkg),
                                        dnf_package_get_evr (new_pkg), new_repo);
            else
              rpmostree_output_message ("  %s -> %s (%s)",
                                        dnf_package_get_nevra (old_pkg),
                                        dnf_package_get_nevra (new_pkg), new_repo);
          }

        return glnx_throw (error, "Some base packages would be replaced");
      }
  }

  /* check that all the pkgs we expect to replace are marked for replacement */
  { g_autoptr(GPtrArray) forbidden = g_ptr_array_new_with_free_func (g_free);

    GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_replace, GVariant*, new, GVariant*, old)
      {
        g_autoptr(GVariant) nevra_v = g_variant_get_child_value (new, 0);
        const char *nevra = g_variant_get_string (nevra_v, NULL);
        if (!rpmostree_str_ptrarray_contains (replaced_nevras, nevra))
          g_ptr_array_add (forbidden, g_strdup (nevra));
      }

    if (forbidden->len > 0)
      return throw_package_list (error, "are not marked to be installed", forbidden);
  }

  return TRUE;
}

static gboolean
install_pkg_from_cache (RpmOstreeContext *self,
                        const char       *nevra,
                        const char       *sha256,
                        GCancellable     *cancellable,
                        GError          **error)
{
  if (!checkout_pkg_metadata_by_nevra (self, nevra, sha256, cancellable, error))
    return FALSE;

  /* This is the great lie: we make libdnf et al. think that they're dealing with a full
   * RPM, all while crossing our fingers that they don't try to look past the header.
   * Ideally, it would be best if libdnf could learn to treat the pkgcache repo as another
   * DnfRepo. We could do this all in-memory though it doesn't seem like libsolv has an
   * appropriate API for this. */
  g_autofree char *rpm = g_strdup_printf ("%s/metarpm/%s.rpm", self->tmpdir.path, nevra);
  DnfPackage *pkg = dnf_sack_add_cmdline_package (dnf_context_get_sack (self->dnfctx), rpm);
  if (!pkg)
    return glnx_throw (error, "Failed to add local pkg %s to sack", nevra);

  hy_goal_install (dnf_context_get_goal (self->dnfctx), pkg);
  return TRUE;
}

/* This is a hacky way to bridge the gap between libdnf and our pkgcache. We extract the
 * metarpm for every RPM in our cache and present that as the cmdline repo to libdnf. But we
 * do still want all the niceties of the libdnf stack, e.g. HyGoal, libsolv depsolv, etc...
 */
static gboolean
add_remaining_pkgcache_pkgs (RpmOstreeContext *self,
                             GHashTable       *already_added,
                             GCancellable     *cancellable,
                             GError          **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_assert (pkgcache_repo);
  g_assert (self->pkgcache_only);

  DnfSack *sack = dnf_context_get_sack (self->dnfctx);
  g_assert (sack);

  g_autoptr(GHashTable) refs = NULL;
  if (!ostree_repo_list_refs_ext (pkgcache_repo, "rpmostree/pkg", &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH (refs, const char*, ref)
    {
      g_autofree char *nevra = rpmostree_cache_branch_to_nevra (ref);
      if (g_hash_table_contains (already_added, nevra))
        continue;

      g_autoptr(GVariant) header = NULL;
      if (!get_header_variant (pkgcache_repo, ref, &header, cancellable, error))
        return FALSE;

      if (!checkout_pkg_metadata (self, nevra, header, cancellable, error))
        return FALSE;

      g_autofree char *rpm = g_strdup_printf ("%s/metarpm/%s.rpm", self->tmpdir.path, nevra);
      DnfPackage *pkg = dnf_sack_add_cmdline_package (sack, rpm);
      if (!pkg)
        return glnx_throw (error, "Failed to add local pkg %s to sack", nevra);
    }

  return TRUE;
}

static gboolean
setup_jigdo_state (RpmOstreeContext *self,
                   GError          **error)
{
  g_assert (self->jigdo_spec);
  g_assert (!self->jigdo_pkg);
  g_assert (!self->jigdo_checksum);

  g_autofree char *jigdo_repoid = NULL;
  g_autofree char *jigdo_name = NULL;

  { const char *colon = strchr (self->jigdo_spec, ':');
    if (!colon)
      return glnx_throw (error, "Invalid jigdo spec '%s', expected repoid:name", self->jigdo_spec);
    jigdo_repoid = g_strndup (self->jigdo_spec, colon - self->jigdo_spec);
    jigdo_name = g_strdup (colon + 1);
  }

  const char *jigdo_version = NULL;
  g_variant_dict_lookup (self->spec->dict, "jigdo-version", "&s", &jigdo_version);

  hy_autoquery HyQuery query = hy_query_create (dnf_context_get_sack (self->dnfctx));
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, jigdo_repoid);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, jigdo_name);
  if (jigdo_version)
    hy_query_filter (query, HY_PKG_VERSION, HY_EQ, jigdo_version);

  g_autoptr(GPtrArray) pkglist = hy_query_run (query);
  if (pkglist->len == 0)
    return glnx_throw (error, "Failed to find jigdo package '%s'", self->jigdo_spec);
  g_ptr_array_sort (pkglist, compare_pkgs);
  /* We use the last package in the array which should be newest */
  self->jigdo_pkg = g_object_ref (pkglist->pdata[pkglist->len-1]);

  /* Iterate over provides directly to provide a nicer error on mismatch */
  gboolean found_vprovide = FALSE;
  g_autoptr(DnfReldepList) provides = dnf_package_get_provides (self->jigdo_pkg);
  const gint n_provides = dnf_reldep_list_count (provides);
  for (int i = 0; i < n_provides; i++)
    {
      DnfReldep *provide = dnf_reldep_list_index (provides, i);

      const char *provide_str = dnf_reldep_to_string (provide);
      if (g_str_equal (provide_str, RPMOSTREE_JIGDO_PROVIDE_V3))
        {
          found_vprovide = TRUE;
        }
      else if (g_str_has_prefix (provide_str, RPMOSTREE_JIGDO_PROVIDE_COMMIT))
        {
          const char *rest = provide_str + strlen (RPMOSTREE_JIGDO_PROVIDE_COMMIT);
          if (*rest != '(')
            return glnx_throw (error, "Invalid %s", provide_str);
          rest++;
          const char *closeparen = strchr (rest, ')');
          if (!closeparen)
            return glnx_throw (error, "Invalid %s", provide_str);

          self->jigdo_checksum = g_strndup (rest, closeparen - rest);
          if (strlen (self->jigdo_checksum) != OSTREE_SHA256_STRING_LEN)
            return glnx_throw (error, "Invalid %s", provide_str);
        }
    }

  if (!found_vprovide)
    return glnx_throw (error, "Package '%s' does not have Provides: %s",
                       dnf_package_get_nevra (self->jigdo_pkg), RPMOSTREE_JIGDO_PROVIDE_V3);
  if (!self->jigdo_checksum)
    return glnx_throw (error, "Package '%s' does not have Provides: %s",
                       dnf_package_get_nevra (self->jigdo_pkg), RPMOSTREE_JIGDO_PROVIDE_COMMIT);

  return TRUE;
}

/* Check for/download new rpm-md, then depsolve */
gboolean
rpmostree_context_prepare (RpmOstreeContext *self,
                           GCancellable     *cancellable,
                           GError          **error)
{
  g_assert (!self->empty);

  DnfContext *dnfctx = self->dnfctx;
  g_autofree char **pkgnames = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "packages",
                                   "^a&s", &pkgnames));

  g_autofree char **cached_pkgnames = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "cached-packages",
                                   "^a&s", &cached_pkgnames));

  g_autofree char **cached_replace_pkgs = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "cached-replaced-base-packages",
                                   "^a&s", &cached_replace_pkgs));

  g_autofree char **removed_base_pkgnames = NULL;
  g_assert (g_variant_dict_lookup (self->spec->dict, "removed-base-packages",
                                   "^a&s", &removed_base_pkgnames));

  /* setup sack if not yet set up */
  if (dnf_context_get_sack (dnfctx) == NULL)
    {
      if (!rpmostree_context_download_metadata (self, cancellable, error))
        return FALSE;
    }

  if (self->jigdo_pure)
    {
      g_assert_cmpint (g_strv_length (pkgnames), ==, 0);
      g_assert_cmpint (g_strv_length (cached_pkgnames), ==, 0);
      g_assert_cmpint (g_strv_length (cached_replace_pkgs), ==, 0);
      g_assert_cmpint (g_strv_length (removed_base_pkgnames), ==, 0);
    }

  /* Handle packages to remove */
  g_autoptr(GPtrArray) removed_pkgnames = g_ptr_array_new ();
  for (char **it = removed_base_pkgnames; it && *it; it++)
    {
      const char *pkgname = *it;
      g_assert (!self->jigdo_pure);
      if (!dnf_context_remove (dnfctx, pkgname, error))
        return FALSE;

      g_ptr_array_add (removed_pkgnames, (gpointer)pkgname);
    }

  /* track cached pkgs already added to the sack so far */
  g_autoptr(GHashTable) already_added = g_hash_table_new (g_str_hash, g_str_equal);

  /* Handle packages to replace */
  g_autoptr(GPtrArray) replaced_nevras = g_ptr_array_new ();
  for (char **it = cached_replace_pkgs; it && *it; it++)
    {
      const char *nevra = *it;
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_assert (!self->jigdo_pure);
      if (!install_pkg_from_cache (self, nevra, sha256, cancellable, error))
        return FALSE;

      g_ptr_array_add (replaced_nevras, (gpointer)nevra);
      g_hash_table_add (already_added, (gpointer)nevra);
    }

  /* For each new local package, tell libdnf to add it to the goal */
  for (char **it = cached_pkgnames; it && *it; it++)
    {
      const char *nevra = *it;
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_assert (!self->jigdo_pure);
      if (!install_pkg_from_cache (self, nevra, sha256, cancellable, error))
        return FALSE;

      g_hash_table_add (already_added, (gpointer)nevra);
    }

  /* If we're in cache-only mode, add all the remaining pkgs now. We do this *after* the
   * replace & local pkgs since they already handle a subset of the cached pkgs and have
   * SHA256 checks. But we do it *before* dnf_context_install() since those subjects are not
   * directly linked to a cached pkg, so we need to teach libdnf about them beforehand. */
  if (self->pkgcache_only)
    {
      if (!add_remaining_pkgcache_pkgs (self, already_added, cancellable, error))
        return FALSE;
    }

  /* Loop over each named package, and tell libdnf to add it to the goal */
  for (char **it = pkgnames; it && *it; it++)
    {
      const char *pkgname = *it;
      g_assert (!self->jigdo_pure);
      if (!dnf_context_install (dnfctx, pkgname, error))
        return FALSE;
    }

  if (self->jigdo_spec)
    {
      if (!setup_jigdo_state (self, error))
        return FALSE;
    }

  if (!self->jigdo_pure)
    {
      HyGoal goal = dnf_context_get_goal (dnfctx);

      rpmostree_output_task_begin ("Resolving dependencies");

      /* XXX: consider a --allow-uninstall switch? */
      if (!dnf_goal_depsolve (goal, DNF_INSTALL | DNF_ALLOW_UNINSTALL, error) ||
          !check_goal_solution (self, removed_pkgnames, replaced_nevras, error))
        {
          rpmostree_output_task_end ("failed");
          return FALSE;
        }
      g_clear_pointer (&self->pkgs, (GDestroyNotify)g_ptr_array_unref);
      self->pkgs = dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                                          DNF_PACKAGE_INFO_INSTALL,
                                          DNF_PACKAGE_INFO_UPDATE,
                                          DNF_PACKAGE_INFO_DOWNGRADE, -1);
      if (!sort_packages (self, self->pkgs, cancellable, error))
        {
          rpmostree_output_task_end ("failed");
          return FALSE;
        }
      rpmostree_output_task_end ("done");
    }

  return TRUE;
}

/* Call this to ensure we don't do any "package stuff" like
 * depsolving - we're in "pure jigdo" mode.  If specified
 * this obviously means there are no layered packages for
 * example.
 */
gboolean
rpmostree_context_prepare_jigdo (RpmOstreeContext *self,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  self->jigdo_pure = TRUE;
  return rpmostree_context_prepare (self, cancellable, error);
}

/* Must have invoked rpmostree_context_prepare().
 * Returns: (transfer container): All packages in the depsolved list.
 */
GPtrArray *
rpmostree_context_get_packages (RpmOstreeContext *self)
{
  return g_ptr_array_ref (self->pkgs);
}

/* Rather than doing a depsolve, directly set which packages
 * are required.  Will be used by jigdo.
 */
gboolean
rpmostree_context_set_packages (RpmOstreeContext *self,
                                GPtrArray        *packages,
                                GCancellable     *cancellable,
                                GError          **error)
{
  g_clear_pointer (&self->pkgs_to_download, (GDestroyNotify)g_ptr_array_unref);
  g_clear_pointer (&self->pkgs_to_import, (GDestroyNotify)g_ptr_array_unref);
  self->n_async_pkgs_imported = 0;
  g_clear_pointer (&self->pkgs_to_relabel, (GDestroyNotify)g_ptr_array_unref);
  self->n_async_pkgs_relabeled = 0;
  return sort_packages (self, packages, cancellable, error);
}

/* Returns a reference to the set of packages that will be imported */
GPtrArray *
rpmostree_context_get_packages_to_import (RpmOstreeContext *self)
{
  g_assert (self->pkgs_to_import);
  return g_ptr_array_ref (self->pkgs_to_import);
}

/* XXX: push this into libdnf */
static const char*
convert_dnf_action_to_string (DnfStateAction action)
{
  switch (action)
    {
    case DNF_STATE_ACTION_INSTALL:
      return "install";
    case DNF_STATE_ACTION_UPDATE:
      return "update";
    case DNF_STATE_ACTION_DOWNGRADE:
      return "downgrade";
    case DNF_STATE_ACTION_REMOVE:
      return "remove";
    case DNF_STATE_ACTION_OBSOLETE:
      return "obsolete";
    default:
      g_assert_not_reached ();
    }
  return NULL; /* satisfy static analysis tools */
}

/* Generate a checksum from a goal in a repeatable fashion - we checksum an ordered array of
 * the checksums of individual packages as well as the associated action. We *used* to just
 * checksum the NEVRAs but that breaks with RPM gpg signatures.
 *
 * This can be used to efficiently see if the goal has changed from a previous one.
 */
gboolean
rpmostree_dnf_add_checksum_goal (GChecksum   *checksum,
                                 HyGoal       goal,
                                 OstreeRepo  *pkgcache,
                                 GError     **error)
{
  g_autoptr(GPtrArray) pkglist = dnf_goal_get_packages (goal, DNF_PACKAGE_INFO_INSTALL,
                                                              DNF_PACKAGE_INFO_UPDATE,
                                                              DNF_PACKAGE_INFO_DOWNGRADE,
                                                              DNF_PACKAGE_INFO_REMOVE,
                                                              DNF_PACKAGE_INFO_OBSOLETE,
                                                              -1);
  g_assert (pkglist);
  g_ptr_array_sort (pkglist, compare_pkgs);
  for (guint i = 0; i < pkglist->len; i++)
    {
      DnfPackage *pkg = pkglist->pdata[i];
      DnfStateAction action = dnf_package_get_action (pkg);
      const char *action_str = convert_dnf_action_to_string (action);
      g_checksum_update (checksum, (guint8*)action_str, strlen (action_str));

      /* For pkgs that were added from the pkgcache repo (e.g. local RPMs and replacement
       * overrides), make sure to pick up the SHA256 from the pkg metadata, rather than what
       * libsolv figured out (which is based on our chopped off fake RPM, which clearly will
       * not match what's in the repo). This ensures that two goals are equivalent whether
       * the same RPM comes from a yum repo or from the pkgcache. */
      const char *reponame = NULL;
      if (pkgcache)
        reponame = dnf_package_get_reponame (pkg);
      if (g_strcmp0 (reponame, HY_CMDLINE_REPO_NAME) == 0)
        {
          g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
          g_autofree char *cached_rev = NULL;
          if (!ostree_repo_resolve_rev (pkgcache, cachebranch, FALSE, &cached_rev, error))
            return FALSE;

          g_autoptr(GVariant) commit = NULL;
          if (!ostree_repo_load_commit (pkgcache, cached_rev, &commit, NULL, error))
            return FALSE;

          g_autofree char *chksum_repr = NULL;
          if (!get_pkgcache_repodata_chksum_repr (commit, &chksum_repr, TRUE, error))
            return FALSE;

          if (chksum_repr)
            {
              g_checksum_update (checksum, (guint8*)chksum_repr, strlen (chksum_repr));
              continue;
            }
        }

      g_autofree char* chksum_repr = NULL;
      g_assert (rpmostree_get_repodata_chksum_repr (pkg, &chksum_repr, NULL));
      g_checksum_update (checksum, (guint8*)chksum_repr, strlen (chksum_repr));
    }

  return TRUE;
}

gboolean
rpmostree_context_get_state_sha512 (RpmOstreeContext *self,
                                    char            **out_checksum,
                                    GError          **error)
{
  g_autoptr(GChecksum) state_checksum = g_checksum_new (G_CHECKSUM_SHA512);
  g_checksum_update (state_checksum, g_variant_get_data (self->spec->spec),
                     g_variant_get_size (self->spec->spec));

  if (!self->empty)
    {
      if (!rpmostree_dnf_add_checksum_goal (state_checksum,
                                            dnf_context_get_goal (self->dnfctx),
                                            get_pkgcache_repo (self), error))
        return FALSE;
    }

  *out_checksum = g_strdup (g_checksum_get_string (state_checksum));
  return TRUE;
}

static GHashTable *
gather_source_to_packages (RpmOstreeContext *self)
{
  g_autoptr(GHashTable) source_to_packages =
    g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  for (guint i = 0; i < self->pkgs_to_download->len; i++)
    {
      DnfPackage *pkg = self->pkgs_to_download->pdata[i];
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
rpmostree_context_download (RpmOstreeContext *self,
                            GCancellable     *cancellable,
                            GError          **error)
{
  int n = self->pkgs_to_download->len;

  if (n > 0)
    {
      guint64 size =
        dnf_package_array_get_download_size (self->pkgs_to_download);
      g_autofree char *sizestr = g_format_size (size);
      rpmostree_output_message ("Will download: %u package%s (%s)", n, _NS(n), sizestr);
    }
  else
    return TRUE;

  { guint progress_sigid;
    g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (self);
    GLNX_HASH_TABLE_FOREACH_KV (source_to_packages, DnfRepo*, src, GPtrArray*, src_packages)
      {
        g_autofree char *target_dir = NULL;
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
        rpmostree_output_progress_end ();
      }
  }

  return TRUE;
}

/* Returns: (transfer none): The jigdo package */
DnfPackage *
rpmostree_context_get_jigdo_pkg (RpmOstreeContext  *self)
{
  g_assert (self->jigdo_spec);
  return self->jigdo_pkg;
}

/* Returns: (transfer none): The jigdo checksum */
const char *
rpmostree_context_get_jigdo_checksum (RpmOstreeContext  *self)
{
  g_assert (self->jigdo_spec);
  return self->jigdo_checksum;
}

static void
on_async_import_done (GObject                    *obj,
                      GAsyncResult               *res,
                      gpointer                    user_data)
{
  RpmOstreeImporter *importer = (RpmOstreeImporter*) obj;
  RpmOstreeContext *self = user_data;
  g_autofree char *rev =
    rpmostree_importer_run_async_finish (importer, res,
                                         self->async_error ? NULL : &self->async_error);
  if (!rev)
    {
      if (self->async_cancellable)
        g_cancellable_cancel (self->async_cancellable);
      g_assert (self->async_error != NULL);
    }

  g_assert_cmpint (self->n_async_pkgs_imported, <, self->pkgs_to_import->len);
  self->n_async_pkgs_imported++;
  rpmostree_output_progress_n_items ("Importing", self->n_async_pkgs_imported,
                                     self->pkgs_to_import->len);
  if (self->n_async_pkgs_imported == self->pkgs_to_import->len)
    self->async_running = FALSE;
}

gboolean
rpmostree_context_import_jigdo (RpmOstreeContext *self,
                                GVariant         *jigdo_xattr_table,
                                GHashTable       *jigdo_pkg_to_xattrs,
                                GCancellable     *cancellable,
                                GError          **error)
{
  DnfContext *dnfctx = self->dnfctx;
  const int n = self->pkgs_to_import->len;
  if (n == 0)
    return TRUE;

  OstreeRepo *repo = get_pkgcache_repo (self);
  g_return_val_if_fail (repo != NULL, FALSE);
  g_return_val_if_fail (jigdo_pkg_to_xattrs == NULL || self->sepolicy == NULL, FALSE);

  if (!dnf_transaction_import_keys (dnf_context_get_transaction (dnfctx), error))
    return FALSE;

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  /* Note use of commit-on-failure */
  if (!rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, cancellable, error))
    return FALSE;

  {
    self->async_running = TRUE;
    self->async_cancellable = cancellable;

    for (guint i = 0; i < self->pkgs_to_import->len; i++)
      {
        DnfPackage *pkg = self->pkgs_to_import->pdata[i];
        GVariant *jigdo_xattrs = NULL;
        if (jigdo_pkg_to_xattrs)
          {
            jigdo_xattrs = g_hash_table_lookup (jigdo_pkg_to_xattrs, pkg);
            if (!jigdo_xattrs)
              g_error ("Failed to find jigdo xattrs for %s", dnf_package_get_nevra (pkg));
          }

        glnx_fd_close int fd = -1;
        if (!rpmostree_context_consume_package (self, pkg, &fd, error))
          return FALSE;

        /* Only set SKIP_EXTRANEOUS for packages we know need it, so that
         * people doing custom composes don't have files silently discarded.
         * (This will also likely need to be configurable).
         */
        const char *pkg_name = dnf_package_get_name (pkg);

        int flags = 0;
        if (g_str_equal (pkg_name, "filesystem") ||
            g_str_equal (pkg_name, "rootfiles"))
          flags |= RPMOSTREE_IMPORTER_FLAGS_SKIP_EXTRANEOUS;

        { gboolean docs;
          g_assert (g_variant_dict_lookup (self->spec->dict, "documentation", "b", &docs));
          if (!docs)
            flags |= RPMOSTREE_IMPORTER_FLAGS_NODOCS;
        }

        /* TODO - tweak the unpacker flags for containers */
        OstreeRepo *ostreerepo = get_pkgcache_repo (self);
        g_autoptr(RpmOstreeImporter) unpacker =
          rpmostree_importer_new_take_fd (&fd, ostreerepo, pkg, flags,
                                          self->sepolicy, error);
        if (!unpacker)
          return FALSE;

        if (jigdo_xattrs)
          {
            g_assert (!self->sepolicy);
            rpmostree_importer_set_jigdo_mode (unpacker, jigdo_xattr_table, jigdo_xattrs);
          }

        rpmostree_importer_run_async (unpacker, cancellable, on_async_import_done, self);
      }

    /* Wait for all of the imports to complete */
    GMainContext *mainctx = g_main_context_get_thread_default ();
    self->async_error = NULL;
    while (self->async_running)
      g_main_context_iteration (mainctx, TRUE);
    if (self->async_error)
      {
        g_propagate_error (error, g_steal_pointer (&self->async_error));
        return FALSE;
      }

    rpmostree_output_progress_end ();
  }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;
  txn.initialized = FALSE;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PKG_IMPORT),
                   "MESSAGE=Imported %u pkg%s", n, _NS(n),
                   "IMPORTED_N_PKGS=%u", n, NULL);

  return TRUE;
}

gboolean
rpmostree_context_import (RpmOstreeContext *self,
                          GCancellable     *cancellable,
                          GError          **error)
{
  return rpmostree_context_import_jigdo (self, NULL, NULL, cancellable, error);
}

/* Given a single package, verify its GPG signature (if enabled), open a file
 * descriptor for it, and delete the on-disk downloaded copy.
 */
gboolean
rpmostree_context_consume_package (RpmOstreeContext  *self,
                                   DnfPackage        *pkg,
                                   int               *out_fd,
                                   GError           **error)
{
  /* Verify signatures if enabled */
  if (!dnf_transaction_gpgcheck_package (dnf_context_get_transaction (self->dnfctx), pkg, error))
    return FALSE;

  g_autofree char *pkg_path = rpmostree_pkg_get_local_path (pkg);
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (AT_FDCWD, pkg_path, TRUE, &fd, error))
    return FALSE;

  /* And delete it now; this does mean if we fail it'll have been
   * deleted and hence more annoying to debug, but in practice people
   * should be able to redownload, and if the error was something like
   * ENOSPC, deleting it was the right move I'd say.
   */
  if (!rpmostree_pkg_is_local (pkg))
    {
      if (!glnx_unlinkat (AT_FDCWD, pkg_path, 0, error))
        return FALSE;
    }

  *out_fd = glnx_steal_fd (&fd);
  return TRUE;
}

static gboolean
checkout_package (OstreeRepo   *repo,
                  int           dfd,
                  const char   *path,
                  OstreeRepoDevInoCache *devino_cache,
                  const char   *pkg_commit,
                  OstreeRepoCheckoutOverwriteMode ovwmode,
                  GCancellable *cancellable,
                  GError      **error)
{
  OstreeRepoCheckoutAtOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                       ovwmode, };

  /* We want the checkout to match the repo type so that we get hardlinks. */
  if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
    opts.mode = OSTREE_REPO_CHECKOUT_MODE_NONE;

  opts.devino_to_csum_cache = devino_cache;

  /* Always want hardlinks */
  opts.no_copy_fallback = TRUE;

  return ostree_repo_checkout_at (repo, &opts, dfd, path,
                                  pkg_commit, cancellable, error);
}

static gboolean
checkout_package_into_root (RpmOstreeContext *self,
                            DnfPackage   *pkg,
                            int           dfd,
                            const char   *path,
                            OstreeRepoDevInoCache *devino_cache,
                            const char   *pkg_commit,
                            OstreeRepoCheckoutOverwriteMode ovwmode,
                            GCancellable *cancellable,
                            GError      **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);

  /* The below is currently TRUE only in the --ex-unified-core path. We probably want to
   * migrate that over to always use a separate cache repo eventually, which would allow us
   * to completely drop the pkgcache_repo/ostreerepo dichotomy in the core. See:
   * https://github.com/projectatomic/rpm-ostree/pull/1055 */
  if (pkgcache_repo != self->ostreerepo)
    {
      if (!rpmostree_pull_content_only (self->ostreerepo, pkgcache_repo, pkg_commit,
                                        cancellable, error))
        {
          g_prefix_error (error, "Linking cached content for %s: ", dnf_package_get_nevra (pkg));
          return FALSE;
        }
    }

  if (!checkout_package (pkgcache_repo, dfd, path,
                         devino_cache, pkg_commit, ovwmode,
                         cancellable, error))
    return glnx_prefix_error (error, "Checkout %s", dnf_package_get_nevra (pkg));

  return TRUE;
}

static Header
get_rpmdb_pkg_header (rpmts rpmdb_ts,
                      DnfPackage *pkg,
                      GCancellable *cancellable,
                      GError **error)
{
  g_auto(rpmts) rpmdb_ts_owned = NULL;
  if (!rpmdb_ts) /* allow callers to pass NULL */
    rpmdb_ts = rpmdb_ts_owned = rpmtsCreate ();

  unsigned int dbid = dnf_package_get_rpmdbid (pkg);
  g_assert (dbid > 0);

  g_auto(rpmdbMatchIterator) it =
    rpmtsInitIterator (rpmdb_ts, RPMDBI_PACKAGES, &dbid, sizeof(dbid));

  Header hdr = it ? rpmdbNextIterator (it) : NULL;
  if (hdr == NULL)
    return glnx_null_throw (error, "Failed to find package '%s' in rpmdb",
                            dnf_package_get_nevra (pkg));

  return headerLink (hdr);
}

static gboolean
delete_package_from_root (RpmOstreeContext *self,
                          rpmte         pkg,
                          int           rootfs_dfd,
                          GCancellable *cancellable,
                          GError      **error)
{
#ifdef BUILDOPT_HAVE_RPMFILES /* use rpmfiles API if possible, rpmteFI is deprecated */
  g_auto(rpmfiles) files = rpmteFiles (pkg);
  g_auto(rpmfi) fi = rpmfilesIter (files, RPMFI_ITER_FWD);
#else
  rpmfi fi = rpmteFI (pkg); /* rpmfi owned by rpmte */
#endif

  g_autoptr(GPtrArray) deleted_dirs = g_ptr_array_new_with_free_func (g_free);

  int i;
  while ((i = rpmfiNext (fi)) >= 0)
    {
      /* see also apply_rpmfi_overrides() for a commented version of the loop */
      const char *fn = rpmfiFN (fi);
      rpm_mode_t mode = rpmfiFMode (fi);

      if (!(S_ISREG (mode) ||
            S_ISLNK (mode) ||
            S_ISDIR (mode)))
        continue;

      g_assert (fn != NULL);
      fn += strspn (fn, "/");
      g_assert (fn[0]);

      g_autofree char *fn_owned = NULL;
      if (g_str_has_prefix (fn, "etc/"))
        fn = fn_owned = g_strconcat ("usr/", fn, NULL);

      /* for now, we only remove files from /usr */
      if (!g_str_has_prefix (fn, "usr/"))
        continue;

      /* avoiding the stat syscall is worth a bit of userspace computation */
      if (rpmostree_str_has_prefix_in_ptrarray (fn, deleted_dirs))
        continue;

      if (!glnx_fstatat_allow_noent (rootfs_dfd, fn, NULL, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT)
        continue; /* a job well done */

      if (!glnx_shutil_rm_rf_at (rootfs_dfd, fn, cancellable, error))
        return FALSE;

      if (S_ISDIR (mode))
        g_ptr_array_add (deleted_dirs, g_strconcat (fn, "/", NULL));
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
      if (!ostree_break_hardlink (dfd_iter.fd, dent->d_name, FALSE,
                                  cancellable, error))
        return FALSE;
    }

  return TRUE;
}

typedef struct {
  int tmpdir_dfd;
  const char *name;
  const char *evr;
  const char *arch;
} RelabelTaskData;

static gboolean
relabel_in_thread_impl (RpmOstreeContext *self,
                        const char       *name,
                        const char       *evr,
                        const char       *arch,
                        int               tmpdir_dfd,
                        gboolean         *out_changed,
                        GCancellable     *cancellable,
                        GError          **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  const char *nevra = glnx_strjoina (name, "-", evr, ".", arch);
  const char *errmsg = glnx_strjoina ("Relabeling ", nevra);
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);
  const char *pkg_dirname = nevra;

  OstreeRepo *repo = get_pkgcache_repo (self);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_for_n_evr_a (name, evr, arch);
  g_autofree char *commit_csum = NULL;
  if (!ostree_repo_resolve_rev (repo, cachebranch, FALSE,
                                &commit_csum, error))
    return FALSE;

  /* Compute the original content checksum */
  g_autoptr(GVariant) orig_commit = NULL;
  if (!ostree_repo_load_commit (repo, commit_csum, &orig_commit, NULL, error))
    return FALSE;
  g_autofree char *orig_content_checksum = rpmostree_commit_content_checksum (orig_commit);

  /* checkout the pkg and relabel, breaking hardlinks */
  g_autoptr(OstreeRepoDevInoCache) cache = ostree_repo_devino_cache_new ();

  if (!checkout_package (repo, tmpdir_dfd, pkg_dirname, cache,
                         commit_csum, OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                         cancellable, error))
    return FALSE;

  /* write to the tree */
  g_autoptr(OstreeRepoCommitModifier) modifier =
    ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME,
                                       NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_devino_cache (modifier, cache);
  ostree_repo_commit_modifier_set_sepolicy (modifier, self->sepolicy);

  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  if (!ostree_repo_write_dfd_to_mtree (repo, tmpdir_dfd, pkg_dirname, mtree,
                                       modifier, cancellable, error))
    return glnx_prefix_error (error, "Writing dfd");

  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    return FALSE;

  /* build metadata and commit */
  g_autoptr(GVariant) commit_var = NULL;
  g_autoptr(GVariantDict) meta_dict = NULL;

  if (!ostree_repo_load_commit (repo, commit_csum, &commit_var, NULL, error))
    return FALSE;

  /* let's just copy the metadata from the previous commit and only change the
   * rpmostree.sepolicy value */
  g_autoptr(GVariant) meta = g_variant_get_child_value (commit_var, 0);
  meta_dict = g_variant_dict_new (meta);

  g_variant_dict_insert (meta_dict, "rpmostree.sepolicy", "s",
                         ostree_sepolicy_get_csum (self->sepolicy));

  g_autofree char *new_commit_csum = NULL;
  if (!ostree_repo_write_commit (repo, NULL, "", "",
                                 g_variant_dict_end (meta_dict),
                                 OSTREE_REPO_FILE (root), &new_commit_csum,
                                 cancellable, error))
    return FALSE;

  /* Compute new content checksum */
  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_commit (repo, new_commit_csum, &new_commit, NULL, error))
    return FALSE;
  g_autofree char *new_content_checksum = rpmostree_commit_content_checksum (new_commit);

  /* Queue an update to the ref */
  ostree_repo_transaction_set_ref (repo, NULL, cachebranch, new_commit_csum);

  /* Return whether or not we actually changed content */
  *out_changed = !g_str_equal (orig_content_checksum, new_content_checksum);

  return TRUE;
}

static void
relabel_in_thread (GTask            *task,
                   gpointer          source,
                   gpointer          task_data,
                   GCancellable     *cancellable)
{
  g_autoptr(GError) local_error = NULL;
  RpmOstreeContext *self = source;
  RelabelTaskData *tdata = task_data;

  gboolean changed;
  if (!relabel_in_thread_impl (self, tdata->name, tdata->evr, tdata->arch,
                               tdata->tmpdir_dfd, &changed,
                               cancellable, &local_error))
    g_task_return_error (task, g_steal_pointer (&local_error));
  else
    g_task_return_int (task, changed ? 1 : 0);
}

static void
relabel_package_async (RpmOstreeContext   *self,
                       DnfPackage         *pkg,
                       int                 tmpdir_dfd,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
  g_autoptr(GTask) task = g_task_new (self, cancellable, callback, user_data);
  RelabelTaskData *tdata = g_new (RelabelTaskData, 1);
  /* We can assume lifetime is greater than the task */
  tdata->tmpdir_dfd = tmpdir_dfd;
  tdata->name = dnf_package_get_name (pkg);
  tdata->evr = dnf_package_get_evr (pkg);
  tdata->arch = dnf_package_get_arch (pkg);
  g_task_set_task_data (task, tdata, g_free);
  g_task_run_in_thread (task, relabel_in_thread);
}

static gssize
relabel_package_async_finish (RpmOstreeContext   *self,
                              GAsyncResult       *result,
                              GError            **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_int ((GTask*)result, error);
}

typedef struct {
  RpmOstreeContext *self;
  guint n_changed_files;
  guint n_changed_pkgs;
} RpmOstreeAsyncRelabelData;

static void
on_async_relabel_done (GObject                    *obj,
                       GAsyncResult               *res,
                       gpointer                    user_data)
{
  RpmOstreeAsyncRelabelData *data = user_data;
  RpmOstreeContext *self = data->self;
  gssize n_relabeled =
    relabel_package_async_finish (self, res, self->async_error ? NULL : &self->async_error);
  if (n_relabeled < 0)
    {
      g_assert (self->async_error != NULL);
      if (self->async_cancellable)
        g_cancellable_cancel (self->async_cancellable);
    }

  g_assert_cmpint (self->n_async_pkgs_relabeled, <, self->pkgs_to_relabel->len);
  self->n_async_pkgs_relabeled++;
  if (n_relabeled > 0)
    {
      data->n_changed_files += n_relabeled;
      data->n_changed_pkgs++;
    }
  rpmostree_output_progress_n_items ("Relabeling", self->n_async_pkgs_relabeled,
                                     self->pkgs_to_relabel->len);
  if (self->n_async_pkgs_relabeled == self->pkgs_to_relabel->len)
    self->async_running = FALSE;
}

static gboolean
relabel_if_necessary (RpmOstreeContext *self,
                      GCancellable     *cancellable,
                      GError          **error)
{
  if (!self->pkgs_to_relabel)
    return TRUE;

  const int n = self->pkgs_to_relabel->len;
  OstreeRepo *ostreerepo = get_pkgcache_repo (self);

  if (n == 0)
    return TRUE;

  g_assert (self->sepolicy);

  g_return_val_if_fail (ostreerepo != NULL, FALSE);

  /* Prep a txn and tmpdir for all of the relabels */
  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, ostreerepo, FALSE, cancellable, error))
    return FALSE;

  g_auto(GLnxTmpDir) relabel_tmpdir = { 0, };
  if (!glnx_mkdtempat (ostree_repo_get_dfd (ostreerepo), "tmp/rpm-ostree-relabel.XXXXXX", 0700,
                       &relabel_tmpdir, error))
    return FALSE;

  self->async_running = TRUE;
  self->async_cancellable = cancellable;

  RpmOstreeAsyncRelabelData data = { self, 0, };
  const guint n_to_relabel = self->pkgs_to_relabel->len;
  for (guint i = 0; i < n_to_relabel; i++)
    {
      DnfPackage *pkg = self->pkgs_to_relabel->pdata[i];
      relabel_package_async (self, pkg, relabel_tmpdir.fd, cancellable,
                             on_async_relabel_done, &data);
    }

  /* Wait for all of the relabeling to complete */
  GMainContext *mainctx = g_main_context_get_thread_default ();
  self->async_error = NULL;
  while (self->async_running)
    g_main_context_iteration (mainctx, TRUE);
  if (self->async_error)
    {
      g_propagate_error (error, g_steal_pointer (&self->async_error));
      return FALSE;
    }

  rpmostree_output_progress_end ();

  /* Commit */
  if (!ostree_repo_commit_transaction (ostreerepo, NULL, cancellable, error))
    return FALSE;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_SELINUX_RELABEL),
                   "MESSAGE=Relabeled %u/%u pkgs", data.n_changed_pkgs, n_to_relabel,
                   "RELABELED_PKGS=%u/%u", data.n_changed_pkgs, n_to_relabel,
                   NULL);

  g_clear_pointer (&self->pkgs_to_relabel, (GDestroyNotify)g_ptr_array_unref);
  self->n_async_pkgs_relabeled = 0;

  return TRUE;
}

/* Forcibly relabel all packages */
gboolean
rpmostree_context_force_relabel (RpmOstreeContext *self,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  g_clear_pointer (&self->pkgs_to_relabel, (GDestroyNotify)g_ptr_array_unref);
  self->pkgs_to_relabel = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_autoptr(GPtrArray) packages = dnf_goal_get_packages (dnf_context_get_goal (self->dnfctx),
                                                         DNF_PACKAGE_INFO_INSTALL,
                                                         DNF_PACKAGE_INFO_UPDATE,
                                                         DNF_PACKAGE_INFO_DOWNGRADE, -1);

  for (guint i = 0; i < packages->len; i++)
    {
      DnfPackage *pkg = packages->pdata[i];

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      /* This logic is equivalent to that in sort_packages() */
      gboolean in_ostree, selinux_match;
      if (!find_pkg_in_ostree (self, pkg, self->sepolicy,
                               &in_ostree, &selinux_match, error))
        return FALSE;

      if (in_ostree && !selinux_match)
        g_ptr_array_add (self->pkgs_to_relabel, g_object_ref (pkg));
    }

  return relabel_if_necessary (self, cancellable, error);
}

typedef struct {
  FD_t current_trans_fd;
  RpmOstreeContext *ctx;
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
        g_autofree char *relpath = get_package_relpath (pkg);
        g_autofree char *path = g_build_filename (tdata->ctx->tmpdir.path, relpath, NULL);
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
get_package_metainfo (RpmOstreeContext *self,
                      const char *path,
                      Header *out_header,
                      rpmfi *out_fi,
                      GError **error)
{
  glnx_autofd int metadata_fd = -1;
  if (!glnx_openat_rdonly (self->tmpdir.fd, path, TRUE, &metadata_fd, error))
    return FALSE;

  return rpmostree_importer_read_metainfo (metadata_fd, out_header, NULL,
                                           out_fi, error);
}

typedef enum {
  RPMOSTREE_TS_FLAG_UPGRADE = (1 << 0),
  RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS = (1 << 1),
} RpmOstreeTsAddInstallFlags;

static gboolean
rpmts_add_install (RpmOstreeContext *self,
                   rpmts  ts,
                   DnfPackage *pkg,
                   RpmOstreeTsAddInstallFlags flags,
                   GCancellable *cancellable,
                   GError **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, &hdr, NULL, error))
    return FALSE;

  if (!(flags & RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS))
    {
      if (!rpmostree_script_txn_validate (pkg, hdr, cancellable, error))
        return FALSE;
    }

  const gboolean is_upgrade = (flags & RPMOSTREE_TS_FLAG_UPGRADE) > 0;
  if (rpmtsAddInstallElement (ts, hdr, pkg, is_upgrade, NULL) != 0)
    return glnx_throw (error, "Failed to add install element for %s",
                       dnf_package_get_filename (pkg));

  return TRUE;
}

static gboolean
rpmts_add_erase (RpmOstreeContext *self,
                 rpmts ts,
                 DnfPackage *pkg,
                 GCancellable *cancellable,
                 GError **error)
{
  /* NB: we're not running any %*un scriptlets right now */
  g_auto(Header) hdr = get_rpmdb_pkg_header (ts, pkg, cancellable, error);
  if (hdr == NULL)
    return FALSE;

  if (rpmtsAddEraseElement (ts, hdr, -1))
    return glnx_throw (error, "Failed to add erase element for package '%s'",
                       dnf_package_get_nevra (pkg));

  return TRUE;
}

/* Look up the header for a package, and pass it
 * to the script core to execute.
 */
static gboolean
run_script_sync (RpmOstreeContext *self,
                 int rootfs_dfd,
                 DnfPackage *pkg,
                 RpmOstreeScriptKind kind,
                 guint        *out_n_run,
                 GCancellable *cancellable,
                 GError    **error)
{
  g_auto(Header) hdr = NULL;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, &hdr, NULL, error))
    return FALSE;

  if (!rpmostree_script_run_sync (pkg, hdr, kind, rootfs_dfd,
                                  out_n_run, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
apply_rpmfi_overrides (RpmOstreeContext *self,
                       int            tmprootfs_dfd,
                       DnfPackage    *pkg,
                       GHashTable    *passwdents,
                       GHashTable    *groupents,
                       GCancellable  *cancellable,
                       GError       **error)
{
  /* In an unprivileged case, we can't do this on the real filesystem. For `ex
   * container`, we want to completely ignore uid/gid.
   *
   * TODO: For non-root `--ex-unified-core` we need to do it as a commit modifier.
   */
  if (getuid () != 0)
    return TRUE;  /* 🔚 Early return */

  int i;
  g_auto(rpmfi) fi = NULL;
  gboolean emitted_nonusr_warning = FALSE;
  g_autofree char *path = get_package_relpath (pkg);

  if (!get_package_metainfo (self, path, NULL, &fi, error))
    return FALSE;

  while ((i = rpmfiNext (fi)) >= 0)
    {
      const char *fn = rpmfiFN (fi);
      const char *user = rpmfiFUser (fi) ?: "root";
      const char *group = rpmfiFGroup (fi) ?: "root";
      const char *fcaps = rpmfiFCaps (fi) ?: '\0';
      const gboolean have_fcaps = fcaps[0] != '\0';
      rpm_mode_t mode = rpmfiFMode (fi);
      rpmfileAttrs fattrs = rpmfiFFlags (fi);
      const gboolean is_ghost = fattrs & RPMFILE_GHOST;

      if (g_str_equal (user, "root") &&
          g_str_equal (group, "root") &&
          !have_fcaps)
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

      g_autofree char *modified_fn = NULL;  /* May be used to override fn */
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

      struct stat stbuf;
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
          if (!ostree_break_hardlink (tmprootfs_dfd, fn, FALSE, cancellable, error))
            return FALSE;
        }

      if ((!g_str_equal (user, "root") && !passwdents) ||
          (!g_str_equal (group, "root") && !groupents))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Missing passwd/group files for chown");
          return FALSE;
        }

      uid_t uid = 0;
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

      gid_t gid = 0;
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
        return glnx_throw_errno_prefix (error, "fchownat(%s)", fn);

      /* the chown clears away file caps, so reapply it here */
      if (have_fcaps)
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
            return glnx_throw_errno_prefix (error, "fchmodat(%s)", fn);
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

static gboolean
add_install (RpmOstreeContext *self,
             DnfPackage       *pkg,
             rpmts             ts,
             gboolean          is_upgrade,
             GHashTable       *pkg_to_ostree_commit,
             GCancellable     *cancellable,
             GError          **error)
{
  OstreeRepo *pkgcache_repo = get_pkgcache_repo (self);
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  g_autofree char *cached_rev = NULL;

  if (!ostree_repo_resolve_rev (pkgcache_repo, cachebranch, FALSE,
                                &cached_rev, error))
    return FALSE;

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (pkgcache_repo, cached_rev, &commit, NULL, error))
    return FALSE;

  gboolean sepolicy_matches;
  if (self->sepolicy)
    {
      if (!commit_has_matching_sepolicy (commit, self->sepolicy, &sepolicy_matches,
                                         error))
        return FALSE;

      /* We already did any relabeling/reimporting above */
      g_assert (sepolicy_matches);
    }

  if (!checkout_pkg_metadata_by_dnfpkg (self, pkg, cancellable, error))
    return FALSE;

  RpmOstreeTsAddInstallFlags flags = 0;
  if (is_upgrade)
    flags |= RPMOSTREE_TS_FLAG_UPGRADE;
  if (!rpmts_add_install (self, ts, pkg, flags, cancellable, error))
    return FALSE;

  g_hash_table_insert (pkg_to_ostree_commit, g_object_ref (pkg),
                                             g_steal_pointer (&cached_rev));
  return TRUE;
}

/* Run %transfiletriggerin */
static gboolean
run_all_transfiletriggers (RpmOstreeContext *self,
                           rpmts         ts,
                           int           rootfs_dfd,
                           guint        *out_n_run,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_assert (!self->jigdo_pure);

  /* Triggers from base packages, but only if we already have an rpmdb,
   * otherwise librpm will whine on our stderr.
   */
  if (!glnx_fstatat_allow_noent (rootfs_dfd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == 0)
    {
      g_auto(rpmdbMatchIterator) mi = rpmtsInitIterator (ts, RPMDBI_PACKAGES, NULL, 0);
      Header hdr;
      while ((hdr = rpmdbNextIterator (mi)) != NULL)
        {
          if (!rpmostree_transfiletriggers_run_sync (hdr, rootfs_dfd, out_n_run,
                                                     cancellable, error))
            return FALSE;
        }
    }

  /* Triggers from newly added packages */
  const guint n = (guint)rpmtsNElements (ts);
  for (guint i = 0; i < n; i++)
    {
      rpmte te = rpmtsElement (ts, i);
      if (rpmteType (te) != TR_ADDED)
        continue;
      DnfPackage *pkg = (void*)rpmteKey (te);
      g_autofree char *path = get_package_relpath (pkg);
      g_auto(Header) hdr = NULL;
      if (!get_package_metainfo (self, path, &hdr, NULL, error))
        return FALSE;

      if (!rpmostree_transfiletriggers_run_sync (hdr, rootfs_dfd, out_n_run,
                                                 cancellable, error))
        return FALSE;
    }
  return TRUE;
}

/* Set the root directory fd used for assemble(); used
 * by the sysroot upgrader for the base tree.  This is optional;
 * assemble() will use a tmpdir if not provided.
 */
void
rpmostree_context_set_tmprootfs_dfd (RpmOstreeContext *self,
                                     int               dfd)
{
  g_assert_cmpint (self->tmprootfs_dfd, ==, -1);
  self->tmprootfs_dfd = dfd;
}

int
rpmostree_context_get_tmprootfs_dfd  (RpmOstreeContext *self)
{
  return self->tmprootfs_dfd;
}

gboolean
rpmostree_context_assemble (RpmOstreeContext      *self,
                            GCancellable          *cancellable,
                            GError               **error)
{
  /* Synthesize a tmpdir if we weren't provided a base */
  if (self->tmprootfs_dfd == -1)
    {
      g_assert (!self->repo_tmpdir.initialized);
      int repo_dfd = ostree_repo_get_dfd (self->ostreerepo); /* Borrowed */
      if (!glnx_mkdtempat (repo_dfd, "tmp/rpmostree-assemble-XXXXXX", 0700,
                           &self->repo_tmpdir, error))
        return FALSE;
      self->tmprootfs_dfd = self->repo_tmpdir.fd;
    }

  int tmprootfs_dfd = self->tmprootfs_dfd; /* Alias to avoid bigger diff */

  /* We need up to date labels; the set of things needing relabeling
   * will have been calculated in sort_packages()
   */
  if (!relabel_if_necessary (self, cancellable, error))
    return FALSE;

  DnfContext *dnfctx = self->dnfctx;
  TransactionData tdata = { 0, NULL };
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, (GDestroyNotify)g_free);
  DnfPackage *filesystem_package = NULL;   /* It's special, see below */
  DnfPackage *setup_package = NULL;   /* Also special due to composes needing to inject /etc/passwd */

  g_auto(rpmts) ordering_ts = rpmtsCreate ();
  rpmtsSetRootDir (ordering_ts, dnf_context_get_install_root (dnfctx));

  /* First for the ordering TS, set the dbpath to relative, which will also gain
   * the root dir.
   */
  set_rpm_macro_define ("_dbpath", "/" RPMOSTREE_RPMDB_LOCATION);

  /* Determine now if we have an existing rpmdb; whether this is a layering
   * operation or a fresh root. Mostly we use this to change how we render
   * output.
   */
  if (!glnx_fstatat_allow_noent (self->tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  const gboolean layering_on_base = (errno == 0);

  /* Don't verify checksums here (we should have done this on ostree
   * import).  Also, avoid updating the database or anything by
   * flagging it as a test.  We'll do the database next.
   */
  rpmtsSetVSFlags (ordering_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS | RPMTRANS_FLAG_TEST);

  g_autoptr(GPtrArray) overlays =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_INSTALL,
                           -1);

  g_autoptr(GPtrArray) overrides_replace =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_UPDATE,
                           DNF_PACKAGE_INFO_DOWNGRADE,
                           -1);

  g_autoptr(GPtrArray) overrides_remove =
    dnf_goal_get_packages (dnf_context_get_goal (dnfctx),
                           DNF_PACKAGE_INFO_REMOVE,
                           DNF_PACKAGE_INFO_OBSOLETE,
                           -1);

  if (overlays->len == 0 && overrides_remove->len == 0 && overrides_replace->len == 0)
    return glnx_throw (error, "No packages in transaction");

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */

  for (guint i = 0; i < overrides_remove->len; i++)
    {
      DnfPackage *pkg = overrides_remove->pdata[i];
      if (!rpmts_add_erase (self, ordering_ts, pkg, cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overrides_replace->len; i++)
    {
      DnfPackage *pkg = overrides_replace->pdata[i];
      if (!add_install (self, pkg, ordering_ts, TRUE, pkg_to_ostree_commit,
                        cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overlays->len; i++)
    {
      DnfPackage *pkg = overlays->pdata[i];
      if (!add_install (self, pkg, ordering_ts, FALSE,
                        pkg_to_ostree_commit, cancellable, error))
        return FALSE;

      if (strcmp (dnf_package_get_name (pkg), "filesystem") == 0)
        filesystem_package = g_object_ref (pkg);
      else if (strcmp (dnf_package_get_name (pkg), "setup") == 0)
        setup_package = g_object_ref (pkg);
    }

  { DECLARE_RPMSIGHANDLER_RESET;
    rpmtsOrder (ordering_ts);
  }

  guint overrides_total = overrides_remove->len + overrides_replace->len;
  if (!layering_on_base)
    {
      g_assert_cmpint (overrides_total, ==, 0);
      rpmostree_output_message ("Installing %u package%s", overlays->len,
                                _NS(overlays->len));
    }
  else if (overrides_total > 0)
    {
      g_assert (layering_on_base);
      if (overlays->len > 0)
        rpmostree_output_message ("Applying %u override%s and %u overlay%s",
                                  overrides_total, _NS(overrides_total),
                                  overlays->len, _NS(overlays->len));
      else
        rpmostree_output_message ("Applying %u override%s", overrides_total,
                                  _NS(overrides_total));
    }

  else if (overlays->len > 0)
    rpmostree_output_message ("Applying %u overlay%s", overlays->len,
                              _NS(overlays->len));
  else
    g_assert_not_reached ();

  const guint n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);
  g_assert (n_rpmts_elements > 0);
  guint n_rpmts_done = 0;

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
                                       tmprootfs_dfd, ".", self->devino_cache,
                                       g_hash_table_lookup (pkg_to_ostree_commit,
                                                            filesystem_package),
                                       OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL,
                                       cancellable, error))
        return FALSE;
      n_rpmts_done++;
      rpmostree_output_progress_n_items ("Building filesystem", n_rpmts_done, n_rpmts_elements);
    }

  /* We're completely disregarding how rpm normally does upgrades/downgrade here. rpm
   * usually calculates the fate of each file and then installs the new files first, then
   * removes the obsoleted files. So the TR_ADDED element for the new pkg comes *before* the
   * TR_REMOVED element for the old pkg. This makes sense when operating on the live system.
   * Though that's not the case here, so let's just take the easy way out and first nuke
   * TR_REMOVED pkgs, then checkout the TR_ADDED ones. This will probably start to matter
   * though when we start handling e.g. file triggers. */

  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      rpmElementType type = rpmteType (te);

      if (type == TR_ADDED)
        continue;
      g_assert_cmpint (type, ==, TR_REMOVED);

      if (!delete_package_from_root (self, te, tmprootfs_dfd, cancellable, error))
        return FALSE;
      n_rpmts_done++;
      rpmostree_output_progress_n_items ("Building filesystem", n_rpmts_done, n_rpmts_elements);
    }

  for (guint i = 0; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      rpmElementType type = rpmteType (te);

      if (type == TR_REMOVED)
        continue;
      g_assert (type == TR_ADDED);

      DnfPackage *pkg = (void*)rpmteKey (te);
      if (pkg == filesystem_package)
        continue;

      /* The "setup" package currently contains /etc/passwd; in the treecompose
       * case we need to inject that beforehand, so use "add files" just for
       * that.
       */
      OstreeRepoCheckoutOverwriteMode ovwmode =
        (pkg == setup_package) ? OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES :
        OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL;

      if (!checkout_package_into_root (self, pkg, tmprootfs_dfd, ".", self->devino_cache,
                                       g_hash_table_lookup (pkg_to_ostree_commit, pkg),
                                       ovwmode, cancellable, error))
        return FALSE;
      n_rpmts_done++;
      rpmostree_output_progress_n_items ("Building filesystem", n_rpmts_done, n_rpmts_elements);
    }

  rpmostree_output_progress_end ();

  if (!rpmostree_rootfs_prepare_links (tmprootfs_dfd, cancellable, error))
    return FALSE;

  /* NB: we're not running scripts right now for removals, so this is only for overlays and
   * replacements */
  if (overlays->len > 0 || overrides_replace->len > 0)
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

      /* Also neuter systemctl - at least glusterfs for example calls `systemctl
       * start` in its %post which both violates Fedora policy and also will not
       * work with the rpm-ostree model.
       * See also https://github.com/projectatomic/rpm-ostree/issues/550
       *
       * See also the SYSTEMD_OFFLINE bits in rpmostree-scripts.c; at some
       * point in the far future when we don't support CentOS7 we can drop
       * our wrapper script.  If we remember.
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
          g_autoptr(GBytes) systemctl_wrapper = g_resources_lookup_data ("/rpmostree/systemctl-wrapper.sh",
                                                                         G_RESOURCE_LOOKUP_FLAGS_NONE,
                                                                         error);
          if (!systemctl_wrapper)
            return FALSE;
          size_t len;
          const guint8* buf = g_bytes_get_data (systemctl_wrapper, &len);
          if (!glnx_file_replace_contents_with_perms_at (tmprootfs_dfd, "usr/bin/systemctl",
                                                         buf, len, 0755, (uid_t) -1, (gid_t) -1,
                                                         GLNX_FILE_REPLACE_NODATASYNC,
                                                         cancellable, error))
            return FALSE;
        }

      /* Necessary for unified core to work with semanage calls in %post, like container-selinux */
      if (!rpmostree_rootfs_fixup_selinux_store_root (tmprootfs_dfd, cancellable, error))
        return FALSE;

      /* We're technically deviating from RPM here by running all the %pre's
       * beforehand, rather than each package's %pre & %post in order. Though I
       * highly doubt this should cause any issues. The advantage of doing it
       * this way is that we only need to read the passwd/group files once
       * before applying the overrides, rather than after each %pre.
       */
      rpmostree_output_task_begin ("Running pre scripts");
      guint n_pre_scripts_run = 0;
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          if (rpmteType (te) != TR_ADDED)
            continue;

          DnfPackage *pkg = (void*)rpmteKey (te);
          g_assert (pkg);

          if (!run_script_sync (self, tmprootfs_dfd, pkg, RPMOSTREE_SCRIPT_PREIN,
                                &n_pre_scripts_run, cancellable, error))
            return FALSE;
        }
      rpmostree_output_task_end ("%u done", n_pre_scripts_run);

      if (faccessat (tmprootfs_dfd, "usr/etc/passwd", F_OK, 0) == 0)
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

      if (faccessat (tmprootfs_dfd, "usr/etc/group", F_OK, 0) == 0)
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

      rpmostree_output_task_begin ("Running post scripts");
      guint n_post_scripts_run = 0;

      /* %post */
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          if (rpmteType (te) != TR_ADDED)
            continue;

          DnfPackage *pkg = (void*)rpmteKey (te);
          g_assert (pkg);

          if (!apply_rpmfi_overrides (self, tmprootfs_dfd, pkg, passwdents, groupents,
                                      cancellable, error))
            return glnx_prefix_error (error, "While applying overrides for pkg %s",
                                      dnf_package_get_name (pkg));

          if (!run_script_sync (self, tmprootfs_dfd, pkg, RPMOSTREE_SCRIPT_POSTIN,
                                &n_post_scripts_run, cancellable, error))
            return FALSE;
        }

      /* %posttrans */
      for (guint i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          if (rpmteType (te) != TR_ADDED)
            continue;

          DnfPackage *pkg = (void*)rpmteKey (te);
          g_assert (pkg);

          if (!run_script_sync (self, tmprootfs_dfd, pkg, RPMOSTREE_SCRIPT_POSTTRANS,
                                &n_post_scripts_run, cancellable, error))
            return FALSE;
        }

      /* file triggers */
      if (!run_all_transfiletriggers (self, ordering_ts, tmprootfs_dfd,
                                      &n_post_scripts_run, cancellable, error))
        return FALSE;

      rpmostree_output_task_end ("%u done", n_post_scripts_run);

      /* We want this to be the first error message if something went wrong
       * with a script; see https://github.com/projectatomic/rpm-ostree/pull/888
       */
      gboolean skip_sanity_check = FALSE;
      g_variant_dict_lookup (self->spec->dict, "skip-sanity-check", "b", &skip_sanity_check);
      if (!skip_sanity_check &&
          !rpmostree_deployment_sanitycheck (tmprootfs_dfd, cancellable, error))
        return FALSE;

      if (have_systemctl)
        {
          if (!glnx_renameat (tmprootfs_dfd, "usr/bin/systemctl.rpmostreesave",
                              tmprootfs_dfd, "usr/bin/systemctl", error))
            return FALSE;
        }

      if (have_passwd)
        {
          if (!rpmostree_passwd_complete_rpm_layering (tmprootfs_dfd, error))
            return FALSE;
        }
    }
  else
    {
      /* Also do a sanity check even if we have no layered packages */
      if (!rpmostree_deployment_sanitycheck (tmprootfs_dfd, cancellable, error))
        return FALSE;
    }

  g_clear_pointer (&ordering_ts, rpmtsFree);

  rpmostree_output_task_begin ("Writing rpmdb");

  if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, 0755, cancellable, error))
    return FALSE;

  /* Now, we use the separate rpmdb ts which *doesn't* have a rootdir set,
   * because if it did rpmtsRun() would try to chroot which it won't be able to
   * if we're unprivileged, even though we're not trying to run %post scripts
   * now.
   *
   * Instead, this rpmts has the dbpath as absolute.
   */
  { g_autofree char *rpmdb_abspath = glnx_fdrel_abspath (tmprootfs_dfd,
                                                         RPMOSTREE_RPMDB_LOCATION);

    /* if we were passed an existing tmprootfs, and that tmprootfs already has
     * an rpmdb, we have to make sure to break its hardlinks as librpm mutates
     * the db in place */
    if (!break_hardlinks_at (tmprootfs_dfd, RPMOSTREE_RPMDB_LOCATION, cancellable, error))
      return FALSE;

    set_rpm_macro_define ("_dbpath", rpmdb_abspath);
  }

  g_auto(rpmts) rpmdb_ts = rpmtsCreate ();
  rpmtsSetVSFlags (rpmdb_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
  rpmtsSetFlags (rpmdb_ts, RPMTRANS_FLAG_JUSTDB);

  tdata.ctx = self;
  rpmtsSetNotifyCallback (rpmdb_ts, ts_callback, &tdata);

  /* Skip validating scripts since we already validated them above */
  RpmOstreeTsAddInstallFlags rpmdb_instflags = RPMOSTREE_TS_FLAG_NOVALIDATE_SCRIPTS;
  for (guint i = 0; i < overlays->len; i++)
    {
      DnfPackage *pkg = overlays->pdata[i];

      if (!rpmts_add_install (self, rpmdb_ts, pkg, rpmdb_instflags,
                              cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < overrides_replace->len; i++)
    {
      DnfPackage *pkg = overrides_replace->pdata[i];

      if (!rpmts_add_install (self, rpmdb_ts, pkg,
                              rpmdb_instflags | RPMOSTREE_TS_FLAG_UPGRADE,
                              cancellable, error))
        return FALSE;
    }

  /* and mark removed packages as such so they drop out of rpmdb */
  for (guint i = 0; i < overrides_remove->len; i++)
    {
      DnfPackage *pkg = overrides_remove->pdata[i];
      if (!rpmts_add_erase (self, rpmdb_ts, pkg, cancellable, error))
        return FALSE;
    }

  rpmtsOrder (rpmdb_ts);

  /* NB: Because we're using the real root here (see above for reason why), rpm
   * will see the read-only /usr mount and think that there isn't any disk space
   * available for install. For now, we just tell rpm to ignore space
   * calculations, but then we lose that nice check. What we could do is set a
   * root dir at least if we have CAP_SYS_CHROOT, or maybe do the space req
   * check ourselves if rpm makes that information easily accessible (doesn't
   * look like it from a quick glance). */
  /* Also enable OLDPACKAGE to allow replacement overrides to older version. */
  int r = rpmtsRun (rpmdb_ts, NULL, RPMPROB_FILTER_DISKSPACE | RPMPROB_FILTER_OLDPACKAGE);
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
rpmostree_context_commit (RpmOstreeContext      *self,
                          const char            *parent,
                          RpmOstreeAssembleType  assemble_type,
                          char                 **out_commit,
                          GCancellable          *cancellable,
                          GError               **error)
{
  g_autoptr(OstreeRepoCommitModifier) commit_modifier = NULL;
  g_autofree char *ret_commit_checksum = NULL;

  rpmostree_output_task_begin ("Writing OSTree commit");

  g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
  if (!rpmostree_repo_auto_transaction_start (&txn, self->ostreerepo, FALSE, cancellable, error))
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

        /* embed packages (really, "patterns") layered */
        g_autoptr(GVariant) pkgs =
          g_variant_dict_lookup_value (self->spec->dict, "packages", G_VARIANT_TYPE ("as"));
        g_assert (pkgs);
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.packages", pkgs);

        /* embed packages removed */
        /* we have to embed both the pkgname and the full nevra to make it easier to match
         * them up with origin directives. the full nevra is used for status -v */
        g_auto(GVariantBuilder) removed_base_pkgs;
        g_variant_builder_init (&removed_base_pkgs, (GVariantType*)"av");
        if (self->pkgs_to_remove)
          {
            GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_remove,
                                        const char*, name, GVariant*, nevra)
              g_variant_builder_add (&removed_base_pkgs, "v", nevra);
          }
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.removed-base-packages",
                               g_variant_builder_end (&removed_base_pkgs));

        /* embed packages replaced */
        g_auto(GVariantBuilder) replaced_base_pkgs;
        g_variant_builder_init (&replaced_base_pkgs, (GVariantType*)"a(vv)");
        if (self->pkgs_to_replace)
          {
            GLNX_HASH_TABLE_FOREACH_KV (self->pkgs_to_replace,
                                        GVariant*, new_nevra, GVariant*, old_nevra)
              g_variant_builder_add (&replaced_base_pkgs, "(vv)", new_nevra, old_nevra);
          }
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.replaced-base-packages",
                               g_variant_builder_end (&replaced_base_pkgs));

        /* this is used by the db commands, and auto updates to diff against the base */
        g_autoptr(GVariant) rpmdb = NULL;
        if (!rpmostree_create_rpmdb_pkglist_variant (self->tmprootfs_dfd, ".", &rpmdb,
                                                     cancellable, error))
          return FALSE;
        g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.rpmdb.pkglist", rpmdb);

        /* be nice to our future selves */
        g_variant_builder_add (&metadata_builder, "{sv}",
                               "rpmostree.clientlayer_version",
                               g_variant_new_uint32 (3));
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

    if (!rpmostree_context_get_state_sha512 (self, &state_checksum, error))
      return FALSE;

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.state-sha512",
                           g_variant_new_string (state_checksum));

    OstreeRepoCommitModifierFlags modflags = OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE;
    /* For ex-container (bare-user-only), we always need canonical permissions */
    if (ostree_repo_get_mode (self->ostreerepo) == OSTREE_REPO_MODE_BARE_USER_ONLY)
      modflags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS;

    /* if we're SELinux aware, then reload the final policy from the tmprootfs in case it
     * was changed by a scriptlet; this covers the foobar/foobar-selinux path */
    g_autoptr(OstreeSePolicy) final_sepolicy = NULL;
    if (self->sepolicy)
      {
        if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, &final_sepolicy,
                                                    cancellable, error))
          return FALSE;

        /* If policy didn't change (or SELinux is disabled), then we can treat
         * the on-disk xattrs as canonical; loading the xattrs is a noticeable
         * slowdown for commit.
         */
        if (ostree_sepolicy_get_name (final_sepolicy) == NULL ||
            g_strcmp0 (ostree_sepolicy_get_csum (self->sepolicy),
                       ostree_sepolicy_get_csum (final_sepolicy)) == 0)
          {
            if (ostree_repo_get_mode (self->ostreerepo) == OSTREE_REPO_MODE_BARE)
              modflags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL;
          }
      }

    commit_modifier = ostree_repo_commit_modifier_new (modflags, NULL, NULL, NULL);
    if (final_sepolicy)
      ostree_repo_commit_modifier_set_sepolicy (commit_modifier, final_sepolicy);

    if (self->devino_cache)
      ostree_repo_commit_modifier_set_devino_cache (commit_modifier, self->devino_cache);

    mtree = ostree_mutable_tree_new ();

    const guint64 start_time_ms = g_get_monotonic_time () / 1000;
    if (!ostree_repo_write_dfd_to_mtree (self->ostreerepo, self->tmprootfs_dfd, ".",
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
      const guint64 end_time_ms = g_get_monotonic_time () / 1000;
      const guint64 elapsed_ms = end_time_ms - start_time_ms;

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
                       "OSTREE_TXN_ELAPSED_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                       NULL);
    }
  }

  rpmostree_output_task_end ("done");

  self->tmprootfs_dfd = -1;
  if (!glnx_tmpdir_delete (&self->repo_tmpdir, cancellable, error))
    return FALSE;

  if (out_commit)
    *out_commit = g_steal_pointer (&ret_commit_checksum);
  return TRUE;
}
