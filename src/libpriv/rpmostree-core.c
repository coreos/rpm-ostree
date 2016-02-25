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
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <libhif/hif-package.h>
#include <librepo/librepo.h>

#include "rpmostree-core.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"

#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "solv"
#define RPMOSTREE_DIR_LOCK "lock"

struct _RpmOstreeContext {
  GObject parent;
  
  RpmOstreeTreespec *spec;
  HifContext *hifctx;
  OstreeRepo *ostreerepo;
  char *dummy_instroot_path;
};

G_DEFINE_TYPE (RpmOstreeContext, rpmostree_context, G_TYPE_OBJECT)

struct _RpmOstreeInstall {
  GObject parent;

  GPtrArray *packages_requested;
  /* Target state */
  guint n_packages_download_local;
  GPtrArray *packages_to_download;
  guint64 n_bytes_to_fetch;

  /* Current state */
  guint n_packages_fetched;
  guint64 n_bytes_fetched;
};

G_DEFINE_TYPE (RpmOstreeInstall, rpmostree_install, G_TYPE_OBJECT)

struct _RpmOstreeTreespec {
  GObject parent;

  GVariant *spec;
  GVariantDict *dict;
};

G_DEFINE_TYPE (RpmOstreeTreespec, rpmostree_treespec, G_TYPE_OBJECT)

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
rpmostree_install_finalize (GObject *object)
{
  RpmOstreeInstall *self = RPMOSTREE_INSTALL (object);

  g_clear_pointer (&self->packages_to_download, g_ptr_array_unref);
  g_clear_pointer (&self->packages_requested, g_ptr_array_unref);

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
  hif_context_set_cache_age (self->hifctx, G_MAXUINT);
  hif_context_set_cache_dir (self->hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_REPOMD);
  hif_context_set_solv_dir (self->hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_SOLV);
  hif_context_set_lock_dir (self->hifctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);

  hif_context_set_check_disk_space (self->hifctx, FALSE);
  hif_context_set_check_transaction (self->hifctx, FALSE);
  hif_context_set_yumdb_enabled (self->hifctx, FALSE);

  /* A nonsense value because repo files used as input for this tool
   * should never use it.
   */
  hif_context_set_release_ver (self->hifctx, "42");

  return self;
}

RpmOstreeContext *
rpmostree_context_new_unprivileged (int           userroot_dfd,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(RpmOstreeContext) ret = rpmostree_context_new_system (cancellable, error);
  struct stat stbuf;

  if (!ret)
    goto out;

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

/* XXX: or put this in new_system() instead? */
void
rpmostree_context_set_repo (RpmOstreeContext *self,
                            OstreeRepo *repo)
{
  g_set_object (&self->ostreerepo, repo);
}


HifContext *
rpmostree_context_get_hif (RpmOstreeContext *self)
{
  return self->hifctx;
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

GHashTable *
rpmostree_context_get_varsubsts (RpmOstreeContext *context)
{
  GHashTable *r = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_hash_table_insert (r, g_strdup ("basearch"), g_strdup (hif_context_get_base_arch (context->hifctx)));

  return r;
}

static gboolean
enable_one_repo (RpmOstreeContext    *context,
                 GPtrArray           *sources,
                 const char          *reponame,
                 GError             **error)
{
  gboolean ret = FALSE;
  guint i;
  gboolean found = FALSE;

  for (i = 0; i < sources->len; i++)
    {
      HifRepo *src = sources->pdata[i];
      const char *id = hif_repo_get_id (src);

      if (strcmp (reponame, id) != 0)
        continue;
      
      hif_repo_set_enabled (src, HIF_REPO_ENABLED_PACKAGES);
      hif_repo_set_required (src, TRUE);
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
      if (!enable_one_repo (context, sources, *iter, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
gboolean
rpmostree_context_setup (RpmOstreeContext    *self,
                         const char    *installroot,
                         RpmOstreeTreespec *spec,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  GPtrArray *repos = NULL;
  char **enabled_repos = NULL;
  char **instlangs = NULL;
  
  if (installroot)
    hif_context_set_install_root (self->hifctx, installroot);
  else
    {
      glnx_fd_close int dfd = -1; /* Auto close, we just use the path */
      if (!rpmostree_mkdtemp ("/tmp/rpmostree-dummy-instroot-XXXXXX",
                              &self->dummy_instroot_path,
                              &dfd, error))
        goto out;
      hif_context_set_install_root (self->hifctx, self->dummy_instroot_path);
    }

  if (!hif_context_setup (self->hifctx, cancellable, error))
    goto out;

  /* This is what we use as default. */
  set_rpm_macro_define ("_dbpath", "/usr/share/rpm");

  self->spec = g_object_ref (spec);

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
  glnx_console_progress_text_percent (text, percentage);
}

gboolean
rpmostree_context_download_metadata (RpmOstreeContext     *self,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  g_auto(GLnxConsoleRef) console = { 0, };
  gs_unref_object HifState *hifstate = hif_state_new ();

  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

  glnx_console_lock (&console);

  if (!hif_context_setup_sack (self->hifctx, hifstate, error))
    goto out;

  g_signal_handler_disconnect (hifstate, progress_sigid);

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
get_packages_to_download (HifContext  *hifctx,
                          OstreeRepo  *ostreerepo,
                          GPtrArray  **out_packages,
                          guint       *out_n_local,
                          GError     **error)
{
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GPtrArray) packages = NULL;
  g_autoptr(GPtrArray) packages_to_download = NULL;
  GPtrArray *sources = hif_context_get_repos (hifctx);
  guint n_local = 0;
  
  packages = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                    HIF_PACKAGE_INFO_INSTALL,
                                    HIF_PACKAGE_INFO_REINSTALL,
                                    HIF_PACKAGE_INFO_DOWNGRADE,
                                    HIF_PACKAGE_INFO_UPDATE,
                                    -1);
  packages_to_download = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  
  for (i = 0; i < packages->len; i++)
    {
      HifPackage *pkg = packages->pdata[i];
      HifRepo *src = NULL;
      guint j;

      /* get correct package source */

      /* Hackily look up the source...we need a hash table */
      for (j = 0; j < sources->len; j++)
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

      /* this is a local file */

      if (ostreerepo)
        {
          g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
          g_autofree char *cached_rev = NULL; 

          if (!ostree_repo_resolve_rev (ostreerepo, cachebranch, TRUE, &cached_rev, error))
            goto out;

          if (cached_rev)
            continue;
        }
      else if (!pkg_is_local (pkg))
        {
          const char *cachepath;
          
          cachepath = hif_package_get_filename (pkg);

          /* Right now we're not re-checksumming cached RPMs, we
           * assume they are valid.  This is a change from the current
           * libhif behavior, but I think it's right.  We should
           * record validity once, then ensure it's immutable after
           * that - which is what happens with the ostree commits
           * above.
           */
          if (g_file_test (cachepath, G_FILE_TEST_EXISTS))
            continue;
        }
      else
        n_local++;

      g_ptr_array_add (packages_to_download, g_object_ref (pkg));
    }

  ret = TRUE;
  *out_packages = g_steal_pointer (&packages_to_download);
  *out_n_local = n_local;
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

  printf ("%s", "Resolving dependencies: ");
  fflush (stdout);

  if (!hif_goal_depsolve (hif_context_get_goal (hifctx), HIF_INSTALL, error))
    {
      printf ("%s", "failed\n");
      goto out;
    }
  printf ("%s", "done\n");

  if (!get_packages_to_download (hifctx, self->ostreerepo, &ret_install->packages_to_download,
                                 &ret_install->n_packages_download_local,
                                 error))
    goto out;

  rpmostree_print_transaction (hifctx);
  g_print ("\n  Need to download %u packages\n",
           ret_install->packages_to_download->len - ret_install->n_packages_download_local);

  ret = TRUE;
  *out_install = g_steal_pointer (&ret_install);
 out:
  return ret;
}

struct GlobalDownloadState {
  RpmOstreeInstall *install;
  HifState *hifstate;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

struct PkgDownloadState {
  struct GlobalDownloadState *gdlstate;
  gboolean added_total;
  guint64 last_bytes_fetched;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

static int
package_download_update_state_cb (void *user_data,
				  gdouble total_to_download,
				  gdouble now_downloaded)
{
  struct PkgDownloadState *dlstate = user_data;
  RpmOstreeInstall *install = dlstate->gdlstate->install;
  if (!dlstate->added_total)
    {
      dlstate->added_total = TRUE;
      install->n_bytes_to_fetch += (guint64) total_to_download;
    }

  install->n_bytes_fetched += ((guint64)now_downloaded) - dlstate->last_bytes_fetched;
  dlstate->last_bytes_fetched = ((guint64)now_downloaded);
  return LR_CB_OK;
}

static int
mirrorlist_failure_cb (void *user_data,
		       const char *message,
		       const char *url)
{
  struct PkgDownloadState *dlstate = user_data;
  struct GlobalDownloadState *gdlstate = dlstate->gdlstate;

  if (gdlstate->last_mirror_url)
    goto out;

  gdlstate->last_mirror_url = g_strdup (url);
  gdlstate->last_mirror_failure_message = g_strdup (message);
 out:
  return LR_CB_OK;
}

static inline void
hif_state_assert_done (HifState *hifstate)
{
  gboolean r;
  r = hif_state_done (hifstate, NULL);
  g_assert (r);
}

static int
package_download_complete_cb (void *user_data,
                              LrTransferStatus status,
                              const char *msg)
{
  struct PkgDownloadState *dlstate = user_data;
  switch (status)
    {
    case LR_TRANSFER_SUCCESSFUL:
    case LR_TRANSFER_ALREADYEXISTS:
      dlstate->gdlstate->install->n_packages_fetched++;
      hif_state_assert_done (dlstate->gdlstate->hifstate);
      return LR_CB_OK;
    case LR_TRANSFER_ERROR:
      return LR_CB_ERROR; 
    default:
      g_assert_not_reached ();
      return LR_CB_ERROR;
    }
}

static int
ptrarray_sort_compare_strings (gconstpointer ap,
                               gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

/* Generate a checksum from a goal in a repeatable fashion (ordered
 * hash of component NEVRAs).  This can be used to see if the goal has
 * changed from a previous one efficiently.
 */
void
rpmostree_hif_add_checksum_goal (GChecksum *checksum,
                                 HyGoal     goal)
{
  g_autoptr(GPtrArray) pkglist = NULL;
  guint i;
  g_autoptr(GPtrArray) nevras = g_ptr_array_new_with_free_func (g_free);

  pkglist = hy_goal_list_installs (goal, NULL);
  g_assert (pkglist);
  for (i = 0; i < pkglist->len; i++)
    {
      HifPackage *pkg = pkglist->pdata[i];
      g_ptr_array_add (nevras, hif_package_get_nevra (pkg));
    }

  g_ptr_array_sort (nevras, ptrarray_sort_compare_strings);
    
  for (i = 0; i < nevras->len; i++)
    {
      const char *nevra = nevras->pdata[i];
      g_checksum_update (checksum, (guint8*)nevra, strlen (nevra));
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

/**
 * hif_source_checksum_hy_to_lr:
 **/
static LrChecksumType
hif_source_checksum_hy_to_lr (int checksum_hy)
{
	if (checksum_hy == G_CHECKSUM_MD5)
		return LR_CHECKSUM_MD5;
	if (checksum_hy == G_CHECKSUM_SHA1)
		return LR_CHECKSUM_SHA1;
	if (checksum_hy == G_CHECKSUM_SHA256)
		return LR_CHECKSUM_SHA256;
	return LR_CHECKSUM_UNKNOWN;
}

static gboolean
source_download_packages (HifRepo *source,
                          GPtrArray *packages,
                          RpmOstreeInstall *install,
                          int        target_dfd,
                          HifState  *state,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  char *checksum_str = NULL;
  const unsigned char *checksum;
  guint i;
  int checksum_type;
  LrPackageTarget *target = NULL;
  GSList *package_targets = NULL;
  struct GlobalDownloadState gdlstate = { 0, };
  g_autoptr(GArray) pkg_dlstates = g_array_new (FALSE, TRUE, sizeof (struct PkgDownloadState));
  LrHandle *handle;
  g_autoptr(GError) error_local = NULL;
  g_autofree char *target_dir = NULL;

  handle = hif_repo_get_lr_handle (source);

  gdlstate.install = install;
  gdlstate.hifstate = state;

  g_array_set_size (pkg_dlstates, packages->len);
  hif_state_set_number_steps (state, packages->len);

  for (i = 0; i < packages->len; i++)
    {
      g_autofree char *target_dir = NULL;
      HifPackage *pkg = packages->pdata[i];
      struct PkgDownloadState *dlstate;

      if (pkg_is_local (pkg))
        continue;

      if (target_dfd == -1)
        {
          target_dir = g_build_filename (hif_repo_get_location (source), "/packages/", NULL);
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
            goto out;
        }
      else
        target_dir = glnx_fdrel_abspath (target_dfd, ".");
      
      checksum = hif_package_get_chksum (pkg, &checksum_type);
      checksum_str = hy_chksum_str (checksum, checksum_type);
      
      dlstate = &g_array_index (pkg_dlstates, struct PkgDownloadState, i);
      dlstate->gdlstate = &gdlstate;

      target = lr_packagetarget_new_v2 (handle,
                                        hif_package_get_location (pkg),
                                        target_dir,
                                        hif_source_checksum_hy_to_lr (checksum_type),
                                        checksum_str,
                                        0, /* size unknown */
                                        hif_package_get_baseurl (pkg),
                                        TRUE,
                                        package_download_update_state_cb,
                                        dlstate,
                                        package_download_complete_cb,
                                        mirrorlist_failure_cb,
                                        error);
      if (target == NULL)
        goto out;
	
      package_targets = g_slist_prepend (package_targets, target);
    }

  _rpmostree_reset_rpm_sighandlers ();

  if (!lr_download_packages (package_targets, LR_PACKAGEDOWNLOAD_FAILFAST, &error_local))
    {
      if (g_error_matches (error_local,
                           LR_PACKAGE_DOWNLOADER_ERROR,
                           LRE_ALREADYDOWNLOADED))
        {
          /* ignore */
          g_clear_error (&error_local);
        }
      else
        {
          if (gdlstate.last_mirror_failure_message)
            {
              g_autofree gchar *orig_message = error_local->message;
              error_local->message = g_strconcat (orig_message, "; Last error: ", gdlstate.last_mirror_failure_message, NULL);
            }
          g_propagate_error (error, g_steal_pointer (&error_local));
          goto out;
        }
  } 

  ret = TRUE;
 out:
  g_free (gdlstate.last_mirror_failure_message);
  g_free (gdlstate.last_mirror_url);
  g_slist_free_full (package_targets, (GDestroyNotify)lr_packagetarget_free);
  g_free (checksum_str);
  return ret;
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
rpmostree_context_download_rpms (RpmOstreeContext     *ctx,
                                 int             target_dfd,
                                 RpmOstreeInstall *install,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = ctx->hifctx;
  g_auto(GLnxConsoleRef) console = { 0, };
  guint progress_sigid;
  GHashTableIter hiter;
  gpointer key, value;

  glnx_console_lock (&console);

  { g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (hifctx, install);

    g_hash_table_iter_init (&hiter, source_to_packages);
    while (g_hash_table_iter_next (&hiter, &key, &value))
      {
        HifRepo *src = key;
        GPtrArray *src_packages = value;
        gs_unref_object HifState *hifstate = hif_state_new ();
        g_autofree char *prefix = g_strconcat ("Downloading packages (", hif_repo_get_id (src), ")", NULL); 

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           prefix);
      
        if (!source_download_packages (src, src_packages, install, target_dfd, hifstate,
                                       cancellable, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }
  }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_package (OstreeRepo   *ostreerepo,
                    int           tmpdir_dfd,
                    HifContext   *hifctx,
                    HifPackage *    pkg,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *ostree_commit = NULL;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  const char *pkg_relpath;

  if (pkg_is_local (pkg))
    {
      tmpdir_dfd = AT_FDCWD;
      pkg_relpath = hif_package_get_filename (pkg);
    }
  else
    pkg_relpath = glnx_basename (hif_package_get_location (pkg)); 
   
  /* TODO - tweak the unpacker flags for containers */
  unpacker = rpmostree_unpacker_new_at (tmpdir_dfd, pkg_relpath,
                                        RPMOSTREE_UNPACKER_FLAGS_ALL,
                                        error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostreerepo, NULL, &ostree_commit,
                                            cancellable, error))
    {
      g_autofree char *nevra = hif_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
      goto out;
    }
   
  if (!pkg_is_local (pkg))
    {
      if (TEMP_FAILURE_RETRY (unlinkat (tmpdir_dfd, pkg_relpath, 0)) < 0)
        {
          glnx_set_error_from_errno (error);
          g_prefix_error (error, "Deleting %s: ", pkg_relpath);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_context_download_import (RpmOstreeContext *self,
                                   RpmOstreeInstall  *install,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = self->hifctx;
  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_fd_close int pkg_tempdir_dfd = -1;
  g_autofree char *pkg_tempdir = NULL;
  guint progress_sigid;
  GHashTableIter hiter;
  gpointer key, value;
  guint i;

  g_return_val_if_fail (self->ostreerepo != NULL, FALSE);

  glnx_console_lock (&console);

  if (!rpmostree_mkdtemp ("/var/tmp/rpmostree-import-XXXXXX", &pkg_tempdir, &pkg_tempdir_dfd, error))
    goto out;

  { g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (hifctx, install);
    
    g_hash_table_iter_init (&hiter, source_to_packages);
    while (g_hash_table_iter_next (&hiter, &key, &value))
      {
        glnx_unref_object HifState *hifstate = hif_state_new ();
        HifRepo *src = key;
        GPtrArray *src_packages = value;
        g_autofree char *prefix = g_strconcat ("Downloading from ", hif_repo_get_id (src), NULL); 

        if (hif_repo_is_local (src))
          continue;

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           prefix);
        
        if (!source_download_packages (src, src_packages, install, pkg_tempdir_dfd, hifstate,
                                       cancellable, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }
  }

  { 
    glnx_unref_object HifState *hifstate = hif_state_new ();

    hif_state_set_number_steps (hifstate, install->packages_to_download->len);
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed), 
                                       "Importing packages:");

    for (i = 0; i < install->packages_to_download->len; i++)
      {
        HifPackage *pkg = install->packages_to_download->pdata[i];
        if (!import_one_package (self->ostreerepo, pkg_tempdir_dfd, hifctx, pkg,
                                 cancellable, error))
          goto out;
        hif_state_assert_done (hifstate);
      }

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
ostree_checkout_package (int           dfd,
                         const char   *path,
                         HifContext   *hifctx,
                         HifPackage *    pkg,
                         OstreeRepo   *ostreerepo,
                         OstreeRepoDevInoCache *devino_cache,
                         const char   *pkg_ostree_commit,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

  opts.devino_to_csum_cache = devino_cache;

  /* For now... to be crash safe we'd need to duplicate some of the
   * boot-uuid/fsync gating at a higher level.
   */
  opts.disable_fsync = TRUE;

  if (!ostree_repo_checkout_tree_at (ostreerepo, &opts, dfd, path,
                                     pkg_ostree_commit, cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (error && *error)
    g_prefix_error (error, "Unpacking %s: ", hif_package_get_nevra (pkg));
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
        const char *nevra = (char *) key;
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

static gboolean
add_to_transaction (rpmts  ts,
                    HifPackage *pkg,
                    int tmp_metadata_dfd,
                    GError **error)
{
  gboolean ret = FALSE;
  int r;
  Header hdr = NULL;
  glnx_fd_close int metadata_fd = -1;

  if ((metadata_fd = openat (tmp_metadata_dfd, hif_package_get_nevra (pkg), O_RDONLY | O_CLOEXEC)) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!rpmostree_unpacker_read_metainfo (metadata_fd, &hdr, NULL, NULL, error))
    goto out;
          
  r = rpmtsAddInstallElement (ts, hdr, (char*)hif_package_get_nevra (pkg), TRUE, NULL);
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

gboolean
rpmostree_context_assemble_commit (RpmOstreeContext *self,
                                   int               tmpdir_dfd,
                                   const char       *name,
                                   char            **out_commit,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  gboolean ret = FALSE;
  HifContext *hifctx = self->hifctx;
  guint progress_sigid;
  TransactionData tdata = { 0, -1 };
  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_unref_object HifState *hifstate = NULL;
  rpmts ordering_ts = NULL;
  rpmts rpmdb_ts = NULL;
  guint i, n_rpmts_elements;
  g_autoptr(GHashTable) nevra_to_pkg =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_object_unref);
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, (GDestroyNotify)g_free);
  HifPackage *filesystem_package = NULL;   /* It's special... */
  int r;
  glnx_fd_close int rootfs_fd = -1;
  char *tmp_metadata_dir_path = NULL;
  glnx_fd_close int tmp_metadata_dfd = -1;
  OstreeRepoDevInoCache *devino_cache = NULL;
  g_autofree char *workdir_path = NULL;
  g_autofree char *ret_commit_checksum = NULL;

  glnx_console_lock (&console);

  if (!rpmostree_mkdtemp ("/tmp/rpmostree-metadata-XXXXXX", &tmp_metadata_dir_path,
                          &tmp_metadata_dfd, error))
    goto out;
  tdata.tmp_metadata_dfd = tmp_metadata_dfd;

  workdir_path = g_strdup ("rpmostree-commit-XXXXXX");
  if (!glnx_mkdtempat (tmpdir_dfd, workdir_path, 0755, error))
    goto out;

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
                                          HIF_PACKAGE_INFO_REINSTALL,
                                          HIF_PACKAGE_INFO_DOWNGRADE,
                                          HIF_PACKAGE_INFO_UPDATE,
                                          -1);

    for (i = 0; i < package_list->len; i++)
      {
        HifPackage *pkg = package_list->pdata[i];
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        g_autofree char *cachebranch = rpmostree_get_cache_branch_pkg (pkg);
        g_autofree char *cached_rev = NULL; 
        g_autoptr(GVariant) pkg_commit = NULL;
        g_autoptr(GVariant) header_variant = NULL;

        if (!ostree_repo_resolve_rev (self->ostreerepo, cachebranch, FALSE, &cached_rev, error))
          goto out;

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

          if (!glnx_file_replace_contents_at (tmp_metadata_dfd, hif_package_get_nevra (pkg),
                                              g_variant_get_data (header_variant),
                                              g_variant_get_size (header_variant),
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            goto out;

          if (!add_to_transaction (ordering_ts, pkg, tmp_metadata_dfd, error))
            goto out;
        }

        g_hash_table_insert (nevra_to_pkg, (char*)hif_package_get_nevra (pkg), g_object_ref (pkg));
        g_hash_table_insert (pkg_to_ostree_commit, g_object_ref (pkg), g_steal_pointer (&cached_rev));

        if (strcmp (hif_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = g_object_ref (pkg);
      }
  }

  rpmtsOrder (ordering_ts);
  _rpmostree_reset_rpm_sighandlers ();

  hifstate = hif_state_new ();
  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Unpacking:");


  n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);
  hif_state_set_number_steps (hifstate, n_rpmts_elements);

  devino_cache = ostree_repo_devino_cache_new ();

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
      if (!ostree_checkout_package (tmpdir_dfd, workdir_path, hifctx, filesystem_package,
                                    self->ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, filesystem_package),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }
  else
    {
      /* Otherwise, we unpack the first package to get the initial
       * rootfs dir.
       */
      rpmte te = rpmtsElement (ordering_ts, 0);
      const char *tekey = rpmteKey (te);
      HifPackage *pkg = g_hash_table_lookup (nevra_to_pkg, tekey);

      if (!ostree_checkout_package (tmpdir_dfd, workdir_path, hifctx, pkg,
                                    self->ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, pkg),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }

  if (!glnx_opendirat (tmpdir_dfd, workdir_path, FALSE, &rootfs_fd, error))
    goto out;

  if (filesystem_package)
    i = 0;
  else
    i = 1;

  for (; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      const char *tekey = rpmteKey (te);
      HifPackage *pkg = g_hash_table_lookup (nevra_to_pkg, tekey);

      if (pkg == filesystem_package)
        continue;

      if (!ostree_checkout_package (rootfs_fd, ".", hifctx, pkg, self->ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, pkg),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }

  glnx_console_unlock (&console);

  g_clear_pointer (&ordering_ts, rpmtsFree);

  g_print ("Writing rpmdb...\n");

  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/share/rpm", 0755, cancellable, error))
    goto out;

  /* Now, we use the separate rpmdb ts which *doesn't* have a rootdir
   * set, because if it did rpmtsRun() would try to chroot which it
   * can't, even though we're not trying to run %post scripts now.
   *
   * Instead, this rpmts has the dbpath as absolute.
   */
  { g_autofree char *rpmdb_abspath = glnx_fdrel_abspath (rootfs_fd, "usr/share/rpm");
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

        if (!add_to_transaction (rpmdb_ts, pkg, tmp_metadata_dfd, error))
          goto out;
      }
  }

  rpmtsOrder (rpmdb_ts);
  r = rpmtsRun (rpmdb_ts, NULL, 0);
  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to update rpmdb (rpmtsRun code %d)", r);
      goto out;
    }

  g_print ("Writing rpmdb...done\n");

  if (!rpmostree_rootfs_postprocess_common (rootfs_fd, cancellable, error))
    goto out;

  g_signal_handler_disconnect (hifstate, progress_sigid);

  g_print ("Writing OSTree commit...\n");

  if (!ostree_repo_prepare_transaction (self->ostreerepo, NULL, cancellable, error))
    goto out;

  { glnx_unref_object OstreeMutableTree *mtree = NULL;
    g_autoptr(GFile) root = NULL;
    g_auto(GVariantBuilder) metadata_builder;
    g_autofree char *state_checksum = NULL;
    OstreeRepoCommitModifier *commit_modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE, NULL, NULL, NULL);
    g_autoptr(GVariant) spec_v = g_variant_ref_sink (rpmostree_treespec_to_variant (self->spec));

    g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

    g_variant_builder_add (&metadata_builder, "{sv}", "rpmostree.spec", spec_v);

    state_checksum = rpmostree_context_get_state_sha512 (self);

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.state-sha512",
                           g_variant_new_string (state_checksum));

    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

    mtree = ostree_mutable_tree_new ();

    if (!ostree_repo_write_dfd_to_mtree (self->ostreerepo, rootfs_fd, ".", mtree,
                                         commit_modifier, cancellable, error))
      goto out;

    if (!ostree_repo_write_mtree (self->ostreerepo, mtree, &root, cancellable, error))
      goto out;

    if (!ostree_repo_write_commit (self->ostreerepo, NULL, "", "",
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

    /* FIXME memleak on error, need g_autoptr for this */
    ostree_repo_commit_modifier_unref (commit_modifier);
  }

  g_print ("Writing OSTree commit...done\n");

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
  if (workdir_path)
    (void) glnx_shutil_rm_rf_at (tmpdir_dfd, workdir_path, cancellable, NULL);
  return ret;
}
