#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-util.h"
#include "rpmostree-core.h"
#include "rpmostree-unpacker.h"
#include "libtest.h"

static void
test_substs_eq (const char *str,
                GHashTable *substs,
                const char *expected_str)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *res = _rpmostree_varsubst_string (str, substs, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (expected_str, ==, res);
}

static void
test_substs_err (const char *str,
                 GHashTable *substs,
                 const char *expected_err)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *res = _rpmostree_varsubst_string (str, substs, &error);
  g_assert_null (res);
  g_assert (error != NULL);
  g_assert (strstr (error->message, expected_err));
}

static void
test_varsubst_string (void)
{
  g_autoptr(GHashTable) substs1 = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (substs1, "basearch", "bacon");
  g_hash_table_insert (substs1, "v", "42");

  test_substs_eq ("${basearch}", substs1, "bacon");
  test_substs_eq ("foo/${basearch}/bar", substs1, "foo/bacon/bar");
  test_substs_eq ("${basearch}/bar", substs1, "bacon/bar");
  test_substs_eq ("foo/${basearch}", substs1, "foo/bacon");
  test_substs_eq ("foo/${basearch}/${v}/bar", substs1, "foo/bacon/42/bar");
  test_substs_eq ("${v}", substs1, "42");

  g_autoptr(GHashTable) substs_empty = g_hash_table_new (g_str_hash, g_str_equal);
  static const char unknown_v[] = "Unknown variable reference ${v}";
  test_substs_err ("${v}", substs_empty, unknown_v);
  test_substs_err ("foo/${v}/bar", substs_empty, unknown_v);

  static const char unclosed_err[] = "Unclosed variable";
  test_substs_err ("${", substs_empty, unclosed_err);
  test_substs_err ("foo/${", substs_empty, unclosed_err);
}

static void
test_one_cache_branch_to_nevra (const char *cache_branch,
                                const char *expected_nevra)
{
  g_autofree char *actual_nevra =
    rpmostree_cache_branch_to_nevra (cache_branch);
  g_print ("comparing %s to %s\n", expected_nevra, actual_nevra);
  g_assert (g_str_equal (expected_nevra, actual_nevra));
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
test_variant_to_nevra(void)
{
  gboolean ret = FALSE;
  GError *error = NULL;

  g_autoptr(GFile) repo_path = g_file_new_for_path ("repo");
  g_autoptr(OstreeRepo) repo =
    ostree_repo_create_at (AT_FDCWD, "repo", OSTREE_REPO_MODE_BARE_USER,
                           NULL, NULL, &error);
  g_assert (repo);
  g_assert_no_error (error);

  const char *nevra = "foo-1.0-1.x86_64";
  const char *name = "foo";
  guint64 epoch = 0;
  const char *version = "1.0";
  const char *release = "1";
  const char *arch = "x86_64";

  ret = rot_test_run_libtest ("build_rpm foo", &error);
  g_assert_no_error (error);
  g_assert (ret);

  g_autoptr(RpmOstreeUnpacker) unpacker = NULL;
  g_autofree char *foo_rpm = g_strdup_printf ("yumrepo/packages/%s/%s.rpm", arch, nevra);
  unpacker = rpmostree_unpacker_new_at (AT_FDCWD, foo_rpm, NULL, 0, &error);
  g_assert_no_error (error);
  g_assert (unpacker);

  ret = rpmostree_unpacker_unpack_to_ostree (unpacker, repo, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (ret);

  g_autoptr(GVariant) header = NULL;
  ret = rpmostree_pkgcache_find_pkg_header (repo, nevra, NULL, &header, NULL, &error);
  g_assert_no_error (error);
  g_assert (ret);

  g_autofree char *tname;
  guint64 tepoch;
  g_autofree char *tversion;
  g_autofree char *trelease;
  g_autofree char *tarch;

  ret = rpmostree_get_nevra_from_pkgcache (repo, nevra, &tname, &tepoch, &tversion,
                                           &trelease, &tarch, NULL, &error);
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

  g_test_add_func ("/utils/varsubst", test_varsubst_string);
  g_test_add_func ("/utils/cachebranch_to_nevra", test_cache_branch_to_nevra);
  g_test_add_func ("/unpacker/variant_to_nevra", test_variant_to_nevra);

  return g_test_run ();
}
