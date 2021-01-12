/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2019 Red Hat Inc.
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

/* These are utilities useful for debugging and tests. This is a bit of a hack for now to
 * avoid bundling it. We could split them out in the future if we grow a -tests subpackage.
 */

#include "config.h"

#include "rpmostree-builtins.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-rust.h"
#include "rpmostree-cxxrs.h"

gboolean
rpmostree_testutils_builtin_inject_pkglist (int argc, char **argv,
                                            RpmOstreeCommandInvocation *invocation,
                                            GCancellable *cancellable, GError **error);

static RpmOstreeCommand testutils_subcommands[] = {
  { "inject-pkglist", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    NULL, rpmostree_testutils_builtin_inject_pkglist },
  // Avoid adding other commands here - write them in Rust in testutils.rs
  { NULL, (RpmOstreeBuiltinFlags)0, NULL, NULL }
};

gboolean
rpmostree_builtin_testutils (int argc, char **argv,
                             RpmOstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  // See above; avoid adding other commands here - write them in Rust in testutils.rs
  if (argc >= 2 && g_str_equal (argv[1], "inject-pkglist"))
    return rpmostree_handle_subcommand (argc, argv, testutils_subcommands,
                                        invocation, cancellable, error);
  else
    {
      rust::Vec<rust::String> rustargv;
      for (int i = 0; i < argc; i++)
        rustargv.push_back(std::string(argv[i]));
      rpmostreecxx::testutils_entrypoint (rustargv);
      return TRUE;
    }
}

/*
Given a ref, read its pkglist, inject it in a new commit that is for our
purposes identical to the one the ref is pointing to, then reset the ref to that
commit. Essentially, we replace the tip with a copy, except that it has the
pkglist metadata.

This is used by tests that test features that require the new pkglist metadata
and is also really useful for debugging.
*/

gboolean
rpmostree_testutils_builtin_inject_pkglist (int argc, char **argv,
                                            RpmOstreeCommandInvocation *invocation,
                                            GCancellable *cancellable, GError **error)
{
  if (argc != 3)
    return glnx_throw (error, "Usage: rpm-ostree inject-pkglist <REPO> <REFSPEC>");

  const char *repo_path = argv[1];
  const char *refspec = argv[2];

  g_autofree char *remote = NULL;
  g_autofree char *ref = NULL;
  if (!ostree_parse_refspec (refspec, &remote, &ref, error))
    return FALSE;

  g_autoptr(OstreeRepo) repo = ostree_repo_open_at (AT_FDCWD, repo_path, NULL, error);
  if (!repo)
    return FALSE;

  g_autofree char *checksum = NULL;
  if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &checksum, error))
    return FALSE;

  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, checksum, &commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) meta_dict = g_variant_dict_new (meta);
  if (g_variant_dict_contains (meta_dict, "rpmostree.rpmdb.pkglist"))
    {
      g_print ("Refspec '%s' already has pkglist metadata; exiting.\n", refspec);
      return TRUE;
    }

  /* just an easy way to checkout the rpmdb */
  g_autoptr(RpmOstreeRefSack) rsack =
    rpmostree_get_refsack_for_commit (repo, checksum, NULL, error);
  if (!rsack)
    return FALSE;
  g_assert (rsack->tmpdir.initialized);

  g_autoptr(GVariant) pkglist = NULL;
  if (!rpmostree_create_rpmdb_pkglist_variant (rsack->tmpdir.fd, ".", &pkglist, NULL, error))
    return FALSE;

  g_variant_dict_insert_value (meta_dict, "rpmostree.rpmdb.pkglist", pkglist);
  g_autoptr(GVariant) new_meta = g_variant_ref_sink (g_variant_dict_end (meta_dict));

  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_read_commit (repo,  checksum, &root, NULL, NULL, error))
    return FALSE;

  g_autofree char *new_checksum = NULL;
  g_autofree char *parent = ostree_commit_get_parent (commit);
  if (!ostree_repo_write_commit (repo, parent, "", "", new_meta, OSTREE_REPO_FILE (root),
                                 &new_checksum, NULL, error))
    return FALSE;

  if (!ostree_repo_set_ref_immediate (repo, remote, ref, new_checksum, NULL, error))
    return FALSE;

  g_print("%s => %s\n", refspec, new_checksum);
  return TRUE;
}
