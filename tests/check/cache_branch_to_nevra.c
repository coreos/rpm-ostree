#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-util.h"

static void
test_one (const char *cache_branch,
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
  test_one ("rpmostree/pkg/foo/1.0-1.x86__64",                             "foo-1.0-1.x86_64");
  test_one ("rpmostree/pkg/git/1.8.3.1-6.el7__2.1.x86__64",                "git-1.8.3.1-6.el7_2.1.x86_64");
  test_one ("rpmostree/pkg/gpm-libs/1.20.7-5.el7.x86__64",                 "gpm-libs-1.20.7-5.el7.x86_64");
  test_one ("rpmostree/pkg/libgnome-keyring/3.8.0-3.el7.x86__64",          "libgnome-keyring-3.8.0-3.el7.x86_64");
  test_one ("rpmostree/pkg/perl/4_3A5.16.3-291.el7.x86__64",               "perl-4:5.16.3-291.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Carp/1.26-244.el7.noarch",                 "perl-Carp-1.26-244.el7.noarch");
  test_one ("rpmostree/pkg/perl-constant/1.27-2.el7.noarch",               "perl-constant-1.27-2.el7.noarch");
  test_one ("rpmostree/pkg/perl-Encode/2.51-7.el7.x86__64",                "perl-Encode-2.51-7.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Error/1_3A0.17020-2.el7.noarch",           "perl-Error-1:0.17020-2.el7.noarch");
  test_one ("rpmostree/pkg/perl-Exporter/5.68-3.el7.noarch",               "perl-Exporter-5.68-3.el7.noarch");
  test_one ("rpmostree/pkg/perl-File-Path/2.09-2.el7.noarch",              "perl-File-Path-2.09-2.el7.noarch");
  test_one ("rpmostree/pkg/perl-File-Temp/0.23.01-3.el7.noarch",           "perl-File-Temp-0.23.01-3.el7.noarch");
  test_one ("rpmostree/pkg/perl-Filter/1.49-3.el7.x86__64",                "perl-Filter-1.49-3.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Getopt-Long/2.40-2.el7.noarch",            "perl-Getopt-Long-2.40-2.el7.noarch");
  test_one ("rpmostree/pkg/perl-Git/1.8.3.1-6.el7__2.1.noarch",            "perl-Git-1.8.3.1-6.el7_2.1.noarch");
  test_one ("rpmostree/pkg/perl-HTTP-Tiny/0.033-3.el7.noarch",             "perl-HTTP-Tiny-0.033-3.el7.noarch");
  test_one ("rpmostree/pkg/perl-libs/4_3A5.16.3-291.el7.x86__64",          "perl-libs-4:5.16.3-291.el7.x86_64");
  test_one ("rpmostree/pkg/perl-macros/4_3A5.16.3-291.el7.x86__64",        "perl-macros-4:5.16.3-291.el7.x86_64");
  test_one ("rpmostree/pkg/perl-parent/1_3A0.225-244.el7.noarch",          "perl-parent-1:0.225-244.el7.noarch");
  test_one ("rpmostree/pkg/perl-PathTools/3.40-5.el7.x86__64",             "perl-PathTools-3.40-5.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Pod-Escapes/1_3A1.04-291.el7.noarch",      "perl-Pod-Escapes-1:1.04-291.el7.noarch");
  test_one ("rpmostree/pkg/perl-podlators/2.5.1-3.el7.noarch",             "perl-podlators-2.5.1-3.el7.noarch");
  test_one ("rpmostree/pkg/perl-Pod-Perldoc/3.20-4.el7.noarch",            "perl-Pod-Perldoc-3.20-4.el7.noarch");
  test_one ("rpmostree/pkg/perl-Pod-Simple/1_3A3.28-4.el7.noarch",         "perl-Pod-Simple-1:3.28-4.el7.noarch");
  test_one ("rpmostree/pkg/perl-Pod-Usage/1.63-3.el7.noarch",              "perl-Pod-Usage-1.63-3.el7.noarch");
  test_one ("rpmostree/pkg/perl-Scalar-List-Utils/1.27-248.el7.x86__64",   "perl-Scalar-List-Utils-1.27-248.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Socket/2.010-4.el7.x86__64",               "perl-Socket-2.010-4.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Storable/2.45-3.el7.x86__64",              "perl-Storable-2.45-3.el7.x86_64");
  test_one ("rpmostree/pkg/perl-TermReadKey/2.30-20.el7.x86__64",          "perl-TermReadKey-2.30-20.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Text-ParseWords/3.29-4.el7.noarch",        "perl-Text-ParseWords-3.29-4.el7.noarch");
  test_one ("rpmostree/pkg/perl-threads/1.87-4.el7.x86__64",               "perl-threads-1.87-4.el7.x86_64");
  test_one ("rpmostree/pkg/perl-threads-shared/1.43-6.el7.x86__64",        "perl-threads-shared-1.43-6.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Time-HiRes/4_3A1.9725-3.el7.x86__64",      "perl-Time-HiRes-4:1.9725-3.el7.x86_64");
  test_one ("rpmostree/pkg/perl-Time-Local/1.2300-2.el7.noarch",           "perl-Time-Local-1.2300-2.el7.noarch");
  test_one ("rpmostree/pkg/vim-common/2_3A7.4.160-1.el7__3.1.x86__64",     "vim-common-2:7.4.160-1.el7_3.1.x86_64");
  test_one ("rpmostree/pkg/vim-enhanced/2_3A7.4.160-1.el7__3.1.x86__64",   "vim-enhanced-2:7.4.160-1.el7_3.1.x86_64");
  test_one ("rpmostree/pkg/vim-filesystem/2_3A7.4.160-1.el7__3.1.x86__64", "vim-filesystem-2:7.4.160-1.el7_3.1.x86_64");
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cachebranch_to_nevra", test_cache_branch_to_nevra);

  return g_test_run ();
}
