#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-core.h"
#include "rpmostree-importer.h"
#include "libtest.h"

static void
test_bsearch_str(void)
{
  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(st)"));
  g_variant_builder_add (&builder, "(st)", "armadillo", 0);
  g_variant_builder_add (&builder, "(st)", "bunny", 1);
  g_variant_builder_add (&builder, "(st)", "bunny", 2);
  g_variant_builder_add (&builder, "(st)", "chipmunk", 3);
  g_variant_builder_add (&builder, "(st)", "chipmunk", 4);
  g_variant_builder_add (&builder, "(st)", "chipmunk", 5);
  g_variant_builder_add (&builder, "(st)", "dung beetle", 6);
  g_variant_builder_add (&builder, "(st)", "earwig", 7);
  g_variant_builder_add (&builder, "(st)", "earwig", 8);
  g_autoptr(GVariant) cool_animals = g_variant_ref_sink (g_variant_builder_end (&builder));

  int idx;
  g_assert (rpmostree_variant_bsearch_str (cool_animals, "armadillo", &idx));
  g_assert_cmpint (idx, ==, 0);
  g_assert (rpmostree_variant_bsearch_str (cool_animals, "bunny", &idx));
  g_assert_cmpint (idx, ==, 1);
  g_assert (rpmostree_variant_bsearch_str (cool_animals, "chipmunk", &idx));
  g_assert_cmpint (idx, ==, 3);
  g_assert (rpmostree_variant_bsearch_str (cool_animals, "dung beetle", &idx));
  g_assert_cmpint (idx, ==, 6);
  g_assert (rpmostree_variant_bsearch_str (cool_animals, "earwig", &idx));
  g_assert_cmpint (idx, ==, 7);
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "aaaa", &idx));
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "armz", &idx));
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "bunz", &idx));
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "chiz", &idx));
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "dunz", &idx));
  g_assert (!rpmostree_variant_bsearch_str (cool_animals, "earz", &idx));
}

static void
test_variant_to_nevra(void)
{
  gboolean ret = FALSE;
  GError *error = NULL;

  g_autoptr(OstreeRepo) repo =
    ostree_repo_create_at (AT_FDCWD, "repo", OSTREE_REPO_MODE_BARE_USER,
                           NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (repo);

  const char *nevra = "foo-1.0-1.x86_64";
  const char *name = "foo";
  guint64 epoch = 0;
  const char *version = "1.0";
  const char *release = "1";
  const char *arch = "x86_64";

  ret = rot_test_run_libtest ("build_rpm foo", &error);
  g_assert_no_error (error);
  g_assert (ret);

  { g_auto(RpmOstreeRepoAutoTransaction) txn = { 0, };
    /* Note use of commit-on-failure */
    rpmostree_repo_auto_transaction_start (&txn, repo, TRUE, NULL, &error);
    g_assert_no_error (error);

    g_autoptr(RpmOstreeImporter) importer = NULL;
    g_autofree char *foo_rpm = g_strdup_printf ("yumrepo/packages/%s/%s.rpm", arch, nevra);
    glnx_autofd int foo_fd = -1;
    glnx_openat_rdonly (AT_FDCWD, foo_rpm, TRUE, &foo_fd, &error);
    g_assert_no_error (error);
    importer = rpmostree_importer_new_take_fd (&foo_fd, repo, NULL, (RpmOstreeImporterFlags)0, NULL, &error);
    g_assert_no_error (error);
    g_assert (importer);

    ret = rpmostree_importer_run (importer, NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert (ret);

    ostree_repo_commit_transaction (repo, NULL, NULL, &error);
    g_assert_no_error (error);
    txn.initialized = FALSE;
  }

  g_autoptr(GVariant) header = NULL;
  ret = rpmostree_pkgcache_find_pkg_header (repo, nevra, NULL, &header, NULL, &error);
  g_assert_no_error (error);
  g_assert (ret);

  g_autofree char *tname;
  guint64 tepoch;
  g_autofree char *tversion;
  g_autofree char *trelease;
  g_autofree char *tarch;

  ret = rpmostree_decompose_nevra (nevra, &tname, &tepoch, &tversion,
                                   &trelease, &tarch, &error);
  g_assert_no_error (error);
  g_assert (ret);

  g_assert_cmpstr (tname, ==, name);
  g_assert_cmpuint (tepoch, ==, epoch);
  g_assert_cmpstr (tversion, ==, version);
  g_assert_cmpstr (trelease, ==, release);
  g_assert_cmpstr (tarch, ==, arch);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/utils/bsearch_str", test_bsearch_str);
  g_test_add_func ("/importer/variant_to_nevra", test_variant_to_nevra);

  return g_test_run ();
}
