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
#include <libhif/libhif.h>
#include <librepo/librepo.h>

#include "rpmostree-core.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-unpacker.h"
#include "rpmostree-output.h"

#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "solv"
#define RPMOSTREE_DIR_LOCK "lock"

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

static gboolean
add_canonicalized_string_array (GVariantBuilder *builder,
                                const char *key,
                                const char *notfound_key,
                                GKeyFile *keyfile,
                                GError **error)
{
  g_auto(GStrv) input = NULL;
  g_autoptr(GHashTable) set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autofree char **sorted = NULL;
  guint count;
  char **iter;
  g_autoptr(GError) temp_error = NULL;

  input = g_key_file_get_string_list (keyfile, "tree", key, NULL, &temp_error);
  if (!(input && *input))
    {
      if (notfound_key)
        {
          g_variant_builder_add (builder, "{sv}", notfound_key, g_variant_new_boolean (TRUE));
          return TRUE;
        }
      else if (temp_error)
        {
          g_propagate_error (error, g_steal_pointer (&temp_error));
          return FALSE;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Key %s is empty", key);
          return FALSE;
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
  return TRUE;
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

  if (!add_canonicalized_string_array (&builder, "packages", NULL, keyfile, error))
    return NULL;

  /* We allow the "repo" key to be missing. This means that we rely on hif's
   * normal behaviour (i.e. look at repos in repodir with enabled=1). */
  { g_auto(GStrv) val = g_key_file_get_string_list (keyfile, "tree", "repos",
                                                    NULL, NULL);
    if (val && *val &&
        !add_canonicalized_string_array (&builder, "repos", "", keyfile, error))
      return NULL;
  }

  if (!add_canonicalized_string_array (&builder, "instlangs", "instlangs-all", keyfile, error))
    return NULL;

  if (!add_canonicalized_string_array (&builder, "ignore-scripts", "", keyfile, error))
    return NULL;

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
  HifContext *hifctx;
  GHashTable *ignore_scripts;
  OstreeRepo *ostreerepo;
  gboolean unprivileged;
  char *dummy_instroot_path;
  OstreeSePolicy *sepolicy;
};

G_DEFINE_TYPE (RpmOstreeContext, rpmostree_context, G_TYPE_OBJECT)

static void
rpmostree_context_finalize (GObject *object)
{
  RpmOstreeContext *rctx = RPMOSTREE_CONTEXT (object);

  g_clear_object (&rctx->hifctx);

  if (rctx->dummy_instroot_path)
    {
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, rctx->dummy_instroot_path, NULL, NULL);
      g_free (rctx->dummy_instroot_path);
    }

  g_clear_object (&rctx->ostreerepo);

  g_clear_object (&rctx->sepolicy);

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

  self->hifctx = hif_context_new ();
  _rpmostree_reset_rpm_sighandlers ();
  hif_context_set_http_proxy (self->hifctx, g_getenv ("http_proxy"));

  hif_context_set_repo_dir (self->hifctx, "/etc/yum.repos.d");
  /* Operating on stale metadata is too annoying */
  hif_context_set_cache_age (self->hifctx, 0);
  hif_context_set_cache_dir (self->hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_REPOMD);
  hif_context_set_solv_dir (self->hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_SOLV);
  hif_context_set_lock_dir (self->hifctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);
  hif_context_set_user_agent (self->hifctx, PACKAGE_NAME "/" PACKAGE_VERSION);

  hif_context_set_check_disk_space (self->hifctx, FALSE);
  hif_context_set_check_transaction (self->hifctx, FALSE);
  hif_context_set_yumdb_enabled (self->hifctx, FALSE);

  return self;
}

static RpmOstreeContext *
rpmostree_context_new_internal (int           userroot_dfd,
                                gboolean      unprivileged,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(RpmOstreeContext) ret = rpmostree_context_new_system (cancellable, error);
  struct stat stbuf;

  if (!ret)
    goto out;

  ret->unprivileged = unprivileged;

  { g_autofree char *reposdir = glnx_fdrel_abspath (userroot_dfd, "rpmmd.repos.d");
    hif_context_set_repo_dir (ret->hifctx, reposdir);
  }
  { const char *cache_rpmmd = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_REPOMD);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_rpmmd);
    hif_context_set_cache_dir (ret->hifctx, cachedir);
  }
  { const char *cache_solv = glnx_strjoina ("cache/", RPMOSTREE_DIR_CACHE_SOLV);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, cache_solv);
    hif_context_set_solv_dir (ret->hifctx, cachedir);
  }
  { const char *lock = glnx_strjoina ("cache/", RPMOSTREE_DIR_LOCK);
    g_autofree char *cachedir = glnx_fdrel_abspath (userroot_dfd, lock);
    hif_context_set_lock_dir (ret->hifctx, lock);
  }

  if (fstatat (userroot_dfd, "repo", &stbuf, 0) < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    {
      g_autofree char *repopath_str = glnx_fdrel_abspath (userroot_dfd, "repo");
      g_autoptr(GFile) repopath = g_file_new_for_path (repopath_str);

      ret->ostreerepo = ostree_repo_new (repopath);

      if (!ostree_repo_open (ret->ostreerepo, cancellable, error))
        goto out;
    }

 out:
  if (ret)
    return g_steal_pointer (&ret);
  return NULL;
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

/* XXX: or put this in new_system() instead? */
void
rpmostree_context_set_repo (RpmOstreeContext *self,
                            OstreeRepo       *repo)
{
  g_set_object (&self->ostreerepo, repo);
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
rpmostree_context_set_ignore_scripts (RpmOstreeContext *self,
                                      GHashTable   *ignore_scripts)
{
  g_clear_pointer (&self->ignore_scripts, g_hash_table_unref);
  if (ignore_scripts)
    self->ignore_scripts = g_hash_table_ref (ignore_scripts);
}

HifContext *
rpmostree_context_get_hif (RpmOstreeContext *self)
{
  return self->hifctx;
}

GHashTable *
rpmostree_context_get_varsubsts (RpmOstreeContext *context)
{
  GHashTable *r = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_insert (r, g_strdup ("basearch"), g_strdup (hif_context_get_base_arch (context->hifctx)));

  return r;
}

static void
require_enabled_repos (GPtrArray *sources)
{
  for (guint i = 0; i < sources->len; i++)
    {
      HifRepo *src = sources->pdata[i];
      if (hif_repo_get_enabled (src) != HIF_REPO_ENABLED_NONE)
        hif_repo_set_required (src, TRUE);
    }
}

static gboolean
enable_one_repo (GPtrArray           *sources,
                 const char          *reponame,
                 GError             **error)
{
  gboolean ret = FALSE;
  gboolean found = FALSE;

  for (guint i = 0; i < sources->len; i++)
    {
      HifRepo *src = sources->pdata[i];
      const char *id = hif_repo_get_id (src);

      if (strcmp (reponame, id) != 0)
        continue;

      hif_repo_set_enabled (src, HIF_REPO_ENABLED_PACKAGES);
      found = TRUE;
      break;
    }

  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown rpm-md repository: %s", reponame);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
context_repos_enable_only (RpmOstreeContext    *context,
                           const char    *const *enabled_repos,
                           GError       **error)
{
  gboolean ret = FALSE;
  guint i;
  const char *const *iter;
  GPtrArray *sources;

  sources = hif_context_get_repos (context->hifctx);
  for (i = 0; i < sources->len; i++)
    {
      HifRepo *src = sources->pdata[i];
      hif_repo_set_enabled (src, HIF_REPO_ENABLED_NONE);
    }

  for (iter = enabled_repos; iter && *iter; iter++)
    {
      if (!enable_one_repo (sources, *iter, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_context_setup (RpmOstreeContext    *self,
                         const char    *install_root,
                         const char    *source_root,
                         RpmOstreeTreespec *spec,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  GPtrArray *repos = NULL;
  char **enabled_repos = NULL;
  char **instlangs = NULL;

  if (install_root)
    hif_context_set_install_root (self->hifctx, install_root);
  else
    {
      glnx_fd_close int dfd = -1; /* Auto close, we just use the path */
      if (!rpmostree_mkdtemp ("/tmp/rpmostree-dummy-instroot-XXXXXX",
                              &self->dummy_instroot_path,
                              &dfd, error))
        goto out;
      hif_context_set_install_root (self->hifctx, self->dummy_instroot_path);
    }

  if (source_root)
    hif_context_set_source_root (self->hifctx, source_root);

  if (!hif_context_setup (self->hifctx, cancellable, error))
    goto out;

  /* This is what we use as default. */
  set_rpm_macro_define ("_dbpath", "/usr/share/rpm");

  self->spec = g_object_ref (spec);

  /* NB: missing repo --> let hif figure it out for itself */
  if (g_variant_dict_lookup (self->spec->dict, "repos", "^a&s", &enabled_repos))
    if (!context_repos_enable_only (self, (const char *const*)enabled_repos, error))
      goto out;

  repos = hif_context_get_repos (self->hifctx);
  if (repos->len == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No enabled repositories");
      goto out;
    }

  require_enabled_repos (repos);

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

      hif_context_set_rpm_macro (self->hifctx, "_install_langs", opt->str);
      g_string_free (opt, TRUE);
    }

  { gboolean docs;

    g_variant_dict_lookup (self->spec->dict, "documentation", "b", &docs);
    
    if (!docs)
        hif_transaction_set_flags (hif_context_get_transaction (self->hifctx),
                                   HIF_TRANSACTION_FLAG_NODOCS);
  }

  { const char *const *ignore_scripts = NULL;
    if (g_variant_dict_lookup (self->spec->dict, "ignore-scripts", "^a&s", &ignore_scripts))
      {
        g_autoptr(GHashTable) ignore_hash = NULL;

        if (!rpmostree_script_ignore_hash_from_strv (ignore_scripts, &ignore_hash, error))
          goto out;
        rpmostree_context_set_ignore_scripts (self, ignore_hash);
      }
  }

  ret = TRUE;
 out:
  return ret;
}


static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  rpmostree_output_percent_progress (text, percentage);
}

gboolean
rpmostree_context_download_metadata (RpmOstreeContext *self,
                                     GCancellable     *cancellable,
                                     GError          **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  gs_unref_object HifState *hifstate = hif_state_new ();

  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed),
                                     "Downloading metadata:");

  if (!hif_context_setup_sack (self->hifctx, hifstate, error))
    goto out;

  g_signal_handler_disconnect (hifstate, progress_sigid);
  rpmostree_output_percent_progress_end ();

  ret = TRUE;
 out:
  _rpmostree_reset_rpm_sighandlers ();
  return ret;
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
rpmostree_get_cache_branch_pkg (HifPackage *pkg)
{
  return cache_branch_for_n_evr_a (hif_package_get_name (pkg),
                                   hif_package_get_evr (pkg),
                                   hif_package_get_arch (pkg));
}

static gboolean
pkg_is_local (HifPackage *pkg)
{
  HifRepo *src = hif_package_get_repo (pkg);
  return (hif_repo_is_local (src) || 
          g_strcmp0 (hif_package_get_reponame (pkg), HY_CMDLINE_REPO_NAME) == 0);
}

static gboolean
get_commit_sepolicy_csum (GVariant *commit,
                          char    **out_csum,
                          GError  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) meta_dict = g_variant_dict_new (meta);
  g_autoptr(GVariant) sepolicy_csum_variant = NULL;

  sepolicy_csum_variant =
    _rpmostree_vardict_lookup_value_required (meta_dict, "rpmostree.sepolicy",
                                              (GVariantType*)"s", error);
  if (!sepolicy_csum_variant)
    goto out;

  g_assert (g_variant_is_of_type (sepolicy_csum_variant,
                                  G_VARIANT_TYPE_STRING));
  *out_csum = g_strdup (g_variant_get_string (sepolicy_csum_variant, NULL));

  ret = TRUE;
out:
  return ret;
}

static gboolean
find_rev_with_sepolicy (OstreeRepo     *repo,
                        const char     *head,
                        OstreeSePolicy *sepolicy,
                        gboolean        allow_noent,
                        char          **out_rev,
                        GError        **error)
{
  gboolean ret = FALSE;
  const char *sepolicy_csum_wanted = ostree_sepolicy_get_csum (sepolicy);
  g_autofree char *commit_rev = g_strdup (head);

  /* walk up the branch until we find a matching policy */
  while (commit_rev != NULL)
    {
      g_autoptr(GVariant) commit = NULL;
      g_autofree char *sepolicy_csum = NULL;

      if (!ostree_repo_load_commit (repo, commit_rev, &commit, NULL, error))
        goto out;
      g_assert (commit);

      if (!get_commit_sepolicy_csum (commit, &sepolicy_csum, error))
        goto out;

      if (strcmp (sepolicy_csum, sepolicy_csum_wanted) == 0)
        break;

      g_free (commit_rev);
      commit_rev = ostree_commit_get_parent (commit);
    }

  if (commit_rev == NULL && !allow_noent)
    goto out;

  *out_rev = g_steal_pointer (&commit_rev);

  ret = TRUE;
out:
  return ret;
}

static gboolean
pkg_is_cached (HifPackage *pkg)
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
  return g_file_test (hif_package_get_filename (pkg), G_FILE_TEST_EXISTS);
}

static gboolean
find_pkg_in_ostree (OstreeRepo     *repo,
                    HifPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    gboolean       *out_in_ostree,
                    gboolean       *out_selinux_match,
                    GError        **error)
{
  gboolean ret = TRUE;

  *out_in_ostree = FALSE;
  *out_selinux_match = FALSE;

  if (repo != NULL)
    {
      g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
      g_autofree char *cached_rev = NULL;

      if (!ostree_repo_resolve_rev (repo, cachebranch, TRUE,
                                    &cached_rev, error))
        goto out;

      if (cached_rev)
        {
          *out_in_ostree = TRUE;

          if (sepolicy)
            {
              g_autofree char *cached_rev_sel = NULL;
              if (!find_rev_with_sepolicy (repo, cached_rev, sepolicy,
                                           TRUE, &cached_rev_sel, error))
                goto out;

              if (cached_rev_sel)
                *out_selinux_match = TRUE;
            }
        }
    }

  ret = TRUE;
out:
  return ret;
}

/* determine of all the marked packages, which ones we'll need to download,
 * which ones we'll need to import, and which ones we'll need to relabel */
static gboolean
sort_packages (HifContext       *hifctx,
               OstreeRepo       *ostreerepo,
               OstreeSePolicy   *sepolicy,
               RpmOstreeInstall *install,
               GError          **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) packages = NULL;
  GPtrArray *sources = hif_context_get_repos (hifctx);

  g_clear_pointer (&install->packages_to_download, g_ptr_array_unref);
  install->packages_to_download
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_clear_pointer (&install->packages_to_import, g_ptr_array_unref);
  install->packages_to_import
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_clear_pointer (&install->packages_to_relabel, g_ptr_array_unref);
  install->packages_to_relabel
    = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  packages = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                    HIF_PACKAGE_INFO_INSTALL,
                                    HIF_PACKAGE_INFO_REINSTALL,
                                    HIF_PACKAGE_INFO_DOWNGRADE,
                                    HIF_PACKAGE_INFO_UPDATE,
                                    -1);

  for (guint i = 0; i < packages->len; i++)
    {
      HifPackage *pkg = packages->pdata[i];
      HifRepo *src = NULL;

      /* Hackily look up the source...we need a hash table */
      for (guint j = 0; j < sources->len; j++)
        {
          HifRepo *tmpsrc = sources->pdata[j];
          if (g_strcmp0 (hif_package_get_reponame (pkg),
                         hif_repo_get_id (tmpsrc)) == 0)
            {
              src = tmpsrc;
              break;
            }
        }

      g_assert (src);
      hif_package_set_repo (pkg, src);

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
          goto out;

        if (!in_ostree && !cached)
          g_ptr_array_add (install->packages_to_download, g_object_ref (pkg));
        if (!in_ostree)
          g_ptr_array_add (install->packages_to_import, g_object_ref (pkg));
        if (in_ostree && !selinux_match)
          g_ptr_array_add (install->packages_to_relabel, g_object_ref (pkg));
      }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_context_prepare_install (RpmOstreeContext    *self,
                                   RpmOstreeInstall    **out_install,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = self->hifctx;
  char **pkgnames = NULL;
  g_autoptr(RpmOstreeInstall) ret_install = g_object_new (RPMOSTREE_TYPE_INSTALL, NULL);

  g_assert (g_variant_dict_lookup (self->spec->dict, "packages", "^a&s", &pkgnames));

  ret_install->packages_requested = g_ptr_array_new_with_free_func (g_free);

  { const char *const*strviter = (const char *const*)pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!hif_context_install (hifctx, pkgname, error))
          goto out;
        g_ptr_array_add (ret_install->packages_requested, g_strdup (pkgname));
      }
  }

  rpmostree_output_task_begin ("Resolving dependencies");

  if (!hif_goal_depsolve (hif_context_get_goal (hifctx), HIF_INSTALL, error))
    {
      g_print ("failed\n");
      goto out;
    }

  rpmostree_output_task_end ("done");

  if (!sort_packages (hifctx, self->ostreerepo, self->sepolicy,
                      ret_install, error))
    goto out;

  rpmostree_print_transaction (hifctx);

  ret = TRUE;
  *out_install = g_steal_pointer (&ret_install);
 out:
  return ret;
}

static int
ptrarray_sort_compare_strings (gconstpointer ap,
                               gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

/* Generate a checksum from a goal in a repeatable fashion -
 * we checksum an ordered array of the checksums of individual
 * packages.  We *used* to just checksum the NEVRAs but that
 * breaks with RPM gpg signatures.
 *
 * This can be used to efficiently see if the goal has changed from a
 * previous one.
 */
void
rpmostree_hif_add_checksum_goal (GChecksum *checksum,
                                 HyGoal     goal)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  guint i;
  g_autoptr(GPtrArray) pkg_checksums = g_ptr_array_new_with_free_func (g_free);

  pkglist = hy_goal_list_installs (goal, NULL);
  g_assert (pkglist);
  for (i = 0; i < pkglist->len; i++)
    {
      HifPackage *pkg = pkglist->pdata[i];
      int pkg_checksum_type;
      const unsigned char *pkg_checksum_bytes = hif_package_get_chksum (pkg, &pkg_checksum_type);
      g_autofree char *pkg_checksum = hy_chksum_str (pkg_checksum_bytes, pkg_checksum_type);
      char *pkg_checksum_with_type = g_strconcat (hy_chksum_name (pkg_checksum_type), ":", pkg_checksum, NULL);
      
      g_ptr_array_add (pkg_checksums, pkg_checksum_with_type);
    }

  g_ptr_array_sort (pkg_checksums, ptrarray_sort_compare_strings);
    
  for (i = 0; i < pkg_checksums->len; i++)
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

  rpmostree_hif_add_checksum_goal (state_checksum, hif_context_get_goal (self->hifctx));
  return g_strdup (g_checksum_get_string (state_checksum));
}

static GHashTable *
gather_source_to_packages (HifContext *hifctx,
                           RpmOstreeInstall *install)
{
  guint i;
  g_autoptr(GHashTable) source_to_packages =
    g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  for (i = 0; i < install->packages_to_download->len; i++)
    {
      HifPackage *pkg = install->packages_to_download->pdata[i];
      HifRepo *src = hif_package_get_repo (pkg);
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
  gboolean ret = FALSE;
  HifContext *hifctx = ctx->hifctx;
  int n = install->packages_to_download->len;

  if (n > 0)
    {
      guint64 size = hif_package_array_get_download_size (install->packages_to_download);
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
        HifRepo *src = key;
        GPtrArray *src_packages = value;
        gs_unref_object HifState *hifstate = hif_state_new ();

        g_autofree char *prefix
          = g_strdup_printf ("  Downloading from %s:", hif_repo_get_id (src));

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed),
                                           prefix);

        target_dir = g_build_filename (hif_repo_get_location (src), "/packages/", NULL);
        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
          goto out;

        if (!hif_repo_download_packages (src, src_packages, target_dir,
                                         hifstate, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
        rpmostree_output_percent_progress_end ();
      }
  }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_package (RpmOstreeContext *self,
                    HifContext     *hifctx,
                    HifPackage     *pkg,
                    OstreeSePolicy *sepolicy,
                    GCancellable   *cancellable,
                    GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepo *ostreerepo = self->ostreerepo;
  g_autofree char *ostree_commit = NULL;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  g_autofree char *pkg_path;
  int flags = 0;

  if (pkg_is_local (pkg))
    pkg_path = g_strdup (hif_package_get_filename (pkg));
  else
    {
      g_autofree char *pkg_location = hif_package_get_location (pkg);
      pkg_path =
        g_build_filename (hif_repo_get_location (hif_package_get_repo (pkg)),
                          "packages", glnx_basename (pkg_location), NULL);
    }

  flags = RPMOSTREE_UNPACKER_FLAGS_OSTREE_CONVENTION;
  if (self->unprivileged)
    flags |= RPMOSTREE_UNPACKER_FLAGS_UNPRIVILEGED;

  /* TODO - tweak the unpacker flags for containers */
  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, pkg_path, flags, error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostreerepo, sepolicy,
                                            &ostree_commit, cancellable, error))
    {
      const char *nevra = hif_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
      goto out;
    }

  if (!pkg_is_local (pkg))
    {
      if (TEMP_FAILURE_RETRY (unlinkat (AT_FDCWD, pkg_path, 0)) < 0)
        {
          glnx_set_error_from_errno (error);
          g_prefix_error (error, "Deleting %s: ", pkg_path);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static inline void
hif_state_assert_done (HifState *hifstate)
{
  gboolean r;
  r = hif_state_done (hifstate, NULL);
  g_assert (r);
}

gboolean
rpmostree_context_import (RpmOstreeContext *self,
                          RpmOstreeInstall *install,
                          GCancellable     *cancellable,
                          GError          **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = self->hifctx;
  guint progress_sigid;
  int n = install->packages_to_import->len;

  if (n == 0)
    return TRUE;

  g_return_val_if_fail (self->ostreerepo != NULL, FALSE);

  {
    glnx_unref_object HifState *hifstate = hif_state_new ();
    hif_state_set_number_steps (hifstate, install->packages_to_import->len);
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed),
                                       "Importing:");

    for (guint i = 0; i < install->packages_to_import->len; i++)
      {
        HifPackage *pkg = install->packages_to_import->pdata[i];
        if (!import_one_package (self, hifctx, pkg,
                                 self->sepolicy, cancellable, error))
          goto out;
        hif_state_assert_done (hifstate);
      }

    g_signal_handler_disconnect (hifstate, progress_sigid);
    rpmostree_output_percent_progress_end ();
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
ostree_checkout_package (OstreeRepo   *repo,
                         HifPackage   *pkg,
                         int           dfd,
                         const char   *path,
                         OstreeRepoDevInoCache *devino_cache,
                         const char   *pkg_commit,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

  /* We want the checkout to match the repo type so that we get hardlinks. */
  if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
    opts.mode = OSTREE_REPO_CHECKOUT_MODE_NONE;

  opts.devino_to_csum_cache = devino_cache;

  /* For now... to be crash safe we'd need to duplicate some of the
   * boot-uuid/fsync gating at a higher level.
   */
  opts.disable_fsync = TRUE;
  /* Always want hardlinks */
  opts.no_copy_fallback = TRUE;

  if (!ostree_repo_checkout_tree_at (repo, &opts, dfd, path,
                                     pkg_commit, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (error && *error)
    {
      const char *nevra = hif_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
    }
  return ret;
}

/* Given a path to a file/symlink, make a copy (reflink if possible)
 * of it if it's a hard link.  We need this for two places right now:
 *  - The RPM database
 *  - SELinux policy "denormalization" where a label changes
 */
static gboolean
break_single_hardlink_at (int           dfd,
                          const char   *path,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;

  if (fstatat (dfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "fstatat");
      goto out;
    }

  if (!S_ISLNK (stbuf.st_mode) && !S_ISREG (stbuf.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported type for entry '%s'", path);
      goto out;
    }

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
              goto out;
            }

          copy_success = TRUE;
          break;
        }

      if (!copy_success)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                       "Exceeded limit of %u file creation attempts", count);
          goto out;
        }
      
      if (renameat (dfd, path_tmp, dfd, path) != 0)
        {
          glnx_set_prefix_error_from_errno (error, "Rename %s", path);
          goto out;
        }
    }

  ret = TRUE;
out:
  return ret;
}

/* Given a directory referred to by @dfd and @dirpath, ensure that physical (or
 * reflink'd) copies of all files are done. */
static gboolean
break_hardlinks_at (int             dfd,
                    const char     *dirpath,
                    GCancellable   *cancellable,
                    GError        **error)
{
  gboolean ret = FALSE;
  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  struct dirent *dent = NULL;

  if (!glnx_dirfd_iterator_init_at (dfd, dirpath, TRUE, &dfd_iter, error))
    goto out;

  while (glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
    {
      if (dent == NULL)
        break;
      if (!break_single_hardlink_at (dfd_iter.fd, dent->d_name, 
                                     cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
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
                        gboolean          *out_changed,
                        GCancellable      *cancellable,
                        GError           **error)
{
  gboolean ret = FALSE;

  g_auto(GLnxDirFdIterator) dfd_iter = { FALSE, };
  struct dirent *dent = NULL;

  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE, &dfd_iter, error))
    goto out;

  while (glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent,
                                                     cancellable, error))
    {
      g_autofree char *fullpath = NULL;

      const char *cur_label = NULL;
      g_autofree char *new_label = NULL;
      g_autoptr(GVariant) cur_xattrs = NULL;
      g_autoptr(GVariant) new_xattrs = NULL;

      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR &&
          dent->d_type != DT_REG &&
          dent->d_type != DT_LNK)
        continue;

      if (!glnx_dfd_name_get_all_xattrs (dfd_iter.fd, dent->d_name, &cur_xattrs,
                                         cancellable, error))
        goto out;

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
          {
            glnx_set_prefix_error_from_errno (error, "%s", "fstatat");
            goto out;
          }

        /* may be NULL */
        if (!ostree_sepolicy_get_label (sepolicy, fullpath, stbuf.st_mode,
                                        &new_label, cancellable, error))
          goto out;
      }

      if (g_strcmp0 (cur_label, new_label) != 0)
        {
          if (dent->d_type != DT_DIR)
            if (!break_single_hardlink_at (dfd_iter.fd, dent->d_name,
                                           cancellable, error))
              goto out;

          new_xattrs = set_selinux_label (cur_xattrs, new_label);

          if (!glnx_dfd_name_set_all_xattrs (dfd_iter.fd, dent->d_name,
                                             new_xattrs, cancellable, error))
            goto out;

          *out_changed = TRUE;
        }

      if (dent->d_type == DT_DIR)
        if (!relabel_dir_recurse_at (repo, dfd_iter.fd, dent->d_name, fullpath,
                                     sepolicy, out_changed, cancellable, error))
          goto out;
    }

  ret = TRUE;
out:
  return ret;
}

static gboolean
relabel_rootfs (OstreeRepo        *repo,
                int                dfd,
                OstreeSePolicy    *sepolicy,
                gboolean          *out_changed,
                GCancellable      *cancellable,
                GError           **error)
{
  /* NB: this does mean that / itself will not be labeled properly, but that
   * doesn't matter since it will always exist during overlay */
  return relabel_dir_recurse_at (repo, dfd, ".", "/", sepolicy,
                                 out_changed, cancellable, error);
}

static gboolean
relabel_one_package (OstreeRepo     *repo,
                     HifPackage     *pkg,
                     OstreeSePolicy *sepolicy,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;

  g_autofree char *tmprootfs = g_strdup ("tmp/rpmostree-relabel-XXXXXX");
  glnx_fd_close int tmprootfs_dfd = -1;
  OstreeRepoDevInoCache *cache = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit_csum = NULL;
  g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
  gboolean changed = FALSE;


  /* let's just use the branch head */
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

  if (!ostree_checkout_package (repo, pkg, tmprootfs_dfd, ".", cache,
                                commit_csum, cancellable, error))
    goto out;

  /* This is where the magic happens. We traverse the tree and relabel stuff,
   * making sure to break hardlinks if needed. */
  if (!relabel_rootfs (repo, tmprootfs_dfd, sepolicy, &changed,
                       cancellable, error))
    goto out;

  /* XXX: 'changed' now holds whether the policy change actually affected any of
   * our labels. If it didn't, then we shouldn't have to recommit, which we do
   * right now unconditionally. Related to the XXX below, maybe we can keep the
   * list of compatible sepolicy csums in the tree directly under e.g.
   * /meta/sepolicy/. Make them individual files rather than a single file so
   * that they can more easily be GC'ed by "refcounting" each sepolicy depending
   * on the current deployments. */

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

    /* let's just copy the metadata from the head and only change the
     * rpmostree.sepolicy value */
    {
      g_autoptr(GVariant) meta = g_variant_get_child_value (commit_var, 0);
      meta_dict = g_variant_dict_new (meta);

      g_variant_dict_insert (meta_dict, "rpmostree.sepolicy", "s",
                             ostree_sepolicy_get_csum (sepolicy));
    }

  /* XXX: Eventually we should find a way to make the header metadata be shared
   * between commits. Either store it in the tree and put its checksum in the
   * commit metadata, or just store it in the tree itself (e.g. have a contents/
   * and a /meta/header). */
    {
      g_autofree char *new_commit_csum = NULL;
      if (!ostree_repo_write_commit (repo, commit_csum, "", "",
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
  if (cache)
    ostree_repo_devino_cache_unref (cache);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
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
  gboolean ret = FALSE;
  guint progress_sigid;
  int n = install->packages_to_relabel->len;

  if (n == 0)
    return TRUE;

  g_assert (self->sepolicy);

  g_return_val_if_fail (self->ostreerepo != NULL, FALSE);

  {
    glnx_unref_object HifState *hifstate = hif_state_new ();
    g_autofree char *prefix = g_strdup_printf ("Relabeling %d package%s:",
                                               n, n>1 ? "s" : "");

    hif_state_set_number_steps (hifstate, install->packages_to_relabel->len);
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed),
                                       prefix);

    for (guint i = 0; i < install->packages_to_relabel->len; i++)
      {
        HifPackage *pkg = install->packages_to_relabel->pdata[i];
        if (!relabel_one_package (self->ostreerepo, pkg, self->sepolicy,
                                  cancellable, error))
          goto out;
        hif_state_assert_done (hifstate);
      }

    g_signal_handler_disconnect (hifstate, progress_sigid);
    rpmostree_output_percent_progress_end ();
  }

  ret = TRUE;
 out:
  return ret;
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
        HifPackage *pkg = (void*)key;
        const char *nevra = hif_package_get_nevra (pkg);
        g_autofree char *path = glnx_fdrel_abspath (tdata->tmp_metadata_dfd, nevra);
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

static Header
get_header_for_package (int tmp_metadata_dfd,
                        HifPackage *pkg,
                        GError **error)
{
  Header hdr = NULL;
  glnx_fd_close int metadata_fd = -1;
  const char *nevra = hif_package_get_nevra (pkg);

  if ((metadata_fd = openat (tmp_metadata_dfd, nevra, O_RDONLY | O_CLOEXEC)) < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  if (!rpmostree_unpacker_read_metainfo (metadata_fd, &hdr, NULL, NULL, error))
    return NULL;

  return hdr;
}

static gboolean
add_to_transaction (rpmts  ts,
                    HifPackage *pkg,
                    int tmp_metadata_dfd,
                    gboolean noscripts,
                    GHashTable *ignore_scripts,
                    GCancellable *cancellable,
                    GError **error)
{
  gboolean ret = FALSE;
  Header hdr = NULL;
  int r;

  hdr = get_header_for_package (tmp_metadata_dfd, pkg, error);
  if (!hdr)
    goto out;

  if (!noscripts)
    {
      if (!rpmostree_script_txn_validate (pkg, hdr, ignore_scripts, cancellable, error))
        goto out;
    }

  r = rpmtsAddInstallElement (ts, hdr, pkg, TRUE, NULL);
  if (r != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to add install element for %s",
                   hif_package_get_filename (pkg));
      goto out;
    }

  ret = TRUE;
 out:
  if (hdr)
    headerFree (hdr);
  return ret;
}

static gboolean
run_posttrans_sync (int tmp_metadata_dfd,
                    int rootfs_dfd,
                    HifPackage *pkg,
                    GHashTable *ignore_scripts,
                    GCancellable *cancellable,
                    GError    **error)
{
  gboolean ret = FALSE;
  Header hdr;

  hdr = get_header_for_package (tmp_metadata_dfd, pkg, error);
  if (!hdr)
    goto out;

  if (!rpmostree_posttrans_run_sync (pkg, hdr, ignore_scripts, rootfs_dfd,
                                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (hdr)
    headerFree (hdr);
  return ret;
}

/* FIXME: This is a copy of ot_admin_checksum_version */
static char *
checksum_version (GVariant *checksum)
{
  gs_unref_variant GVariant *metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, "version", "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

gboolean
rpmostree_context_assemble_commit (RpmOstreeContext      *self,
                                   int                    tmprootfs_dfd,
                                   OstreeRepoDevInoCache *devino_cache,
                                   const char            *parent,
                                   gboolean               noscripts,
                                   char                 **out_commit,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = self->hifctx;
  TransactionData tdata = { 0, -1 };
  rpmts ordering_ts = NULL;
  rpmts rpmdb_ts = NULL;
  guint i, n_rpmts_elements;
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, (GDestroyNotify)g_free);
  HifPackage *filesystem_package = NULL;   /* It's special... */
  int r;
  char *tmp_metadata_dir_path = NULL;
  glnx_fd_close int tmp_metadata_dfd = -1;
  g_autofree char *ret_commit_checksum = NULL;
  OstreeRepoCommitModifier *commit_modifier = NULL;

  if (!rpmostree_mkdtemp ("/tmp/rpmostree-metadata-XXXXXX", &tmp_metadata_dir_path,
                          &tmp_metadata_dfd, error))
    goto out;
  tdata.tmp_metadata_dfd = tmp_metadata_dfd;

  ordering_ts = rpmtsCreate ();
  rpmtsSetRootDir (ordering_ts, hif_context_get_install_root (hifctx));
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

    package_list = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                          HIF_PACKAGE_INFO_INSTALL,
                                          -1);

    for (i = 0; i < package_list->len; i++)
      {
        HifPackage *pkg = package_list->pdata[i];
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
        g_autofree char *cached_rev = NULL;
        g_autoptr(GVariant) pkg_commit = NULL;
        g_autoptr(GVariant) header_variant = NULL;
        const char *nevra = hif_package_get_nevra (pkg);

        {
          g_autofree char *branch_head_rev = NULL;

          if (!ostree_repo_resolve_rev (self->ostreerepo, cachebranch, FALSE,
                                        &branch_head_rev, error))
            goto out;

          if (self->sepolicy == NULL)
            cached_rev = g_steal_pointer (&branch_head_rev);
          else if (!find_rev_with_sepolicy (self->ostreerepo, branch_head_rev,
                                            self->sepolicy, FALSE, &cached_rev,
                                            error))
            goto out;
        }

        if (!ostree_repo_load_variant (self->ostreerepo, OSTREE_OBJECT_TYPE_COMMIT, cached_rev,
                                       &pkg_commit, error))
          goto out;

        { g_autoptr(GVariant) pkg_meta = g_variant_get_child_value (pkg_commit, 0);
          g_autoptr(GVariantDict) pkg_meta_dict = g_variant_dict_new (pkg_meta);

          header_variant = _rpmostree_vardict_lookup_value_required (pkg_meta_dict, "rpmostree.metadata",
                                                                     (GVariantType*)"ay", error);
          if (!header_variant)
            {
              g_prefix_error (error, "In commit %s of %s: ", cached_rev, hif_package_get_package_id (pkg));
              goto out;
            }

          if (!glnx_file_replace_contents_at (tmp_metadata_dfd, nevra,
                                              g_variant_get_data (header_variant),
                                              g_variant_get_size (header_variant),
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            goto out;

          if (!add_to_transaction (ordering_ts, pkg, tmp_metadata_dfd, noscripts,
                                   self->ignore_scripts,
                                   cancellable, error))
            goto out;
        }

        g_hash_table_insert (pkg_to_ostree_commit, g_object_ref (pkg), g_steal_pointer (&cached_rev));

        if (strcmp (hif_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = g_object_ref (pkg);
      }
  }

  rpmtsOrder (ordering_ts);
  _rpmostree_reset_rpm_sighandlers ();

  rpmostree_output_task_begin ("Overlaying");

  n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);

  if (devino_cache == NULL)
    devino_cache = ostree_repo_devino_cache_new ();
  else
    devino_cache = ostree_repo_devino_cache_ref (devino_cache);

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
      if (!ostree_checkout_package (self->ostreerepo, filesystem_package,
                                    tmprootfs_dfd, ".", devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit,
                                                         filesystem_package),
                                    cancellable, error))
        goto out;
    }
  else
    {
      /* Otherwise, we unpack the first package to get the initial
       * rootfs dir.
       */
      rpmte te = rpmtsElement (ordering_ts, 0);
      HifPackage *pkg = (void*)rpmteKey (te);

      g_assert (pkg);

      if (!ostree_checkout_package (self->ostreerepo, pkg,
                                    tmprootfs_dfd, ".", devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit,
                                                         pkg),
                                    cancellable, error))
        goto out;
    }

  if (filesystem_package)
    i = 0;
  else
    i = 1;

  for (; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      HifPackage *pkg = (void*)rpmteKey (te);

      g_assert (pkg);

      if (pkg == filesystem_package)
        continue;

      if (!ostree_checkout_package (self->ostreerepo, pkg,
                                    tmprootfs_dfd, ".", devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit,
                                                         pkg),
                                    cancellable, error))
        goto out;
    }

  rpmostree_output_task_end ("done");

  if (!rpmostree_rootfs_prepare_links (tmprootfs_dfd, cancellable, error))
    goto out;

  if (!noscripts)
    {
      for (i = 0; i < n_rpmts_elements; i++)
        {
          rpmte te = rpmtsElement (ordering_ts, i);
          HifPackage *pkg = (void*)rpmteKey (te);

          g_assert (pkg);

          if (!run_posttrans_sync (tmp_metadata_dfd, tmprootfs_dfd, pkg,
                                   self->ignore_scripts,
                                   cancellable, error))
            goto out;
        }
    }

  g_clear_pointer (&ordering_ts, rpmtsFree);

  rpmostree_output_task_begin ("Writing rpmdb");

  if (!glnx_shutil_mkdir_p_at (tmprootfs_dfd, "usr/share/rpm", 0755,
                               cancellable, error))
    goto out;

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
      goto out;

    set_rpm_macro_define ("_dbpath", rpmdb_abspath);
  }

  rpmdb_ts = rpmtsCreate ();
  rpmtsSetVSFlags (rpmdb_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
  rpmtsSetFlags (rpmdb_ts, RPMTRANS_FLAG_JUSTDB);
  rpmtsSetNotifyCallback (rpmdb_ts, ts_callback, &tdata);

  { gpointer k,v;
    GHashTableIter hiter;

    g_hash_table_iter_init (&hiter, pkg_to_ostree_commit);
    while (g_hash_table_iter_next (&hiter, &k, &v))
      {
        HifPackage *pkg = k;

        /* Set noscripts since we already validated them above */
        if (!add_to_transaction (rpmdb_ts, pkg, tmp_metadata_dfd, TRUE, NULL,
                                 cancellable, error))
          goto out;
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
  r = rpmtsRun (rpmdb_ts, NULL, RPMPROB_FILTER_DISKSPACE);
  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to update rpmdb (rpmtsRun code %d)", r);
      goto out;
    }
  if (r > 0)
    {
      if (!hif_rpmts_look_for_problems (rpmdb_ts, error))
        goto out;
    }

  rpmostree_output_task_end ("done");

  if (!rpmostree_rootfs_postprocess_common (tmprootfs_dfd, cancellable, error))
    goto out;

  rpmostree_output_task_begin ("Writing OSTree commit");

  if (!ostree_repo_prepare_transaction (self->ostreerepo, NULL, cancellable, error))
    goto out;

  { glnx_unref_object OstreeMutableTree *mtree = NULL;
    g_autoptr(GFile) root = NULL;
    g_auto(GVariantBuilder) metadata_builder;
    g_autofree char *state_checksum = NULL;
    g_autoptr(GVariant) spec_v = g_variant_ref_sink (rpmostree_treespec_to_variant (self->spec));

    g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.spec", spec_v);

    /* copy the version tag from the parent if present -- XXX: this behaviour
     * should probably be adjustable from a new parameter instead */
    if (parent != NULL)
      {
        g_autoptr(GVariant) commit = NULL;
        g_autofree char *parent_version = NULL;

        if (!ostree_repo_load_commit (self->ostreerepo, parent, &commit, NULL, error))
          goto out;

        parent_version = checksum_version (commit);

        if (parent_version)
          g_variant_builder_add (&metadata_builder, "{sv}", "version",
                                 g_variant_new_string (parent_version));
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
      goto out;

    if (!ostree_repo_write_mtree (self->ostreerepo, mtree, &root, cancellable, error))
      goto out;

    if (!ostree_repo_write_commit (self->ostreerepo, parent, "", "",
                                   g_variant_builder_end (&metadata_builder),
                                   OSTREE_REPO_FILE (root),
                                   &ret_commit_checksum, cancellable, error))
      goto out;

    { const char * ref = rpmostree_treespec_get_ref (self->spec);
      if (ref != NULL)
        ostree_repo_transaction_set_ref (self->ostreerepo, NULL, ref,
                                         ret_commit_checksum);
    }

    if (!ostree_repo_commit_transaction (self->ostreerepo, NULL, cancellable, error))
      goto out;
  }

  rpmostree_output_task_end ("done");

  ret = TRUE;
  if (out_commit)
    *out_commit = g_steal_pointer (&ret_commit_checksum);
 out:
  if (devino_cache)
    ostree_repo_devino_cache_unref (devino_cache);
  if (ordering_ts)
    rpmtsFree (ordering_ts);
  if (rpmdb_ts)
    rpmtsFree (rpmdb_ts);
  if (tmp_metadata_dir_path)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_metadata_dir_path, cancellable, NULL);
  if (commit_modifier)
    ostree_repo_commit_modifier_unref (commit_modifier);
  return ret;
}
