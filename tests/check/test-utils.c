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
test_one_cache_branch_to_nevra (const char *cache_branch,
                                const char *expected_nevra)
{
  g_autofree char *actual_nevra =
    rpmostree_cache_branch_to_nevra (cache_branch);
  g_print ("comparing %s to %s\n", expected_nevra, actual_nevra);
  g_assert (g_str_equal (expected_nevra, actual_nevra));

  g_autofree char *actual_branch = NULL;
  g_assert (rpmostree_nevra_to_cache_branch (expected_nevra, &actual_branch, NULL));
  g_assert (g_str_equal (cache_branch, actual_branch));
}

static void
test_cache_branch_to_nevra (void)
{
  /* pkgs imported from doing install foo git vim-enhanced and outputs of
   * install and ostree refs massaged with sort and paste and column --table */
  test_one_cache_branch_to_nevra ("rpmostree/pkg/foo/1.0-1.x86__64",                             "foo-1.0-1.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/git/1.8.3.1-6.el7__2.1.x86__64",                "git-1.8.3.1-6.el7_2.1.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/gpm-libs/1.20.7-5.el7.x86__64",                 "gpm-libs-1.20.7-5.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/libgnome-keyring/3.8.0-3.el7.x86__64",          "libgnome-keyring-3.8.0-3.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl/4_3A5.16.3-291.el7.x86__64",               "perl-4:5.16.3-291.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Carp/1.26-244.el7.noarch",                 "perl-Carp-1.26-244.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-constant/1.27-2.el7.noarch",               "perl-constant-1.27-2.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Encode/2.51-7.el7.x86__64",                "perl-Encode-2.51-7.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Error/1_3A0.17020-2.el7.noarch",           "perl-Error-1:0.17020-2.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Exporter/5.68-3.el7.noarch",               "perl-Exporter-5.68-3.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-File-Path/2.09-2.el7.noarch",              "perl-File-Path-2.09-2.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-File-Temp/0.23.01-3.el7.noarch",           "perl-File-Temp-0.23.01-3.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Filter/1.49-3.el7.x86__64",                "perl-Filter-1.49-3.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Getopt-Long/2.40-2.el7.noarch",            "perl-Getopt-Long-2.40-2.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Git/1.8.3.1-6.el7__2.1.noarch",            "perl-Git-1.8.3.1-6.el7_2.1.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-HTTP-Tiny/0.033-3.el7.noarch",             "perl-HTTP-Tiny-0.033-3.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-libs/4_3A5.16.3-291.el7.x86__64",          "perl-libs-4:5.16.3-291.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-macros/4_3A5.16.3-291.el7.x86__64",        "perl-macros-4:5.16.3-291.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-parent/1_3A0.225-244.el7.noarch",          "perl-parent-1:0.225-244.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-PathTools/3.40-5.el7.x86__64",             "perl-PathTools-3.40-5.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Pod-Escapes/1_3A1.04-291.el7.noarch",      "perl-Pod-Escapes-1:1.04-291.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-podlators/2.5.1-3.el7.noarch",             "perl-podlators-2.5.1-3.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Pod-Perldoc/3.20-4.el7.noarch",            "perl-Pod-Perldoc-3.20-4.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Pod-Simple/1_3A3.28-4.el7.noarch",         "perl-Pod-Simple-1:3.28-4.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Pod-Usage/1.63-3.el7.noarch",              "perl-Pod-Usage-1.63-3.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Scalar-List-Utils/1.27-248.el7.x86__64",   "perl-Scalar-List-Utils-1.27-248.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Socket/2.010-4.el7.x86__64",               "perl-Socket-2.010-4.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Storable/2.45-3.el7.x86__64",              "perl-Storable-2.45-3.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-TermReadKey/2.30-20.el7.x86__64",          "perl-TermReadKey-2.30-20.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Text-ParseWords/3.29-4.el7.noarch",        "perl-Text-ParseWords-3.29-4.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-threads/1.87-4.el7.x86__64",               "perl-threads-1.87-4.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-threads-shared/1.43-6.el7.x86__64",        "perl-threads-shared-1.43-6.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Time-HiRes/4_3A1.9725-3.el7.x86__64",      "perl-Time-HiRes-4:1.9725-3.el7.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/perl-Time-Local/1.2300-2.el7.noarch",           "perl-Time-Local-1.2300-2.el7.noarch");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/vim-common/2_3A7.4.160-1.el7__3.1.x86__64",     "vim-common-2:7.4.160-1.el7_3.1.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/vim-enhanced/2_3A7.4.160-1.el7__3.1.x86__64",   "vim-enhanced-2:7.4.160-1.el7_3.1.x86_64");
  test_one_cache_branch_to_nevra ("rpmostree/pkg/vim-filesystem/2_3A7.4.160-1.el7__3.1.x86__64", "vim-filesystem-2:7.4.160-1.el7_3.1.x86_64");
}

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
    importer = rpmostree_importer_new_take_fd (&foo_fd, repo, NULL, 0, NULL, &error);
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

  g_test_add_func ("/utils/cachebranch_to_nevra", test_cache_branch_to_nevra);
  g_test_add_func ("/utils/bsearch_str", test_bsearch_str);
  g_test_add_func ("/importer/variant_to_nevra", test_variant_to_nevra);

  return g_test_run ();
}
