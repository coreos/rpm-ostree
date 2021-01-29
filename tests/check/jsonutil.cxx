#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include "libglnx.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-util.h"

static const gchar *test_data =
"{ \"text\" : \"hello, world!\", \"foo\" : null, \"blah\" : 47, \"double\" : 42.47 }";

static JsonObject *
get_test_data (void)
{
  GError *error = NULL;
  glnx_unref_object JsonParser *parser = json_parser_new ();
  (void)json_parser_load_from_data (parser, test_data, -1, &error);
  g_assert_no_error (error);
  return json_object_ref (json_node_get_object (json_parser_get_root (parser)));
}

static void
test_get_optional_string_member (void)
{
  GError *error = NULL;
  JsonObject *obj = get_test_data ();
  const char *str;

  (void) _rpmostree_jsonutil_object_get_optional_string_member (obj, "nomember", &str, &error);
  g_assert_no_error (error);
  g_assert (str == NULL);

  (void) _rpmostree_jsonutil_object_get_optional_string_member (obj, "text", &str, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (str, ==, "hello, world!");

  str = _rpmostree_jsonutil_object_require_string_member (obj, "nomember", &error);
  g_assert (error != NULL);
  g_clear_error (&error);
  g_assert (str == NULL);

  str = _rpmostree_jsonutil_object_require_string_member (obj, "text", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (str, ==, "hello, world!");

  json_object_unref (obj);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/jsonparsing/get-optional-member", test_get_optional_string_member);

  return g_test_run ();
}
