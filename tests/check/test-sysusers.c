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

static void
verify_sysuser_ent_content (GPtrArray       *sysusers_entries,
                            const gchar     **expected_content)
{
  /* Now we do the comparison between the converted output and the expected */
  for (int counter = 0; counter < sysusers_entries->len; counter++)
    {
      g_autofree gchar** sysent_list = g_strsplit (expected_content[counter], " ", -1);
      struct sysuser_ent *sysuser_ent = sysusers_entries->pdata[counter];
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
test_passwd_conversion(void)
{
  g_autoptr(GPtrArray) sysusers_entries = NULL;
  g_autoptr(GError) error = NULL;

  setup_sysuser_passwd (test_passwd, &sysusers_entries, &error);
  /* Check the pointer array entries after the set up */
  g_assert (sysusers_entries);

  verify_sysuser_ent_content (sysusers_entries, expected_sysuser_passwd_content);
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sysusers/passwd_conversion", test_passwd_conversion);
  return g_test_run ();
}
