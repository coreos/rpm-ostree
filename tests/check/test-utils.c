#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-util.h"

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

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/utils/varsubst", test_varsubst_string);

  return g_test_run ();
}
