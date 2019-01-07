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

static void
test_auto_version (void)
{
  char *version = NULL;
  char *final_version = NULL;
  g_autoptr(GDateTime) date_time_utc = g_date_time_new_now_utc ();
  char *prev_version = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

#define _VER_TST(x, y, z)                              \
  g_clear_error (error);                               \
  version = _rpmostree_util_next_version ((x), (y), error);   \
  g_assert_cmpstr (version, ==, z);                           \
  g_free (version);                                           \
  version = NULL

  _VER_TST("10",  NULL,    "10");
  _VER_TST("10",  "",      "10");
  _VER_TST("10",  "xyz",   "10");
  _VER_TST("10",  "9",     "10");
  _VER_TST("10", "11",     "10");

  _VER_TST("10", "10",     "10.1");
  _VER_TST("10.1", "10.1",     "10.1.1");

  _VER_TST("10", "10.0",   "10.1");
  _VER_TST("10", "10.1",   "10.2");
  _VER_TST("10", "10.2",   "10.3");
  _VER_TST("10", "10.3",   "10.4");
  _VER_TST("10", "10.1.5", "10.2");
  _VER_TST("10.1", "10.1.5",   "10.1.6");
  _VER_TST("10.1", "10.1.1.5", "10.1.2");

  _VER_TST("10", "10001",  "10");
  _VER_TST("10", "101.1",  "10");
  _VER_TST("10", "10x.1",  "10");
  _VER_TST("10.1", "10",    "10.1");
  _VER_TST("10.1", "10.",   "10.1");
  _VER_TST("10.1", "10.0",  "10.1");
  _VER_TST("10.1", "10.2",  "10.1");
  _VER_TST("10.1", "10.12", "10.1");
  _VER_TST("10.1", "10.1x", "10.1");
  _VER_TST("10.1", "10.1.x", "10.1.1");
  _VER_TST("10.1", "10.1.2x", "10.1.3");

  /* Like _VER_TST, but renders datetime specifiers in
   * date_fmt with the current time. The rendered string
   * is then compared against the result of _rpmostree_util_next_version(). */
  #define _VER_DATE_TST(pre, last, final_datefmt)   \
    g_clear_error (error);                          \
    final_version = g_date_time_format (date_time_utc, (final_datefmt));   \
    version = _rpmostree_util_next_version ((pre), (last), error);   \
    g_assert_cmpstr (version, ==, final_version);                    \
    g_free (final_version);                                          \
    g_free (version);                                                \
    version = NULL;                                                  \
    final_version = NULL

  /* Test date updates. */
  _VER_DATE_TST("10.<date:%Y%m%d>", "10.20001010", "10.%Y%m%d.0");

  /* Test increment reset when date changed. */
  _VER_DATE_TST("10.<date:%Y%m%d>", "10.20001010.5", "10.%Y%m%d.0");

  /* Test increment up when same date. */
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d.1");
  _VER_DATE_TST("10.<date:%Y%m%d>", prev_version, "10.%Y%m%d.2");
  g_free (prev_version);

  /* Test append version number. */
  _VER_DATE_TST("10.<date:%Y%m%d>", NULL, "10.%Y%m%d.0");
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d");
  _VER_DATE_TST("10.<date:%Y%m%d>.0", prev_version, "10.%Y%m%d.0.0");
  g_free (prev_version);
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d.0");
  _VER_DATE_TST("10.<date:%Y%m%d>.0", prev_version, "10.%Y%m%d.0.0");
  g_free (prev_version);
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d.x");
  _VER_DATE_TST("10.<date:%Y%m%d>", prev_version, "10.%Y%m%d.1");
  g_free (prev_version);
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d.2.x");
  _VER_DATE_TST("10.<date:%Y%m%d>.2", prev_version, "10.%Y%m%d.2.1");
  g_free (prev_version);
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d.1.2x");
  _VER_DATE_TST("10.<date:%Y%m%d>.1", prev_version, "10.%Y%m%d.1.3");
  g_free (prev_version);

  /* Test variations to the formatting. */
  _VER_DATE_TST("10.<date: %Y%m%d>",          "10.20001010",    "10. %Y%m%d.0");
  _VER_DATE_TST("10.<date:%Y%m%d>.",          "10.20001010.",   "10.%Y%m%d..0");
  _VER_DATE_TST("10.<date:%Y%m%d>abc",        "10.20001010abc", "10.%Y%m%dabc.0");
  _VER_DATE_TST("10.<date:%Y%m%d >",          "10.20001010",    "10.%Y%m%d .0");
  _VER_DATE_TST("10.<date:text%Y%m%dhere>",   "10.20001010",    "10.text%Y%m%dhere.0");
  _VER_DATE_TST("10.<date:text %Y%m%d here>", "10.20001010",    "10.text %Y%m%d here.0");
  _VER_DATE_TST("10.<date:%Y%m%d here>",      "10.20001010",    "10.%Y%m%d here.0");

  /* Test equal last version and prefix. */
  prev_version = g_date_time_format (date_time_utc, "10.%Y%m%d");
  _VER_DATE_TST("10.<date:%Y%m%d>", prev_version, "10.%Y%m%d.0");
  g_free (prev_version);

  /* Test different prefix from last version. */
  _VER_DATE_TST("10.<date:%Y%m%d>", "10", "10.%Y%m%d.0");

  /* Test no field given. */
  _VER_TST("10.<date: >",     "10.20001010", "10. .0");
  _VER_TST("10.<date:>",      "10.20001010", "10..0");
  _VER_TST("10.<wrongtag: >", "10.20001010", "10.<wrongtag: >");

  /* Test invalid datetime specifier given. */
  _VER_TST("10.<date:%f>", "10.20001010", NULL);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/jsonparsing/get-optional-member", test_get_optional_string_member);
  g_test_add_func ("/versioning/automatic", test_auto_version);

  return g_test_run ();
}
