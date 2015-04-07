/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antil <james@fedoraproject.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "rpmostree-rpm-util.h"

#include <fnmatch.h>
#include <sys/ioctl.h>
#include <libglnx.h>

#include <rpm/rpmdb.h>
#include <rpm/rpmts.h>

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

GS_DEFINE_CLEANUP_FUNCTION0(rpmtd, _cleanup_rpmtdFreeData, rpmtdFreeData);
#define _cleanup_rpmtddata_ __attribute__((cleanup(_cleanup_rpmtdFreeData)))

static int
header_name_cmp (Header h1, Header h2)
{
  const char*    n1 = headerGetString (h1, RPMTAG_NAME);
  const char*    n2 = headerGetString (h2, RPMTAG_NAME);
  int cmp = strcmp (n1, n2);
  /* printf("%s <=> %s = %u\n", n1, n2, cmp); */
  return cmp;
}

/* generate pkg nevra/etc. strings from Header objects */
static char *
pkg_envra_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  uint64_t      epoch = headerGetNumber (h1, RPMTAG_EPOCH);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);
  char *envra = NULL;

  if (!epoch)
    envra = g_strdup_printf ("%s-%s-%s.%s", name, version, release, arch);
  else
    {
      unsigned long long ullepoch = epoch;
      envra = g_strdup_printf ("%llu:%s-%s-%s.%s", ullepoch, name,
                               version, release, arch);
    }

  return envra;
}

static char *
pkg_nevra_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  uint64_t      epoch = headerGetNumber (h1, RPMTAG_EPOCH);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);
  char *nevra = NULL;

  if (!epoch)
    nevra = g_strdup_printf ("%s-%s-%s.%s", name, version, release, arch);
  else
    {
      unsigned long long ullepoch = epoch;
      nevra = g_strdup_printf ("%s-%llu:%s-%s.%s", name,
                               ullepoch, version, release, arch);
    }

  return nevra;
}

static char *
pkg_na_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);

  return g_strdup_printf ("%s.%s", name, arch);
}

static char *
pkg_nvr_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);

  return g_strdup_printf ("%s-%s-%s", name, version, release);
}

static char *
pkg_yumdb_relpath (Header h1)
{
   const char*    name = headerGetString (h1, RPMTAG_NAME);
   const char* version = headerGetString (h1, RPMTAG_VERSION);
   const char* release = headerGetString (h1, RPMTAG_RELEASE);
   const char*    arch = headerGetString (h1, RPMTAG_ARCH);
   const char*   pkgid = headerGetString (h1, RPMTAG_SHA1HEADER);
   char *path = NULL;

   g_assert (name[0]);

   // FIXME: sanitize name ... remove '/' and '~'

   // FIXME: If pkgid doesn't exist use: name.buildtime

   path = g_strdup_printf ("%c/%s-%s-%s-%s-%s", name[0],
                           pkgid, name, version, release, arch);

   return path;
}

static GInputStream *
pkg_yumdb_file_read (GFile *root, Header pkg, const char *yumdb_key,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gs_unref_object GFile *f = NULL;
  gs_free char *pkgpath = pkg_yumdb_relpath (pkg);
  gs_free char *path = g_strconcat ("/var/lib/yum/yumdb/", pkgpath, "/",
                                    yumdb_key, NULL);

  f = g_file_resolve_relative_path (root, path);
  return (GInputStream*)g_file_read (f, cancellable, error);
}

static char *
pkg_yumdb_strdup (GFile *root, Header pkg, const char *yumdb_key)
{
  gs_unref_object GFile *f = NULL;
  gs_free char *pkgpath = pkg_yumdb_relpath (pkg);
  gs_free char *path = g_strconcat ("/var/lib/yum/yumdb/", pkgpath, "/",
                                    yumdb_key, NULL);
  char *ret = NULL;

  f = g_file_resolve_relative_path (root, path);

  // allow_noent returns true for noent, false for other errors.
  if (!_rpmostree_file_load_contents_utf8_allow_noent (f, &ret, NULL, NULL) || !ret)
    ret = g_strdup ("");

  return ret;
}

static void
pkg_print (GFile *root, Header pkg)
{
  gs_free char *nevra = pkg_nevra_strdup (pkg);
  gs_free char *from_repo = pkg_yumdb_strdup (root, pkg, "from_repo");
  gsize align = glnx_console_lines ();

  if (*from_repo)
    {
      if (align)
        {
          gsize plen = strlen (nevra);
          gsize rlen = strlen (from_repo) + 1;
          int off = 0;

          --align; // hacky ... for leading spaces.

          off = align - (plen + rlen);

          if (align > (plen + rlen))
            printf ("%s%*s@%s\n", nevra, off, "", from_repo);
          else
            align = 0;
        }
      if (!align)
        printf ("%s @%s\n", nevra, from_repo);
    }
  else
    printf("%s\n", nevra);
}

#define CASENCMP_EQ(x, y, n) (g_ascii_strncasecmp (x, y, n) == 0)
#define CASEFNMATCH_EQ(x, y) (fnmatch (x, y, FNM_CASEFOLD) == 0)

/* find a common prefix length that doesn't need fnmatch */
static size_t
pat_fnmatch_prefix (const GPtrArray *patterns)
{
  gsize ret = G_MAXSIZE;
  int num = 0;

  if (!patterns)
    return 0;

  for (num = 0; num < patterns->len; num++)
    {
      const char *pat = patterns->pdata[num];
      gsize prefix = 0;

      while (*pat)
        {
          if (*pat == ':')
            break;
          if (*pat == '-')
            break;
          if (*pat == '*')
            break;
          if (*pat == '?')
            break;
          if (*pat == '.')
            break;
          if (*pat == '[')
            break;

          ++prefix;
          ++pat;
        }

      ret = MIN(ret, prefix);
    }

  return ret;
}

static gboolean
pat_fnmatch_match (Header pkg, const char *name,
                   gsize patprefixlen, const GPtrArray *patterns)
{
  int num = 0;
  gs_free char *pkg_na    = NULL;
  gs_free char *pkg_nevra = NULL;
  gs_free char *pkg_nvr   = NULL;

  if (!patterns)
    return TRUE;

  for (num = 0; num < patterns->len; num++)
    {
      const char *pattern = patterns->pdata[num];

      if (patprefixlen && !CASENCMP_EQ (name, pattern, patprefixlen))
        continue;

      if (!pkg_na)
        {
          pkg_nevra = pkg_nevra_strdup (pkg);
          pkg_na    = pkg_na_strdup (pkg);
          pkg_nvr   = pkg_nvr_strdup (pkg);
        }

      if (CASEFNMATCH_EQ (pattern, name) ||
          CASEFNMATCH_EQ (pattern, pkg_nevra) ||
          CASEFNMATCH_EQ (pattern, pkg_na) ||
          CASEFNMATCH_EQ (pattern, pkg_nvr) ||
          FALSE)
        return TRUE;
    }

  return FALSE;
}

static void
header_free_p (gpointer data)
{
  headerFree (data);
}

static int
header_cmp_p (gconstpointer gph1, gconstpointer gph2)
{
  const Header *ph1 = gph1;
  const Header *ph2 = gph2;
  Header h1 = *ph1;
  Header h2 = *ph2;
  int cmp = header_name_cmp (h1, h2);
  if (!cmp)
    cmp = rpmVersionCompare (h1, h2);
  return cmp;
}

static struct RpmHeaders *
rpmhdrs_new (const char *root, const GPtrArray *patterns)
{
  rpmts ts = rpmtsCreate();
  int status = -1;
  rpmdbMatchIterator iter;
  Header h1;
  GPtrArray *hs = NULL;
  struct RpmHeaders *ret = NULL;
  gsize patprefixlen = pat_fnmatch_prefix (patterns);

  // rpm also aborts on mem errors, so this is fine.
  g_assert (ts);
  rpmtsSetVSFlags (ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES);

  // This only fails if root isn't absolute.
  g_assert (root && root[0] == '/');
  status = rpmtsSetRootDir (ts, root);
  g_assert (status == 0);

  /* iter = rpmtsInitIterator (ts, RPMTAG_NAME, "yum", 0); */
  iter = rpmtsInitIterator (ts, RPMDBI_PACKAGES, NULL, 0);

  hs = g_ptr_array_new_with_free_func (header_free_p);
  while ((h1 = rpmdbNextIterator (iter)))
    {
      const char*    name = headerGetString (h1, RPMTAG_NAME);

      if (g_str_equal (name, "gpg-pubkey")) continue; /* rpmdb abstraction leak */

      if (!pat_fnmatch_match (h1, name, patprefixlen, patterns))
        continue;

      h1 = headerLink (h1);
      g_ptr_array_add (hs, h1);
    }
  iter = rpmdbFreeIterator (iter);

  g_ptr_array_sort (hs, header_cmp_p);

  ret = g_malloc0 (sizeof (struct RpmHeaders));

  ret->ts = ts;
  ret->hs = hs;

  return ret;
}

static void
rpmhdrs_free (struct RpmHeaders *l1)
{
  g_ptr_array_free (l1->hs, TRUE);
  l1->hs = NULL;
  l1->ts = rpmtsFree (l1->ts);

  g_free (l1);
}

static struct RpmHeadersDiff *
rpmhdrs_diff_new (void)
{
  struct RpmHeadersDiff *ret = g_malloc0(sizeof (struct RpmHeadersDiff));

  ret->hs_add = g_ptr_array_new ();
  ret->hs_del = g_ptr_array_new ();
  ret->hs_mod_old = g_ptr_array_new ();
  ret->hs_mod_new = g_ptr_array_new ();

 return ret;
}

static void
rpmhdrs_diff_free (struct RpmHeadersDiff *diff)
{
  g_ptr_array_free (diff->hs_add, TRUE);
  g_ptr_array_free (diff->hs_del, TRUE);
  g_ptr_array_free (diff->hs_mod_old, TRUE);
  g_ptr_array_free (diff->hs_mod_new, TRUE);

  g_free (diff);
}

struct RpmHeadersDiff *
rpmhdrs_diff (struct RpmHeaders *l1,
              struct RpmHeaders *l2)
{
  int n1 = 0;
  int n2 = 0;
  struct RpmHeadersDiff *ret = rpmhdrs_diff_new ();

  while (n1 < l1->hs->len)
    {
      Header h1 = l1->hs->pdata[n1];
      if (n2 >= l2->hs->len)
        {
          g_ptr_array_add (ret->hs_del, h1);
          ++n1;
        }
      else
        {
          Header h2 = l2->hs->pdata[n2];
          int cmp = header_name_cmp (h1, h2);

          if (cmp > 0)
            {
              g_ptr_array_add (ret->hs_add, h2);
              ++n2;
            }
          else if (cmp < 0)
            {
              g_ptr_array_add (ret->hs_del, h1);
              ++n1;
            }
          else
            {
              cmp = rpmVersionCompare (h1, h2);
              if (!cmp) { ++n1; ++n2; continue; }

              g_ptr_array_add (ret->hs_mod_old, h1);
              ++n1;
              g_ptr_array_add (ret->hs_mod_new, h2);
              ++n2;
            }
        }
    }

  while (n2 < l2->hs->len)
    {
      Header h2 = l2->hs->pdata[n2];

      g_ptr_array_add (ret->hs_add, h2);
      ++n2;
    }

  return ret;
}

void
rpmhdrs_list (GFile *root, struct RpmHeaders *l1)
{
  int num = 0;

  while (num < l1->hs->len)
    {
      Header h1 = l1->hs->pdata[num++];
      printf (" ");
      pkg_print (root, h1);
    }
}

char *
rpmhdrs_rpmdbv (GFile *root, struct RpmHeaders *l1,
                GCancellable   *cancellable,
                GError        **error)
{
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA1);
  gs_free char *checksum_cstr = NULL;
  char *ret = NULL;
  int num = 0;

  while (num < l1->hs->len)
    {
      Header pkg = l1->hs->pdata[num++];
      gs_unref_object GInputStream *tin = NULL;
      gs_unref_object GInputStream *din = NULL;
      char tbuf[1024];
      char dbuf[1024];
      gs_free char *envra = pkg_envra_strdup (pkg);
      gsize tbytes_read = 0;
      gsize dbytes_read = 0;
      GError *local_error = NULL;

      g_checksum_update (checksum, (guint8*)envra, strlen(envra));

      tin = pkg_yumdb_file_read (root, pkg, "checksum_type", cancellable, &local_error);

      /* Tolerate missing database files. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          continue;
        }

      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      din = pkg_yumdb_file_read (root, pkg, "checksum_data", cancellable,error);

      /* Tolerate missing database files. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          continue;
        }

      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      if (!g_input_stream_read_all (tin, tbuf, sizeof(tbuf), &tbytes_read,
                                    cancellable, error))
        goto out;
      if (!g_input_stream_read_all (din, dbuf, sizeof(dbuf), &dbytes_read,
                                    cancellable, error))
        goto out;

      if (tbytes_read >= 512)
        continue; // should be == len(md5) or len(sha256) etc.
      if (dbytes_read >= 1024)
        continue; // should be digest size of md5/sha256/sha512/etc.

      g_checksum_update (checksum, (guint8*)tbuf, tbytes_read);
      g_checksum_update (checksum, (guint8*)dbuf, dbytes_read);
    }

  checksum_cstr = g_strdup (g_checksum_get_string (checksum));

  ret = g_strdup_printf ("%u:%s", num, checksum_cstr);

out:
  g_checksum_free (checksum);

  return ret;
}

/* glib? */
static void
_gptr_array_reverse (GPtrArray *data)
{
  gpointer *ptr = NULL;
  guint num = 0;

  g_assert (data);

  ptr = data->pdata;
  num = data->len;
  while (num >= 2)
    {
      void *swap = ptr[0];
      ptr[0] = ptr[num-1];
      ptr[num-1] = swap;

      num -= 2;
      ptr++;
    }
}

static int
_rpmhdrs_diff_cmp_end (const GPtrArray *hs1, const GPtrArray *hs2)
{
  Header h1 = NULL;
  Header h2 = NULL;

  if (!hs2->len)
    return -1;
  if (!hs1->len)
    return  1;

  h1 = hs1->pdata[hs1->len - 1];
  h2 = hs2->pdata[hs2->len - 1];

  return header_name_cmp (h1, h2);
}

void
rpmhdrs_diff_prnt_block (GFile *root1, GFile *root2,
                         struct RpmHeadersDiff *diff)
{
  int num = 0;

  g_assert (diff->hs_mod_old->len == diff->hs_mod_new->len);

  if (diff->hs_mod_old->len)
    {
      gboolean done = FALSE;

      for (num = 0; num < diff->hs_mod_new->len; ++num)
        {
          Header ho = diff->hs_mod_old->pdata[num];
          Header hn = diff->hs_mod_new->pdata[num];
          struct rpmtd_s ochanges_date_s;
          _cleanup_rpmtddata_ rpmtd ochanges_date = NULL;
          struct rpmtd_s ochanges_name_s;
          _cleanup_rpmtddata_ rpmtd ochanges_name = NULL;
          struct rpmtd_s ochanges_text_s;
          _cleanup_rpmtddata_ rpmtd ochanges_text = NULL;
          struct rpmtd_s nchanges_date_s;
          _cleanup_rpmtddata_ rpmtd nchanges_date = NULL;
          struct rpmtd_s nchanges_name_s;
          _cleanup_rpmtddata_ rpmtd nchanges_name = NULL;
          struct rpmtd_s nchanges_text_s;
          _cleanup_rpmtddata_ rpmtd nchanges_text = NULL;
          int ocnum = 0;
          int ncnum = 0;
          uint64_t    ochange_date = 0;
          const char *ochange_name = NULL;
          const char *ochange_text = NULL;
          uint64_t    nchange_date = 0;
          const char *nchange_name = NULL;
          const char *nchange_text = NULL;

          g_assert (!header_name_cmp (ho, hn));
          if (rpmVersionCompare (ho, hn) > 0)
            continue;

          if (!done)
            {
              done = TRUE;
              g_print ("Upgraded:\n");
            }

          printf (" ");
          pkg_print (root2, hn);

          // Load the old %changelog entries
          ochanges_date = &ochanges_date_s;
          headerGet (ho, RPMTAG_CHANGELOGTIME, ochanges_date, HEADERGET_MINMEM);
          ochanges_name = &ochanges_name_s;
          headerGet (ho, RPMTAG_CHANGELOGNAME, ochanges_name, HEADERGET_MINMEM);
          ochanges_text = &ochanges_text_s;
          headerGet (ho, RPMTAG_CHANGELOGTEXT, ochanges_text, HEADERGET_MINMEM);

          ocnum = rpmtdCount (ochanges_date);
          if (!ocnum)
            continue;

          // Load the new %changelog entries
          nchanges_date = &nchanges_date_s;
          headerGet (hn, RPMTAG_CHANGELOGTIME, nchanges_date, HEADERGET_MINMEM);
          nchanges_name = &nchanges_name_s;
          headerGet (hn, RPMTAG_CHANGELOGNAME, nchanges_name, HEADERGET_MINMEM);
          nchanges_text = &nchanges_text_s;
          headerGet (hn, RPMTAG_CHANGELOGTEXT, nchanges_text, HEADERGET_MINMEM);

          ncnum = rpmtdCount (nchanges_date);
          if (!ncnum)
            continue;

          // Load the latest old %changelog entry.
          ochange_date = rpmtdGetNumber (ochanges_date);
          ochange_name = rpmtdGetString (ochanges_name);
          ochange_text = rpmtdGetString (ochanges_text);

          while (ncnum > 0)
            {
              GDateTime *dt = NULL;
              gs_free char *date_time_str = NULL;

              // Load next new %changelog entry, starting at the newest.
              rpmtdNext (nchanges_date);
              rpmtdNext (nchanges_name);
              rpmtdNext (nchanges_text);
              nchange_date = rpmtdGetNumber (nchanges_date);
              nchange_name = rpmtdGetString (nchanges_name);
              nchange_text = rpmtdGetString (nchanges_text);

              //  If we are now older than, or match, the latest old %changelog
              // then we are done.
              if (ochange_date > nchange_date)
                break;
              if ((ochange_date == nchange_date) &&
                  g_str_equal (ochange_name, nchange_name) &&
                  g_str_equal (ochange_text, nchange_text))
                break;

              // Otherwise, print.
              dt = g_date_time_new_from_unix_utc (nchange_date);
              date_time_str = g_date_time_format (dt, "%a %b %d %Y");
              g_date_time_unref (dt);

              printf ("* %s %s\n%s\n\n", date_time_str, nchange_name,
                      nchange_text);

              --ncnum;
            }
        }

      done = FALSE;
      for (num = 0; num < diff->hs_mod_new->len; ++num)
        {
          Header ho = diff->hs_mod_old->pdata[num];
          Header hn = diff->hs_mod_new->pdata[num];

          g_assert (!header_name_cmp (ho, hn));
          if (rpmVersionCompare (ho, hn) < 0)
            continue;

          if (!done)
            {
              done = TRUE;
              g_print ("Downgraded:\n");
            }

          printf (" ");
          pkg_print (root2, hn);
        }
    }

  if (diff->hs_del->len)
    {
      g_print ("Removed:\n");

      for (num = 0; num < diff->hs_del->len; ++num)
        {
          Header hd = diff->hs_del->pdata[num];

          printf (" ");
          pkg_print (root1, hd);
        }
    }

  if (diff->hs_add->len)
    {
      g_print ("Added:\n");

      for (num = 0; num < diff->hs_add->len; ++num)
        {
          Header ha = diff->hs_add->pdata[num];

          printf (" ");
          pkg_print (root2, ha);
        }
    }

  rpmhdrs_diff_free (diff);
}

void
rpmhdrs_diff_prnt_diff (GFile *root1, GFile *root2, struct RpmHeadersDiff *diff)
{
  _gptr_array_reverse (diff->hs_add);
  _gptr_array_reverse (diff->hs_del);
  _gptr_array_reverse (diff->hs_mod_old);
  _gptr_array_reverse (diff->hs_mod_new);

  g_assert (diff->hs_mod_old->len == diff->hs_mod_new->len);

  while (diff->hs_add->len ||
         diff->hs_del->len ||
         diff->hs_mod_old->len)
    {
      if (_rpmhdrs_diff_cmp_end (diff->hs_mod_old, diff->hs_del) < 0)
        if (_rpmhdrs_diff_cmp_end (diff->hs_mod_old, diff->hs_add) < 0)
          { // mod is first
            Header hm = diff->hs_mod_old->pdata[diff->hs_mod_old->len-1];

            printf("!");
            pkg_print (root1, hm);
            g_ptr_array_remove_index(diff->hs_mod_old, diff->hs_mod_old->len-1);
            printf("=");
            hm = diff->hs_mod_new->pdata[diff->hs_mod_new->len-1];
            pkg_print (root2, hm);
            g_ptr_array_remove_index(diff->hs_mod_new, diff->hs_mod_new->len-1);
          }
        else
          { // add is first
            Header ha = diff->hs_add->pdata[diff->hs_add->len-1];

            printf ("+");
            pkg_print (root2, ha);
            g_ptr_array_remove_index(diff->hs_add, diff->hs_add->len-1);
          }
      else
        if (_rpmhdrs_diff_cmp_end (diff->hs_del, diff->hs_add) < 0)
          { // del is first
            Header hd = diff->hs_del->pdata[diff->hs_del->len-1];

            printf ("-");
            pkg_print (root1, hd);
            g_ptr_array_remove_index(diff->hs_del, diff->hs_del->len-1);
          }
        else
          { // add is first
            Header ha = diff->hs_add->pdata[diff->hs_add->len-1];

            printf ("+");
            pkg_print (root2, ha);
            g_ptr_array_remove_index(diff->hs_add, diff->hs_add->len-1);
          }
    }

  rpmhdrs_diff_free (diff);
}

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo, GFile *rpmdbdir, const char *rev,
            const GPtrArray *patterns,
            GCancellable   *cancellable,
            GError        **error)
{
  gs_unref_object GFile *subtree = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *targetp = NULL;
  gs_unref_object GFile *target = NULL;
  gs_free char *targetp_path = NULL;
  gs_free char *target_path = NULL;
  gs_unref_object GFile *revdir = NULL;
  GError *tmp_error = NULL;
  struct RpmRevisionData *rpmrev = NULL;
  gs_unref_object GFile *root = NULL;
  gs_free char *commit = NULL;

  if (!ostree_repo_read_commit (repo, rev, &root, &commit, NULL, error))
    goto out;

  subtree = g_file_resolve_relative_path (root, "/var/lib/rpm");
  if (!g_file_query_exists (subtree, cancellable))
    {
      g_object_unref (subtree);
      subtree = g_file_resolve_relative_path (root, "/usr/share/rpm");
    }

  file_info = g_file_query_info (subtree, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, &tmp_error);
  if (!file_info) // g_file_query_exists (subtree, cancellable))
  {
    g_propagate_error (error, tmp_error);
    //    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
    //                 "No rpmdb directory found");
    goto out;
  }

  revdir = g_file_resolve_relative_path (rpmdbdir, commit);

  targetp_path = g_strconcat (commit, "/var/lib", NULL);
  targetp = g_file_resolve_relative_path (rpmdbdir, targetp_path);
  target_path = g_strconcat (commit, "/var/lib/rpm", NULL);
  target = g_file_resolve_relative_path (rpmdbdir, target_path);
  if (!g_file_query_exists (target, cancellable) &&
      (!gs_file_ensure_directory (targetp, TRUE, cancellable, error) ||
       !ostree_repo_checkout_tree (repo, OSTREE_REPO_CHECKOUT_MODE_USER,
                                   OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                   target, OSTREE_REPO_FILE (subtree),
                                   file_info, cancellable, error)))
    goto out;

  rpmrev = g_malloc0 (sizeof(struct RpmRevisionData));

  rpmrev->root   = root;   root = NULL;
  rpmrev->commit = commit; commit = NULL;
  rpmrev->rpmdb  = rpmhdrs_new (gs_file_get_path_cached (revdir), patterns);

 out:
  return rpmrev;
}

void
rpmrev_free (struct RpmRevisionData *ptr)
{
  gs_unref_object GFile *root = NULL;
  gs_free char *commit = NULL;

  if (!ptr)
    return;

  rpmhdrs_free (ptr->rpmdb);
  ptr->rpmdb = NULL;

  root = ptr->root;
  ptr->root = NULL;

  commit = ptr->commit;
  ptr->commit = NULL;

  g_free (ptr);
}

