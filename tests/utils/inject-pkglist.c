/*
Given a ref, read its pkglist, inject it in a new commit that is for our
purposes identical to the one the ref is pointing to, then reset the ref to that
commit. Essentially, we replace the tip with a copy, except that it has the
pkglist metadata.

This is used by tests that test features that require the new pkglist metadata
and is also really useful for debugging.
*/

#include "config.h"

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib-unix.h>

#include "libglnx.h"
#include "rpmostree-rpm-util.h"

static gboolean
impl (const char *repo_path,
      const char *refspec,
      GError    **error)
{
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

int
main (int argc, char *argv[])
{
  if (argc != 3)
    errx (EXIT_FAILURE, "Usage: %s <repo> <refspec>", argv[0]);

  const char *repo_path = argv[1];
  const char *refspec = argv[2];

  g_autoptr(GError) local_error = NULL;
  if (!impl (repo_path, refspec, &local_error))
    errx (EXIT_FAILURE, "%s", local_error->message);
  g_assert (local_error == NULL);

  return EXIT_SUCCESS;
}
