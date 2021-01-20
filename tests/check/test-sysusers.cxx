#include "config.h"
#include "libglnx.h"
#include "rpmostree-util.h"
#include "rpmostree-json-parsing.h"
#include "rpmostree-passwd-util.h"

static const gchar *test_passwd[] = {
  "chrony:x:994:992::/var/lib/chrony:/sbin/nologin",
  "tcpdump:x:72:72::/:/sbin/nologin",
  "systemd-timesync:x:993:991:a:/:/sbin/nologin",
  "cockpit-ws:x:988:987:b:/:/sbin/nologin",
  NULL
};

static const gchar *expected_sysuser_passwd_content[] ={
  "u chrony 994:992 - /var/lib/chrony /sbin/nologin",
  "u tcpdump 72 - / /sbin/nologin",
  "u systemd-timesync 993:991 \"a\" / /sbin/nologin",
  "u cockpit-ws 988:987 \"b\" / /sbin/nologin",
  NULL
};

static const gchar *test_group[] = {
  "chrony:x:992:",
  "tcpdump:x:72:",
  "systemd-timesync:x:991:",
  "cockpit-ws:x:987:",
  "test:x:111:",
  NULL
};

static const gchar *expected_sysuser_group_content[] = {
  "g chrony 992 - - -",
  "g tcpdump 72 - - -",
  "g systemd-timesync 991 - - -",
  "g cockpit-ws 987 - - -",
  "g test 111 - - -",
  NULL
};

static const gchar *expected_sysuser_combined_content[] = {
  "g chrony 992 - - -",
  "g cockpit-ws 987 - - -",
  "g systemd-timesync 991 - - -",
  "g tcpdump 72 - - -",
  "g test 111 - - -",
  "u chrony 994:992 - /var/lib/chrony /sbin/nologin",
  "u cockpit-ws 988:987 \"b\" / /sbin/nologin",
  "u systemd-timesync 993:991 \"a\" / /sbin/nologin",
  "u tcpdump 72 - / /sbin/nologin",
  "", /* Adds an empty string at the end, so we get an \n for the string when using g_strjoinv */
  NULL
};

static void
verify_sysuser_ent_content (GPtrArray       *sysusers_entries,
                            const gchar     **expected_content)
{
  /* Now we do the comparison between the converted output and the expected */
  for (int counter = 0; counter < sysusers_entries->len; counter++)
    {
      g_autofree gchar** sysent_list = g_strsplit (expected_content[counter], " ", -1);
      auto sysuser_ent = (struct sysuser_ent *)sysusers_entries->pdata[counter];
      const char *shell = sysuser_ent->shell ?: "-";
      const char *gecos = sysuser_ent->gecos ?: "-";
      const char *dir = sysuser_ent->dir ?: "-";
      g_assert (g_str_equal (sysuser_ent->type, (char **)sysent_list[0]));
      g_assert (g_str_equal (sysuser_ent->name, (char **)sysent_list[1]));
      g_assert (g_str_equal (sysuser_ent->id, (char **)sysent_list[2]));
      g_assert (g_str_equal (shell, (char **)sysent_list[5]));
      g_assert (g_str_equal (gecos, (char **)sysent_list[3]));
      g_assert (g_str_equal (dir, (char **)sysent_list[4]));
    }
}

static void
setup_sysuser_passwd(const gchar  **test_content,
                     GPtrArray    **out_entries,
                     GError       **error)
{
  gboolean ret;
  g_autofree char* passwd_data = g_strjoinv ("\n", (char **)test_content);
  g_autoptr(GPtrArray) passwd_ents = rpmostree_passwd_data2passwdents (passwd_data);
  ret = rpmostree_passwdents2sysusers (passwd_ents, out_entries, error);
  g_assert(ret);
}

static void
setup_sysuser_group (const gchar  **test_content,
                     GPtrArray    **out_entries,
                     GError       **error)
{
  gboolean ret;
  g_autofree char* group_data = g_strjoinv ("\n", (char **)test_content);
  g_autoptr(GPtrArray) group_ents = rpmostree_passwd_data2groupents (group_data);
  ret = rpmostree_groupents2sysusers (group_ents, out_entries, error);
  g_assert (ret);
}

static void
test_passwd_conversion(void)
{
  g_autoptr(GPtrArray) sysusers_entries = NULL;
  g_autoptr(GError) error = NULL;

  setup_sysuser_passwd (test_passwd, &sysusers_entries, &error);
  /* Check the pointer array entries after the set up */
  g_assert (sysusers_entries);

  verify_sysuser_ent_content (sysusers_entries, expected_sysuser_passwd_content);
}

static void
test_group_conversion(void)
{
  g_autoptr(GPtrArray) sysusers_entries = NULL;
  g_autoptr(GError) error = NULL;

  setup_sysuser_group (test_group, &sysusers_entries, &error);
  /* Check the pointer array entries after the set up */
  g_assert (sysusers_entries);

  verify_sysuser_ent_content (sysusers_entries, expected_sysuser_group_content);
}

static void
check_sysuser_conversion_with_sorting (const gchar  **input_passwd,
                                       const gchar  **input_group,
                                       const gchar  **expected_content)
{
  g_autoptr(GPtrArray) sysusers_entries = NULL;
  g_autoptr(GError) error = NULL;
  gboolean ret;

  setup_sysuser_passwd (input_passwd, &sysusers_entries, &error);
  setup_sysuser_group (input_group, &sysusers_entries, &error);

  g_autofree gchar* output_content = NULL;
  ret = rpmostree_passwd_sysusers2char (sysusers_entries, &output_content, &error);

  g_assert (ret);
  g_autofree char* expected_combined_result = g_strjoinv ("\n", (char **)expected_content);
  g_assert (g_str_equal (expected_combined_result, output_content));
}

static void
test_sysuser_conversion_with_sorting(void)
{
  /* Checks for g > u */
  const char* sorting_case_one_group[] = {"chrony:x:992:", NULL};
  const char* sorting_case_one_passwd[] = {"chrony:x:994:992::/var/lib/chrony:/sbin/nologin", NULL};
  const char* sorting_case_one_output[] = {"g chrony 992 - - -", "u chrony 994:992 - /var/lib/chrony /sbin/nologin", "", NULL};

  check_sysuser_conversion_with_sorting (sorting_case_one_passwd,
                                         sorting_case_one_group,
                                         sorting_case_one_output);

  /* Checks for naming comparison */
  const char* sorting_case_two_group[] = {"tcpdump:x:72:", "chrony:x:992:", NULL};
  const char* sorting_case_two_passwd[] = {"chrony:x:994:992::/var/lib/chrony:/sbin/nologin", NULL};
  const char* sorting_case_two_output[] = {"g chrony 992 - - -", "g tcpdump 72 - - -", "u chrony 994:992 - /var/lib/chrony /sbin/nologin", "", NULL};

  check_sysuser_conversion_with_sorting (sorting_case_two_passwd,
                                         sorting_case_two_group,
                                         sorting_case_two_output);
  /* Check the combined output */
  check_sysuser_conversion_with_sorting (test_passwd,
                                         test_group,
                                         expected_sysuser_combined_content);
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sysusers/passwd_conversion", test_passwd_conversion);
  g_test_add_func ("/sysusers/group_conversion", test_group_conversion);
  g_test_add_func ("/sysusers/sysuser_conversion_with_sorting", test_sysuser_conversion_with_sorting);
  return g_test_run ();
}
