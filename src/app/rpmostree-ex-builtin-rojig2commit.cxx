/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
#include <gio/gunixoutputstream.h>
#include <libdnf/libdnf.h>
#include <sys/mount.h>
#include <stdio.h>
#include <libglnx.h>
#include <rpm/rpmmacro.h>

#include "rpmostree-ex-builtins.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-rojig-assembler.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-libbuiltin.h"
#include "rpmostree-rpm-util.h"

#include "libglnx.h"

static char *opt_repo;
static char *opt_rpmmd_reposdir;
static char *opt_releasever;
static char **opt_enable_rpmmdrepo;
static char *opt_oirpm_version;

static GOptionEntry rojig2commit_option_entries[] = {
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "OSTree repo", "REPO" },
  { "rpmmd-reposd", 'd', 0, G_OPTION_ARG_STRING, &opt_rpmmd_reposdir, "Path to yum.repos.d (rpmmd) config directory", "PATH" },
  { "enablerepo", 'e', 0, G_OPTION_ARG_STRING_ARRAY, &opt_enable_rpmmdrepo, "Enable rpm-md repo with id ID", "ID" },
  { "releasever", 0, 0, G_OPTION_ARG_STRING, &opt_releasever, "Value for $releasever", "RELEASEVER" },
  { "oirpm-version", 'V', 0, G_OPTION_ARG_STRING, &opt_oirpm_version, "Use this specific version of OIRPM", "VERSION" },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  GLnxTmpDir tmpd;
  RpmOstreeContext *ctx;
} RpmOstreeRojig2CommitContext;

static void
rpm_ostree_rojig2commit_context_free (RpmOstreeRojig2CommitContext *ctx)
{
  g_clear_object (&ctx->repo);
  (void) glnx_tmpdir_delete (&ctx->tmpd, NULL, NULL);
  g_free (ctx);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(RpmOstreeRojig2CommitContext, rpm_ostree_rojig2commit_context_free)

/* Initialize a context for converting a rojig to a commit.
 */
static gboolean
rpm_ostree_rojig2commit_context_new (RpmOstreeRojig2CommitContext **out_context,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  g_autoptr(RpmOstreeRojig2CommitContext) self = g_new0 (RpmOstreeRojig2CommitContext, 1);

  self->repo = ostree_repo_open_at (AT_FDCWD, opt_repo, cancellable, error);
  if (!self->repo)
    return FALSE;

  /* Our workdir lives in the repo for command line testing */
  if (!glnx_mkdtempat (ostree_repo_get_dfd (self->repo),
                       "tmp/rpmostree-rojig-XXXXXX", 0700, &self->tmpd, error))
    return FALSE;

  self->ctx = rpmostree_context_new_compose (self->tmpd.fd, self->repo, cancellable, error);
  if (!self->ctx)
    return FALSE;

  DnfContext *dnfctx = rpmostree_context_get_dnf (self->ctx);

  if (opt_rpmmd_reposdir)
    dnf_context_set_repo_dir (dnfctx, opt_rpmmd_reposdir);

  *out_context = util::move_nullify (self);
  return TRUE;
}

static gboolean
impl_rojig2commit (RpmOstreeRojig2CommitContext *self,
                   const char                   *rojig_id,
                   GCancellable                 *cancellable,
                   GError                      **error)
{
  g_autoptr(GKeyFile) tsk = g_key_file_new ();

  g_key_file_set_string (tsk, "tree", "rojig", rojig_id);
  if (opt_oirpm_version)
    g_key_file_set_string (tsk, "tree", "rojig-version", opt_oirpm_version);
  if (opt_releasever)
    g_key_file_set_string (tsk, "tree", "releasever", opt_releasever);
  if (opt_enable_rpmmdrepo)
    g_key_file_set_string_list (tsk, "tree", "repos",
                                (const char *const*)opt_enable_rpmmdrepo,
                                g_strv_length (opt_enable_rpmmdrepo));
  g_autoptr(RpmOstreeTreespec) treespec = rpmostree_treespec_new_from_keyfile (tsk, error);
  if (!treespec)
    return FALSE;

  /* We're also "pure" rojig - this adds assertions that we don't depsolve for example */
  if (!rpmostree_context_setup (self->ctx, NULL, NULL, treespec, cancellable, error))
    return FALSE;
  if (!rpmostree_context_prepare_rojig (self->ctx, FALSE, cancellable, error))
    return FALSE;
  gboolean rojig_changed;
  if (!rpmostree_context_execute_rojig (self->ctx, &rojig_changed, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
rpmostree_ex_builtin_rojig2commit (int             argc,
                                   char          **argv,
                                   RpmOstreeCommandInvocation *invocation,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("REPOID:OIRPM-NAME");
  if (!rpmostree_option_context_parse (context,
                                       rojig2commit_option_entries,
                                       &argc, &argv,
                                       invocation,
                                       cancellable,
                                       NULL, NULL, NULL, NULL, NULL,
                                       error))
    return FALSE;

  if (argc != 2)
   {
      rpmostree_usage_error (context, "REPOID:OIRPM-NAME is required", error);
      return FALSE;
    }

  if (!opt_repo)
    {
      rpmostree_usage_error (context, "--repo must be specified", error);
      return FALSE;
    }

  const char *oirpm = argv[1];

  g_autoptr(RpmOstreeRojig2CommitContext) self = NULL;
  if (!rpm_ostree_rojig2commit_context_new (&self, cancellable, error))
    return FALSE;
  if (!impl_rojig2commit (self, oirpm, cancellable, error))
    return FALSE;

  return TRUE;
}
