#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-postprocess.h"

typedef struct {
  const char *input;
  const char *output;
} AltfilesTest;

static AltfilesTest altfiles_tests[] = {
  {
    /* F25 */
  .input = "# An nsswitch.conf\n" \
  "\npasswd: files sss\n" \
  "\ngroup: files sss\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  .output = "# An nsswitch.conf\n" \
  "\npasswd: files altfiles sss\n" \
  "\ngroup: files altfiles sss\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n"
  },
  {
    /* F26 */
  .input = "# An nsswitch.conf\n" \
  "\npasswd: sss files systemd\n" \
  "\ngroup: sss files systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  .output = "# An nsswitch.conf\n" \
  "\npasswd: sss files altfiles systemd\n" \
  "\ngroup: sss files altfiles systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n"
  },
  {
    /* Already have altfiles, input/output identical */
  .input = "# An nsswitch.conf\n" \
  "\npasswd: sss files altfiles systemd\n" \
  "\ngroup: sss files altfiles systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  .output = "# An nsswitch.conf\n" \
  "\npasswd: sss files altfiles systemd\n" \
  "\ngroup: sss files altfiles systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  },
  {
    /* Test having `files` as a substring */
  .input = "# An nsswitch.conf\n" \
  "\npasswd: sss foofiles files systemd\n" \
  "\ngroup: sss foofiles files systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  .output = "# An nsswitch.conf\n" \
  "\npasswd: sss foofiles files altfiles systemd\n" \
  "\ngroup: sss foofiles files altfiles systemd\n" \
  "\nhosts:      files mdns4_minimal [NOTFOUND=return] dns myhostname\n",
  }
};

static void
test_postprocess_altfiles (void)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  for (guint i = 0; i < G_N_ELEMENTS(altfiles_tests); i++)
    {
      AltfilesTest *test = &altfiles_tests[i];
      g_autofree char *newbuf = rpmostree_postprocess_replace_nsswitch (test->input, error);
      g_assert_no_error (error);
      g_assert (newbuf);
      g_assert_cmpstr (newbuf, ==, test->output);
    }
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/altfiles", test_postprocess_altfiles);

  return g_test_run ();
}
