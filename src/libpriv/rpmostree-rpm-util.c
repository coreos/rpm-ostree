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

#include <inttypes.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <sys/capability.h>
#include <libglnx.h>

#include <rpm/rpmts.h>

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static inline void
cleanup_rpmtdFreeData (rpmtd *tdp)
{
  rpmtd td = *tdp;
  if (td)
    rpmtdFreeData (td);
}
#define _cleanup_rpmtddata_ __attribute__((cleanup(cleanup_rpmtdFreeData)))

struct RpmRevisionData
{
  struct RpmHeaders *rpmdb;
  RpmOstreeRefTs *refts;
  char *commit;
};

static int
header_name_cmp (Header h1, Header h2)
{
  const char*    n1 = headerGetString (h1, RPMTAG_NAME);
  const char*    n2 = headerGetString (h2, RPMTAG_NAME);
  int cmp = strcmp (n1, n2);
  /* printf("%s <=> %s = %u\n", n1, n2, cmp); */
  return cmp;
}

/* keep this one to be backwards compatible with previously
 * generated checksums */
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

char *
rpmostree_pkg_custom_nevra_strdup (Header h, RpmOstreePkgNevraFlags flags)
{
  const char*    name = headerGetString (h, RPMTAG_NAME);
  uint64_t      epoch = headerGetNumber (h, RPMTAG_EPOCH);
  const char* version = headerGetString (h, RPMTAG_VERSION);
  const char* release = headerGetString (h, RPMTAG_RELEASE);
  const char*    arch = headerGetString (h, RPMTAG_ARCH);
  GString *nevra = g_string_new ("");

  if (flags & PKG_NEVRA_FLAGS_NAME)
    g_string_append (nevra, name);

  if (flags & (PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE |
               PKG_NEVRA_FLAGS_VERSION_RELEASE))
    {
      if (nevra->len)
        g_string_append_c (nevra, '-');

      if ((flags & PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE) && (epoch > 0))
        g_string_append_printf (nevra, "%" PRIu64 ":", epoch);

      g_string_append_printf (nevra, "%s-%s", version, release);
    }

  if (flags & PKG_NEVRA_FLAGS_ARCH)
    {
      if (nevra->len)
        g_string_append_c (nevra, '.');
      g_string_append (nevra, arch);
    }

  return g_string_free (nevra, FALSE);
}

static char *
pkg_nevra_strdup (Header h1)
{
  return rpmostree_pkg_custom_nevra_strdup (h1, PKG_NEVRA_FLAGS_NAME |
                                                PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE |
                                                PKG_NEVRA_FLAGS_ARCH);
}

static char *
pkg_na_strdup (Header h1)
{
  return rpmostree_pkg_custom_nevra_strdup (h1, PKG_NEVRA_FLAGS_NAME |
                                                PKG_NEVRA_FLAGS_ARCH);
}

static char *
pkg_nvr_strdup (Header h1)
{
  return rpmostree_pkg_custom_nevra_strdup (h1, PKG_NEVRA_FLAGS_NAME |
                                                PKG_NEVRA_FLAGS_VERSION_RELEASE);
}

static char *
pkg_evra_strdup (Header h1)
{
  return rpmostree_pkg_custom_nevra_strdup (h1, PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE |
                                                PKG_NEVRA_FLAGS_ARCH);
}

static void
pkg_print (Header pkg)
{
  g_autofree char *nevra = pkg_nevra_strdup (pkg);
  g_print ("%s\n", nevra);
}

static void
pkg_print_changed (Header opkg, Header npkg)
{
  const char *name = headerGetString (opkg, RPMTAG_NAME);
  g_autofree char *old_evra = pkg_evra_strdup (opkg);
  g_autofree char *new_evra = pkg_evra_strdup (npkg);
  g_print ("%s %s -> %s\n", name, old_evra, new_evra);
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
  g_autofree char *pkg_na    = NULL;
  g_autofree char *pkg_nevra = NULL;
  g_autofree char *pkg_nvr   = NULL;

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
rpmhdrs_new (RpmOstreeRefTs *refts, const GPtrArray *patterns)
{
  rpmdbMatchIterator iter;
  Header h1;
  GPtrArray *hs = NULL;
  struct RpmHeaders *ret = NULL;
  gsize patprefixlen = pat_fnmatch_prefix (patterns);

  /* iter = rpmtsInitIterator (ts, RPMTAG_NAME, "yum", 0); */
  iter = rpmtsInitIterator (refts->ts, RPMDBI_PACKAGES, NULL, 0);

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

  ret->refts = rpmostree_refts_ref (refts);
  ret->hs = hs;

  return ret;
}

static void
rpmhdrs_free (struct RpmHeaders *l1)
{
  g_ptr_array_free (l1->hs, TRUE);
  l1->hs = NULL;
  rpmostree_refts_unref (l1->refts);

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
rpmhdrs_list (struct RpmHeaders *l1)
{
  int num = 0;

  while (num < l1->hs->len)
    {
      Header h1 = l1->hs->pdata[num++];
      printf (" ");
      pkg_print (h1);
    }
}

char *
rpmhdrs_rpmdbv (struct RpmHeaders *l1,
                GCancellable   *cancellable,
                GError        **error)
{
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA1);
  g_autofree char *checksum_cstr = NULL;
  char *ret = NULL;
  int num = 0;

  while (num < l1->hs->len)
    {
      Header pkg = l1->hs->pdata[num++];
      g_autofree char *envra = pkg_envra_strdup (pkg);

      g_checksum_update (checksum, (guint8*)envra, strlen(envra));
    }

  checksum_cstr = g_strdup (g_checksum_get_string (checksum));

  ret = g_strdup_printf ("%u:%s", num, checksum_cstr);

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
rpmhdrs_diff_prnt_block (gboolean changelogs, struct RpmHeadersDiff *diff)
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

          g_assert (!header_name_cmp (ho, hn));
          if (rpmVersionCompare (ho, hn) > 0)
            continue;

          if (!done)
            {
              done = TRUE;
              g_print ("Upgraded:\n");
            }

          g_print ("  ");
          pkg_print_changed (ho, hn);

          if (!changelogs)
            continue;

          /* Load the old %changelog entries */
          ochanges_date = &ochanges_date_s;
          headerGet (ho, RPMTAG_CHANGELOGTIME, ochanges_date, HEADERGET_MINMEM);
          ochanges_name = &ochanges_name_s;
          headerGet (ho, RPMTAG_CHANGELOGNAME, ochanges_name, HEADERGET_MINMEM);
          ochanges_text = &ochanges_text_s;
          headerGet (ho, RPMTAG_CHANGELOGTEXT, ochanges_text, HEADERGET_MINMEM);

          ocnum = rpmtdCount (ochanges_date);
          if (!ocnum)
            continue;

          /* Load the new %changelog entries */
          nchanges_date = &nchanges_date_s;
          headerGet (hn, RPMTAG_CHANGELOGTIME, nchanges_date, HEADERGET_MINMEM);
          nchanges_name = &nchanges_name_s;
          headerGet (hn, RPMTAG_CHANGELOGNAME, nchanges_name, HEADERGET_MINMEM);
          nchanges_text = &nchanges_text_s;
          headerGet (hn, RPMTAG_CHANGELOGTEXT, nchanges_text, HEADERGET_MINMEM);

          ncnum = rpmtdCount (nchanges_date);
          if (!ncnum)
            continue;

          /* Load the latest old %changelog entry. */
          ochange_date = rpmtdGetNumber (ochanges_date);
          ochange_name = rpmtdGetString (ochanges_name);
          ochange_text = rpmtdGetString (ochanges_text);

          while (ncnum > 0)
            {
              uint64_t    nchange_date = 0;
              const char *nchange_name = NULL;
              const char *nchange_text = NULL;
              GDateTime *dt = NULL;
              g_autofree char *date_time_str = NULL;

              /* Load next new %changelog entry, starting at the newest. */
              rpmtdNext (nchanges_date);
              rpmtdNext (nchanges_name);
              rpmtdNext (nchanges_text);
              nchange_date = rpmtdGetNumber (nchanges_date);
              nchange_name = rpmtdGetString (nchanges_name);
              nchange_text = rpmtdGetString (nchanges_text);

              /*  If we are now older than, or match, the latest old %changelog
               * then we are done. */
              if (ochange_date > nchange_date)
                break;
              if ((ochange_date == nchange_date) &&
                  g_str_equal (ochange_name, nchange_name) &&
                  g_str_equal (ochange_text, nchange_text))
                break;

              /* Otherwise, print. */
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

          printf ("  ");
          pkg_print_changed (ho, hn);
        }
    }

  if (diff->hs_del->len)
    {
      g_print ("Removed:\n");

      for (num = 0; num < diff->hs_del->len; ++num)
        {
          Header hd = diff->hs_del->pdata[num];

          printf ("  ");
          pkg_print (hd);
        }
    }

  if (diff->hs_add->len)
    {
      g_print ("Added:\n");

      for (num = 0; num < diff->hs_add->len; ++num)
        {
          Header ha = diff->hs_add->pdata[num];

          printf ("  ");
          pkg_print (ha);
        }
    }

  rpmhdrs_diff_free (diff);
}

void
rpmhdrs_diff_prnt_diff (struct RpmHeadersDiff *diff)
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
          { /* mod is first */
            Header hm = diff->hs_mod_old->pdata[diff->hs_mod_old->len-1];

            printf("!");
            pkg_print (hm);
            g_ptr_array_remove_index(diff->hs_mod_old, diff->hs_mod_old->len-1);
            printf("=");
            hm = diff->hs_mod_new->pdata[diff->hs_mod_new->len-1];
            pkg_print (hm);
            g_ptr_array_remove_index(diff->hs_mod_new, diff->hs_mod_new->len-1);
          }
        else
          { /* add is first */
            Header ha = diff->hs_add->pdata[diff->hs_add->len-1];

            printf ("+");
            pkg_print (ha);
            g_ptr_array_remove_index(diff->hs_add, diff->hs_add->len-1);
          }
      else
        if (_rpmhdrs_diff_cmp_end (diff->hs_del, diff->hs_add) < 0)
          { /* del is first */
            Header hd = diff->hs_del->pdata[diff->hs_del->len-1];

            printf ("-");
            pkg_print (hd);
            g_ptr_array_remove_index(diff->hs_del, diff->hs_del->len-1);
          }
        else
          { /* add is first */
            Header ha = diff->hs_add->pdata[diff->hs_add->len-1];

            printf ("+");
            pkg_print (ha);
            g_ptr_array_remove_index(diff->hs_add, diff->hs_add->len-1);
          }
    }

  rpmhdrs_diff_free (diff);
}

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo, const char *rev,
            const GPtrArray *patterns,
            GCancellable   *cancellable,
            GError        **error)
{
  g_autofree char *commit = NULL;
  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &commit, error))
    return NULL;

  g_autoptr(RpmOstreeRefTs) refts = NULL;
  if (!rpmostree_get_refts_for_commit (repo, commit, &refts, cancellable, error))
    return NULL;

  RpmRevisionData *rpmrev = g_malloc0 (sizeof(struct RpmRevisionData));
  rpmrev->refts = g_steal_pointer (&refts);
  rpmrev->commit = g_steal_pointer (&commit);
  rpmrev->rpmdb = rpmhdrs_new (rpmrev->refts, patterns);
  return rpmrev;
}

struct RpmHeaders *
rpmrev_get_headers (struct RpmRevisionData *self)
{
  return self->rpmdb;
}

const char *
rpmrev_get_commit (struct RpmRevisionData *self)
{
  return self->commit;
}

void
rpmrev_free (struct RpmRevisionData *ptr)
{
  if (!ptr)
    return;

  rpmhdrs_free (ptr->rpmdb);
  ptr->rpmdb = NULL;

  rpmostree_refts_unref (ptr->refts);

  g_clear_pointer (&ptr->commit, g_free);

  g_free (ptr);
}

gboolean
rpmostree_checkout_only_rpmdb_tempdir (OstreeRepo       *repo,
                                       const char       *ref,
                                       const char       *template,
                                       char            **out_tempdir,
                                       int              *out_tempdir_dfd,
                                       GCancellable     *cancellable,
                                       GError          **error)
{
  gboolean ret = FALSE;
  g_autofree char *commit = NULL;
  g_autofree char *tempdir = NULL;
  OstreeRepoCheckoutAtOptions checkout_options = { 0, };
  glnx_fd_close int tempdir_dfd = -1;

  g_return_val_if_fail (out_tempdir != NULL, FALSE);

  if (!rpmostree_mkdtemp (template, &tempdir, &tempdir_dfd, error))
    goto out;

  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &commit, error))
    goto out;

  /* Create intermediate dirs */ 
  if (!glnx_shutil_mkdir_p_at (tempdir_dfd, "usr/share", 0777, cancellable, error))
    goto out;

  checkout_options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  checkout_options.subpath = "usr/share/rpm";

  if (!ostree_repo_checkout_at (repo, &checkout_options,
                                tempdir_dfd, "usr/share/rpm",
                                commit, 
                                cancellable, error))
    goto out;

  /* And make a compat symlink to keep rpm happy */ 
  if (!glnx_shutil_mkdir_p_at (tempdir_dfd, "var/lib", 0777, cancellable, error))
    goto out;

  if (symlinkat ("../../usr/share/rpm", tempdir_dfd, "var/lib/rpm") == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  *out_tempdir = g_steal_pointer (&tempdir);
  if (out_tempdir_dfd)
    {
      *out_tempdir_dfd = tempdir_dfd;
      tempdir_dfd = -1;
    }
  ret = TRUE;
 out:
  if (tempdir)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tempdir, NULL, NULL);
  return ret;
}

static gboolean
get_sack_for_root (int               dfd,
                   const char       *path,
                   DnfSack         **out_sack,
                   GCancellable     *cancellable,
                   GError          **error)
{
  g_return_val_if_fail (out_sack != NULL, FALSE);

  g_autofree char *fullpath = glnx_fdrel_abspath (dfd, path);

  g_autoptr(DnfSack) sack = dnf_sack_new ();
  dnf_sack_set_rootdir (sack, fullpath);

  if (!dnf_sack_setup (sack, DNF_SACK_LOAD_FLAG_BUILD_CACHE, error))
    return FALSE;

  if (!dnf_sack_load_system_repo (sack, NULL, 0, error))
    return FALSE;

  *out_sack = g_steal_pointer (&sack);
  return TRUE;
}

RpmOstreeRefSack *
rpmostree_get_refsack_for_root (int              dfd,
                                const char      *path,
                                GCancellable    *cancellable,
                                GError         **error)
{
  g_autoptr(DnfSack) sack; /* NB: refsack adds a ref to it */
  if (!get_sack_for_root (dfd, path, &sack, cancellable, error))
    return NULL;

  return rpmostree_refsack_new (sack, AT_FDCWD, NULL);
}


RpmOstreeRefSack *
rpmostree_get_refsack_for_commit (OstreeRepo                *repo,
                                  const char                *ref,
                                  GCancellable              *cancellable,
                                  GError                   **error)
{
  RpmOstreeRefSack *ret = NULL;
  g_autofree char *tempdir = NULL;
  glnx_fd_close int tempdir_dfd = -1;
  g_autoptr(DnfSack) hsack = NULL; /* NB: refsack adds a ref to it */

  if (!rpmostree_checkout_only_rpmdb_tempdir (repo, ref,
                                              "/tmp/rpmostree-dbquery-XXXXXX",
                                              &tempdir, &tempdir_dfd,
                                              cancellable, error))
    goto out;

  if (!get_sack_for_root (tempdir_dfd, ".",
                          &hsack, cancellable, error))
    goto out;

  ret = rpmostree_refsack_new (hsack, AT_FDCWD, tempdir);
  g_clear_pointer (&tempdir, g_free);
 out:
  if (tempdir)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tempdir, NULL, NULL);
  return ret;
}

gboolean
rpmostree_get_refts_for_commit (OstreeRepo                *repo,
                                const char                *ref,
                                RpmOstreeRefTs           **out_ts,
                                GCancellable              *cancellable,
                                GError                   **error)
{
  gboolean ret = FALSE;
  g_autofree char *tempdir = NULL;
  rpmts ts;
  int r;
  
  if (!rpmostree_checkout_only_rpmdb_tempdir (repo, ref,
                                              "/tmp/rpmostree-dbquery-XXXXXX",
                                              &tempdir, NULL,
                                              cancellable, error))
    goto out;

  ts = rpmtsCreate ();
  /* This actually makes sense because we know we've verified it at build time */
  rpmtsSetVSFlags (ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES);

  r = rpmtsSetRootDir (ts, tempdir);
  g_assert_cmpint (r, ==, 0);
  
  ret = TRUE;
  *out_ts = rpmostree_refts_new (ts, AT_FDCWD, tempdir);
  g_clear_pointer (&tempdir, g_free);
 out:
  if (tempdir)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tempdir, NULL, NULL);
  return ret;
}

gboolean
rpmostree_get_pkglist_for_root (int               dfd,
                                const char       *path,
                                RpmOstreeRefSack **out_refsack,
                                GPtrArray        **out_pkglist,
                                GCancellable     *cancellable,
                                GError          **error)
{
  g_autoptr(RpmOstreeRefSack) refsack =
    rpmostree_get_refsack_for_root (dfd, path, cancellable, error);
  if (!refsack)
    return FALSE;

  hy_autoquery HyQuery query = hy_query_create (refsack->sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  g_autoptr(GPtrArray) pkglist = hy_query_run (query);

  if (out_refsack)
    *out_refsack = g_steal_pointer (&refsack);
  if (out_pkglist)
    *out_pkglist = g_steal_pointer (&pkglist);
  return TRUE;
}

static gint
pkg_array_compare (DnfPackage **p_pkg1,
                   DnfPackage **p_pkg2)
{
  return dnf_package_cmp (*p_pkg1, *p_pkg2);
}

void
rpmostree_sighandler_reset_cleanup (RpmSighandlerResetCleanup *cleanup)
{
  /* Forcibly override rpm/librepo SIGINT handlers.  We always operate
   * in a fully idempotent/atomic mode, and can be killed at any time.
   */
#if !BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
#endif
}

static void
print_pkglist (GPtrArray *pkglist)
{
  g_ptr_array_sort (pkglist, (GCompareFunc) pkg_array_compare);

  for (guint i = 0; i < pkglist->len; i++)
    {
      DnfPackage *pkg = pkglist->pdata[i];
      g_print ("  %s (%s)\n", dnf_package_get_nevra (pkg),
                              dnf_package_get_reponame (pkg));
    }
}

void
rpmostree_print_transaction (DnfContext   *hifctx)
{
  gboolean empty = TRUE;

  { g_autoptr(GPtrArray) packages = NULL;
    packages = dnf_goal_get_packages (dnf_context_get_goal (hifctx),
                                      DNF_PACKAGE_INFO_INSTALL,
                                      DNF_PACKAGE_INFO_REINSTALL,
                                      DNF_PACKAGE_INFO_DOWNGRADE,
                                      DNF_PACKAGE_INFO_UPDATE,
                                      -1);

    if (packages->len > 0)
      empty = FALSE;

    g_print ("Installing %u packages:\n", packages->len);
    print_pkglist (packages);
  }

  { g_autoptr(GPtrArray) packages = NULL;
    packages = dnf_goal_get_packages (dnf_context_get_goal (hifctx),
                                    DNF_PACKAGE_INFO_REMOVE,
                                    DNF_PACKAGE_INFO_OBSOLETE,
                                      -1);

    if (packages->len > 0)
      empty = FALSE;

    g_print ("Removing %u packages:\n", packages->len);
    print_pkglist (packages);
  }

  if (empty)
    g_print ("Empty transaction\n");
}

struct _cap_struct {
    struct __user_cap_header_struct head;
    union {
        struct __user_cap_data_struct set;
        __u32 flat[3];
    } u[_LINUX_CAPABILITY_U32S_2];
};

/* Rewritten version of _fcaps_save from libcap, since it's not
 * exposed, and we need to generate the raw value.
 */
static void
cap_t_to_vfs (cap_t cap_d, struct vfs_cap_data *rawvfscap, int *out_size)
{
  guint32 eff_not_zero, magic;
  guint tocopy, i;

  /* Hardcoded to 2.  There is apparently a version 3 but it just maps
   * to 2.  I doubt another version would ever be implemented, and
   * even if it was we'd need to be backcompatible forever.  Anyways,
   * setuid/fcaps binaries should go away entirely.
   */
  magic = VFS_CAP_REVISION_2;
  tocopy = VFS_CAP_U32_2;
  *out_size = XATTR_CAPS_SZ_2;

  for (eff_not_zero = 0, i = 0; i < tocopy; i++)
    eff_not_zero |= cap_d->u[i].flat[CAP_EFFECTIVE];

  /* Here we're also not validating that the kernel understands
   * the capabilities.
   */

  for (i = 0; i < tocopy; i++)
    {
      rawvfscap->data[i].permitted
        = GUINT32_TO_LE(cap_d->u[i].flat[CAP_PERMITTED]);
      rawvfscap->data[i].inheritable
        = GUINT32_TO_LE(cap_d->u[i].flat[CAP_INHERITABLE]);
    }

  if (eff_not_zero == 0)
    rawvfscap->magic_etc = GUINT32_TO_LE(magic);
  else
    rawvfscap->magic_etc = GUINT32_TO_LE(magic|VFS_CAP_FLAGS_EFFECTIVE);
}

GVariant *
rpmostree_fcap_to_xattr_variant (const char *fcap)
{
  g_auto(GVariantBuilder) builder;
  cap_t caps = cap_from_text (fcap);
  struct vfs_cap_data vfscap = { 0, };
  g_autoptr(GBytes) vfsbytes = NULL;
  int vfscap_size;

  cap_t_to_vfs (caps, &vfscap, &vfscap_size);
  vfsbytes = g_bytes_new (&vfscap, vfscap_size);

  g_variant_builder_init (&builder, (GVariantType*)"a(ayay)");
  g_variant_builder_add (&builder, "(@ay@ay)",
                         g_variant_new_bytestring ("security.capability"),
                         g_variant_new_from_bytes ((GVariantType*)"ay",
                                                   vfsbytes, FALSE));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Returns the checksum of the RPM we retrieved from the repodata XML. The
 * actual checksum type used depends on how the repodata was created. Thus, the
 * output is a string representation of the form "TYPE:HASH" where TYPE is the
 * name of the checksum employed. In most cases, it will be "sha256" (the
 * current default for `createrepo_c`). */
gboolean
rpmostree_get_repodata_chksum_repr (DnfPackage *pkg,
                                    char      **out_chksum_repr,
                                    GError    **error)
{
  int chksum_type;
  g_autofree char *chksum = NULL;
  const unsigned char *chksum_raw = NULL;

  chksum_raw = dnf_package_get_chksum (pkg, &chksum_type);
  if (chksum_raw)
    chksum = hy_chksum_str (chksum_raw, chksum_type);

  if (chksum == NULL)
    return glnx_throw (error, "Couldn't get chksum for pkg %s",
                       dnf_package_get_nevra(pkg));

  *out_chksum_repr =
    g_strconcat (hy_chksum_name (chksum_type), ":", chksum, NULL);
  return TRUE;
}

GPtrArray*
rpmostree_get_matching_packages (DnfSack *sack,
                                 const char *pattern)
{
  /* mimic dnf_context_install() */
  g_autoptr(GPtrArray) matches = NULL;
  HySelector selector = NULL;
  HySubject subject = NULL;

  subject = hy_subject_create (pattern);
  selector = hy_subject_get_best_selector (subject, sack);
  matches = hy_selector_matches (selector);

  hy_selector_free (selector);
  hy_subject_free (subject);

  return g_steal_pointer (&matches);
}

gboolean
rpmostree_sack_has_subject (DnfSack *sack,
                            const char *pattern)
{
  g_autoptr(GPtrArray) matches =
    rpmostree_get_matching_packages (sack, pattern);
  return matches->len > 0;
}
