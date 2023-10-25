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

#include "rpmostree-core.h"
#include "rpmostree-output.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include <exception>
#include <stdexcept>
#include <string>

#include <fnmatch.h>
#include <inttypes.h>
#include <libglnx.h>
#include <sys/capability.h>
#include <sys/ioctl.h>

#include <rpm/rpmts.h>

static inline void
cleanup_rpmtdFreeData (rpmtd *tdp)
{
  rpmtd td = *tdp;
  if (td)
    rpmtdFreeData (td);
}
#define _cleanup_rpmtddata_ __attribute__ ((cleanup (cleanup_rpmtdFreeData)))

struct RpmRevisionData
{
  struct RpmHeaders *rpmdb;
  RpmOstreeRefTs *refts;
  char *commit;
};

static int
header_name_cmp (Header h1, Header h2)
{
  const char *n1 = headerGetString (h1, RPMTAG_NAME);
  const char *n2 = headerGetString (h2, RPMTAG_NAME);
  int cmp = strcmp (n1, n2);
  return cmp;
}

/* keep this one to be backwards compatible with previously
 * generated checksums */
static char *
pkg_envra_strdup (Header h1)
{
  const char *name = headerGetString (h1, RPMTAG_NAME);
  uint64_t epoch = headerGetNumber (h1, RPMTAG_EPOCH);
  const char *version = headerGetString (h1, RPMTAG_VERSION);
  const char *release = headerGetString (h1, RPMTAG_RELEASE);
  const char *arch = headerGetString (h1, RPMTAG_ARCH);
  char *envra = NULL;

  if (!epoch)
    envra = g_strdup_printf ("%s-%s-%s.%s", name, version, release, arch);
  else
    {
      unsigned long long ullepoch = epoch;
      envra = g_strdup_printf ("%llu:%s-%s-%s.%s", ullepoch, name, version, release, arch);
    }

  return envra;
}

void
rpmostree_custom_nevra (GString *buffer, const char *name, uint64_t epoch, const char *version,
                        const char *release, const char *arch, RpmOstreePkgNevraFlags flags)
{
  gsize original_len = buffer->len;

  if (flags & PKG_NEVRA_FLAGS_NAME)
    {
      g_assert (name);
      g_string_append (buffer, name);
    }

  if (flags & (PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE | PKG_NEVRA_FLAGS_VERSION_RELEASE))
    {
      if (buffer->len > original_len)
        g_string_append_c (buffer, '-');

      if ((flags & PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE) && (epoch > 0))
        g_string_append_printf (buffer, "%" PRIu64 ":", epoch);

      g_string_append_printf (buffer, "%s-%s", version, release);
    }

  if (flags & PKG_NEVRA_FLAGS_ARCH)
    {
      if (buffer->len > original_len)
        g_string_append_c (buffer, '.');
      g_string_append (buffer, arch);
    }
}

char *
rpmostree_custom_nevra_strdup (const char *name, uint64_t epoch, const char *version,
                               const char *release, const char *arch, RpmOstreePkgNevraFlags flags)
{
  GString *nevra = g_string_new ("");
  rpmostree_custom_nevra (nevra, name, epoch, version, release, arch, flags);
  return g_string_free (nevra, FALSE);
}

char *
rpmostree_header_custom_nevra_strdup (Header h, RpmOstreePkgNevraFlags flags)
{
  return rpmostree_custom_nevra_strdup (
      headerGetString (h, RPMTAG_NAME), headerGetNumber (h, RPMTAG_EPOCH),
      headerGetString (h, RPMTAG_VERSION), headerGetString (h, RPMTAG_RELEASE),
      headerGetString (h, RPMTAG_ARCH), flags);
}

static char *
pkg_nevra_strdup (Header h1)
{
  return rpmostree_header_custom_nevra_strdup (
      h1,
      (RpmOstreePkgNevraFlags)(PKG_NEVRA_FLAGS_NAME | PKG_NEVRA_FLAGS_EVR | PKG_NEVRA_FLAGS_ARCH));
}

namespace rpmostreecxx
{

// Return a canonicalized NEVRA string.  Crucially this differs from the librpm
// version which may or may not return an explicit `0` epoch.  This version
// always omits both unset and zero epoch.
rust::String
header_get_nevra (Header h)
{
  g_autofree char *v = pkg_nevra_strdup (h);
  return rust::String (v);
}
}

static char *
pkg_na_strdup (Header h1)
{
  return rpmostree_header_custom_nevra_strdup (
      h1, (RpmOstreePkgNevraFlags)(PKG_NEVRA_FLAGS_NAME | PKG_NEVRA_FLAGS_ARCH));
}

static char *
pkg_nvr_strdup (Header h1)
{
  return rpmostree_header_custom_nevra_strdup (
      h1, (RpmOstreePkgNevraFlags)(PKG_NEVRA_FLAGS_NAME | PKG_NEVRA_FLAGS_VERSION_RELEASE));
}

static char *
pkg_evra_strdup (Header h1)
{
  return rpmostree_header_custom_nevra_strdup (
      h1, (RpmOstreePkgNevraFlags)(PKG_NEVRA_FLAGS_EPOCH_VERSION_RELEASE | PKG_NEVRA_FLAGS_ARCH));
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
      auto pat = static_cast<const char *> (patterns->pdata[num]);
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

      ret = MIN (ret, prefix);
    }

  return ret;
}

static gboolean
pat_fnmatch_match (Header pkg, const char *name, gsize patprefixlen, const GPtrArray *patterns)
{
  int num = 0;
  g_autofree char *pkg_na = NULL;
  g_autofree char *pkg_nevra = NULL;
  g_autofree char *pkg_nvr = NULL;

  if (!patterns)
    return TRUE;

  for (num = 0; num < patterns->len; num++)
    {
      auto pattern = static_cast<const char *> (patterns->pdata[num]);

      if (patprefixlen && !CASENCMP_EQ (name, pattern, patprefixlen))
        continue;

      if (!pkg_na)
        {
          pkg_nevra = pkg_nevra_strdup (pkg);
          pkg_na = pkg_na_strdup (pkg);
          pkg_nvr = pkg_nvr_strdup (pkg);
        }

      if (CASEFNMATCH_EQ (pattern, name) || CASEFNMATCH_EQ (pattern, pkg_nevra)
          || CASEFNMATCH_EQ (pattern, pkg_na) || CASEFNMATCH_EQ (pattern, pkg_nvr) || FALSE)
        return TRUE;
    }

  return FALSE;
}

static void
header_free_p (gpointer data)
{
  headerFree (static_cast<Header> (data));
}

static int
header_cmp_p (gconstpointer gph1, gconstpointer gph2)
{
  auto h1 = (Header *)gph1;
  auto h2 = (Header *)gph2;
  int cmp = header_name_cmp (*h1, *h2);
  if (!cmp)
    cmp = rpmVersionCompare (*h1, *h2);
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
      const char *name = headerGetString (h1, RPMTAG_NAME);

      if (g_str_equal (name, "gpg-pubkey"))
        continue; /* rpmdb abstraction leak */

      if (!pat_fnmatch_match (h1, name, patprefixlen, patterns))
        continue;

      h1 = headerLink (h1);
      g_ptr_array_add (hs, h1);
    }
  iter = rpmdbFreeIterator (iter);
  (void)iter;

  g_ptr_array_sort (hs, header_cmp_p);

  ret = (struct RpmHeaders *)g_malloc0 (sizeof (struct RpmHeaders));

  ret->refts = rpmostree_refts_ref (refts);
  ret->hs = hs;

  return ret;
}

void
rpmhdrs_free (struct RpmHeaders *hdrs)
{
  if (!hdrs)
    return;

  g_ptr_array_free (hdrs->hs, TRUE);
  hdrs->hs = NULL;
  rpmostree_refts_unref (hdrs->refts);

  g_free (hdrs);
}

static struct RpmHeadersDiff *
rpmhdrs_diff_new (void)
{
  auto ret = static_cast<struct RpmHeadersDiff *> (g_malloc0 (sizeof (struct RpmHeadersDiff)));

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
rpmhdrs_diff (struct RpmHeaders *l1, struct RpmHeaders *l2)
{
  int n1 = 0;
  int n2 = 0;
  struct RpmHeadersDiff *ret = rpmhdrs_diff_new ();

  while (n1 < l1->hs->len)
    {
      auto h1 = static_cast<Header> (l1->hs->pdata[n1]);
      if (n2 >= l2->hs->len)
        {
          g_ptr_array_add (ret->hs_del, h1);
          ++n1;
        }
      else
        {
          auto h2 = static_cast<Header> (l2->hs->pdata[n2]);
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
              if (!cmp)
                {
                  ++n1;
                  ++n2;
                  continue;
                }

              g_ptr_array_add (ret->hs_mod_old, h1);
              ++n1;
              g_ptr_array_add (ret->hs_mod_new, h2);
              ++n2;
            }
        }
    }

  while (n2 < l2->hs->len)
    {
      auto h2 = static_cast<Header> (l2->hs->pdata[n2]);

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
      auto h1 = static_cast<Header> (l1->hs->pdata[num++]);
      g_print (" ");
      pkg_print (h1);
    }
}

char *
rpmhdrs_rpmdbv (struct RpmHeaders *l1, GCancellable *cancellable, GError **error)
{
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  int num = 0;
  while (num < l1->hs->len)
    {
      auto pkg = static_cast<Header> (l1->hs->pdata[num++]);
      g_autofree char *envra = pkg_envra_strdup (pkg);

      g_checksum_update (checksum, (guint8 *)envra, strlen (envra));
    }

  return g_strdup_printf ("%u:%s", num, g_checksum_get_string (checksum));
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
      ptr[0] = ptr[num - 1];
      ptr[num - 1] = swap;

      num -= 2;
      ptr++;
    }
}

static int
_rpmhdrs_diff_cmp_end (const GPtrArray *hs1, const GPtrArray *hs2)
{
  if (!hs2->len)
    return -1;
  if (!hs1->len)
    return 1;

  auto h1 = static_cast<Header> (hs1->pdata[hs1->len - 1]);
  auto h2 = static_cast<Header> (hs2->pdata[hs2->len - 1]);

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

      /* used to avoid fetching SOURCERPM from the same header twice */
      const char *next_srpm = NULL;
      for (num = 0; num < diff->hs_mod_new->len; ++num)
        {
          auto ho = static_cast<Header> (diff->hs_mod_old->pdata[num]);
          auto hn = static_cast<Header> (diff->hs_mod_new->pdata[num]);
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
          uint64_t ochange_date = 0;
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

          /* RPMs from the same SRPM share the same changelogs; just peek ahead and skip if
           * the next one has the same SRPM; i.e. this effectively groups consecutive RPMs
           * from the same SRPM. */
          const char *current_srpm = next_srpm ?: headerGetString (ho, RPMTAG_SOURCERPM);
          if (num == diff->hs_mod_old->len - 1)
            next_srpm = NULL;
          else
            {
              auto next_ho = static_cast<Header> (diff->hs_mod_old->pdata[num + 1]);
              next_srpm = headerGetString (next_ho, RPMTAG_SOURCERPM);
            }
          if (g_strcmp0 (current_srpm, next_srpm) == 0)
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
              uint64_t nchange_date = 0;
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
              if ((ochange_date == nchange_date) && g_str_equal (ochange_name, nchange_name)
                  && g_str_equal (ochange_text, nchange_text))
                break;

#define CHANGELOG_INDENTATION "    "

              g_autofree char *indented_nchange_text = NULL;
              if (strchr (nchange_text, '\n'))
                {
                  g_auto (GStrv) lines = g_strsplit (nchange_text, "\n", 0);
                  indented_nchange_text = g_strjoinv ("\n" CHANGELOG_INDENTATION, lines);
                }

              /* Otherwise, print. */
              dt = g_date_time_new_from_unix_utc (nchange_date);
              date_time_str = g_date_time_format (dt, "%a %b %d %Y");
              g_date_time_unref (dt);

              g_print (CHANGELOG_INDENTATION "* %s %s\n" CHANGELOG_INDENTATION "%s\n\n",
                       date_time_str, nchange_name, indented_nchange_text ?: nchange_text);

#undef CHANGELOG_INDENTATION

              --ncnum;
            }
        }

      done = FALSE;
      for (num = 0; num < diff->hs_mod_new->len; ++num)
        {
          auto ho = static_cast<Header> (diff->hs_mod_old->pdata[num]);
          auto hn = static_cast<Header> (diff->hs_mod_new->pdata[num]);

          g_assert (!header_name_cmp (ho, hn));
          if (rpmVersionCompare (ho, hn) < 0)
            continue;

          if (!done)
            {
              done = TRUE;
              g_print ("Downgraded:\n");
            }

          g_print ("  ");
          pkg_print_changed (ho, hn);
        }
    }

  if (diff->hs_del->len)
    {
      g_print ("Removed:\n");

      for (num = 0; num < diff->hs_del->len; ++num)
        {
          auto hd = static_cast<Header> (diff->hs_del->pdata[num]);

          g_print ("  ");
          pkg_print (hd);
        }
    }

  if (diff->hs_add->len)
    {
      g_print ("Added:\n");

      for (num = 0; num < diff->hs_add->len; ++num)
        {
          auto ha = static_cast<Header> (diff->hs_add->pdata[num]);

          g_print ("  ");
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

  while (diff->hs_add->len || diff->hs_del->len || diff->hs_mod_old->len)
    {
      if (_rpmhdrs_diff_cmp_end (diff->hs_mod_old, diff->hs_del) < 0)
        if (_rpmhdrs_diff_cmp_end (diff->hs_mod_old, diff->hs_add) < 0)
          { /* mod is first */
            auto hm = static_cast<Header> (diff->hs_mod_old->pdata[diff->hs_mod_old->len - 1]);

            g_print ("!");
            pkg_print (hm);
            g_ptr_array_remove_index (diff->hs_mod_old, diff->hs_mod_old->len - 1);
            g_print ("=");
            hm = static_cast<Header> (diff->hs_mod_new->pdata[diff->hs_mod_new->len - 1]);
            pkg_print (hm);
            g_ptr_array_remove_index (diff->hs_mod_new, diff->hs_mod_new->len - 1);
          }
        else
          { /* add is first */
            auto ha = static_cast<Header> (diff->hs_add->pdata[diff->hs_add->len - 1]);

            g_print ("+");
            pkg_print (ha);
            g_ptr_array_remove_index (diff->hs_add, diff->hs_add->len - 1);
          }
      else if (_rpmhdrs_diff_cmp_end (diff->hs_del, diff->hs_add) < 0)
        { /* del is first */
          auto hd = static_cast<Header> (diff->hs_del->pdata[diff->hs_del->len - 1]);

          g_print ("-");
          pkg_print (hd);
          g_ptr_array_remove_index (diff->hs_del, diff->hs_del->len - 1);
        }
      else
        { /* add is first */
          auto ha = static_cast<Header> (diff->hs_add->pdata[diff->hs_add->len - 1]);

          g_print ("+");
          pkg_print (ha);
          g_ptr_array_remove_index (diff->hs_add, diff->hs_add->len - 1);
        }
    }

  rpmhdrs_diff_free (diff);
}

struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo, const char *rev, const GPtrArray *patterns, GCancellable *cancellable,
            GError **error)
{
  g_autofree char *commit = NULL;
  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &commit, error))
    return NULL;

  g_autoptr (RpmOstreeRefTs) refts = NULL;
  if (!rpmostree_get_refts_for_commit (repo, commit, &refts, cancellable, error))
    return NULL;

  auto rpmrev = static_cast<RpmRevisionData *> (g_malloc0 (sizeof (struct RpmRevisionData)));
  rpmrev->refts = util::move_nullify (refts);
  rpmrev->commit = util::move_nullify (commit);
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

/* Currently, our rpmdb lives at /usr/share/rpm. Add symlinks from the legacy
 * /var/lib/rpm *and* the future /usr/lib/sysimage/rpm paths to that. */
static gboolean
mk_rpmdb_compat_symlinks (int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  if (!glnx_shutil_mkdir_p_at (rootfs_dfd, "var/lib", 0777, cancellable, error))
    return FALSE;
  if (symlinkat ("../../" RPMOSTREE_RPMDB_LOCATION, rootfs_dfd, "var/lib/rpm") == -1)
    return glnx_throw_errno_prefix (error, "symlinkat");

  if (!glnx_shutil_mkdir_p_at (rootfs_dfd, RPMOSTREE_SYSIMAGE_DIR, 0777, cancellable, error))
    return FALSE;
  if (symlinkat ("../../../" RPMOSTREE_RPMDB_LOCATION, rootfs_dfd, RPMOSTREE_SYSIMAGE_RPMDB) == -1)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
}

/* Check out a copy of the rpmdb into @tmpdir */
static gboolean
checkout_only_rpmdb (OstreeRepo *repo, const char *ref, const char *rpmdb, GLnxTmpDir *tmpdir,
                     GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("rpmdb checkout", error);
  g_autofree char *commit = NULL;
  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &commit, error))
    return FALSE;

  /* Create intermediate dirs */
  if (!glnx_shutil_mkdir_p_at (tmpdir->fd, "usr/share", 0777, cancellable, error))
    return FALSE;

  /* Check out the database (via copy) */
  OstreeRepoCheckoutAtOptions checkout_options = {
    (OstreeRepoCheckoutMode)0,
  };
  checkout_options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  const char *subpath = glnx_strjoina ("/", rpmdb);
  checkout_options.subpath = subpath;
  if (!ostree_repo_checkout_at (repo, &checkout_options, tmpdir->fd, RPMOSTREE_RPMDB_LOCATION,
                                commit, cancellable, error))
    return FALSE;

  if (!mk_rpmdb_compat_symlinks (tmpdir->fd, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
get_sack_for_root (int dfd, const char *path, DnfSack **out_sack, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading sack", error);
  g_assert (out_sack != NULL);

  ROSCXX_TRY (core_libdnf_process_global_init (), error);

  g_autofree char *fullpath = glnx_fdrel_abspath (dfd, path);

  g_autoptr (DnfSack) sack = dnf_sack_new ();
  dnf_sack_set_rootdir (sack, fullpath);

  if (!dnf_sack_setup (sack, 0, error))
    return FALSE;

  if (!dnf_sack_load_system_repo (sack, NULL, 0, error))
    return FALSE;

  *out_sack = util::move_nullify (sack);
  return TRUE;
}

/* Given @dfd + @path, return a "sack", i.e. database of packages.
 */
RpmOstreeRefSack *
rpmostree_get_refsack_for_root (int dfd, const char *path, GError **error)
{
  g_autoptr (DnfSack) sack = NULL; /* NB: refsack adds a ref to it */
  if (!get_sack_for_root (dfd, path, &sack, error))
    return NULL;
  return rpmostree_refsack_new (sack, NULL);
}

/* Given @dfd + @path, return a sack corresponding to the base layer (which is the same as
 * /usr/share/rpm if it's not a layered deployment.
 *
 * Note this function may return %TRUE without a sack if the deployment predates when we
 * started embedding RPMOSTREE_BASE_RPMDB.
 */
gboolean
rpmostree_get_base_refsack_for_root (int dfd, const char *path, RpmOstreeRefSack **out_sack,
                                     GCancellable *cancellable, GError **error)
{
  g_autofree char *subpath = g_build_filename (path, RPMOSTREE_BASE_RPMDB, NULL);
  if (!glnx_fstatat_allow_noent (dfd, subpath, NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT)
    return TRUE;

  g_auto (GLnxTmpDir) tmpdir = {
    0,
  };
  if (!glnx_mkdtemp ("rpmostree-dbquery-XXXXXX", 0700, &tmpdir, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (tmpdir.fd, dirname (strdupa (RPMOSTREE_RPMDB_LOCATION)), 0777,
                               cancellable, error))
    return FALSE;

  g_autofree char *base_rpm = glnx_fdrel_abspath (dfd, subpath);
  if (symlinkat (base_rpm, tmpdir.fd, RPMOSTREE_RPMDB_LOCATION) == -1)
    return glnx_throw_errno_prefix (error, "symlinkat");

  if (!mk_rpmdb_compat_symlinks (tmpdir.fd, cancellable, error))
    return FALSE;

  g_autoptr (DnfSack) sack = NULL; /* NB: refsack adds a ref to it */
  if (!get_sack_for_root (tmpdir.fd, ".", &sack, error))
    return FALSE;

  *out_sack = rpmostree_refsack_new (sack, &tmpdir);
  return TRUE;
}

/* Given @ref which is an OSTree ref, return a "sack" i.e. database of packages.
 */
RpmOstreeRefSack *
rpmostree_get_refsack_for_commit (OstreeRepo *repo, const char *ref, GCancellable *cancellable,
                                  GError **error)
{
  g_auto (GLnxTmpDir) tmpdir = {
    0,
  };
  if (!glnx_mkdtemp ("rpmostree-dbquery-XXXXXX", 0700, &tmpdir, error))
    return NULL;

  if (!checkout_only_rpmdb (repo, ref, RPMOSTREE_RPMDB_LOCATION, &tmpdir, cancellable, error))
    return NULL;

  g_autoptr (DnfSack) hsack = NULL; /* NB: refsack adds a ref to it */
  if (!get_sack_for_root (tmpdir.fd, ".", &hsack, error))
    return NULL;

  /* Ownership of tmpdir is transferred */
  return rpmostree_refsack_new (hsack, &tmpdir);
}

/* Return a sack for the "base" rpmdb without any layering/overrides/etc.
 * involved.
 */
RpmOstreeRefSack *
rpmostree_get_base_refsack_for_commit (OstreeRepo *repo, const char *ref, GCancellable *cancellable,
                                       GError **error)
{
  g_auto (GLnxTmpDir) tmpdir = {
    0,
  };
  if (!glnx_mkdtemp ("rpmostree-dbquery-XXXXXX", 0700, &tmpdir, error))
    return NULL;

  /* This is a bit of a hack; we checkout the "base" dbpath as /usr/share/rpm in
   * a temporary root. Fixing this would require patching through new APIs into
   * libdnf → libsolv to teach it about a way to find a user-specified dbpath.
   */
  if (!checkout_only_rpmdb (repo, ref, RPMOSTREE_BASE_RPMDB, &tmpdir, cancellable, error))
    return NULL;

  g_autoptr (DnfSack) hsack = NULL; /* NB: refsack adds a ref to it */
  if (!get_sack_for_root (tmpdir.fd, ".", &hsack, error))
    return NULL;

  /* Ownership of tmpdir is transferred */
  return rpmostree_refsack_new (hsack, &tmpdir);
}

static RpmOstreeRefTs *
get_refts_for_rootfs (const char *rootfs, GLnxTmpDir *tmpdir)
{
  g_assert ((rootfs != NULL) || (tmpdir != NULL));
  g_assert ((rootfs == NULL) || (tmpdir == NULL));

  rpmts ts = rpmtsCreate ();
  /* This actually makes sense because we know we've verified it at build time */
  rpmtsSetVSFlags (ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES);

  int r = rpmtsSetRootDir (ts, rootfs ?: tmpdir->path);
  g_assert_cmpint (r, ==, 0);

  /* Ownership of tmpdir is transferred */
  return rpmostree_refts_new (ts, tmpdir);
}

gboolean
rpmostree_get_refts_for_commit (OstreeRepo *repo, const char *ref, RpmOstreeRefTs **out_ts,
                                GCancellable *cancellable, GError **error)

{
  ROSCXX_TRY (core_libdnf_process_global_init (), error);
  g_auto (GLnxTmpDir) tmpdir = {
    0,
  };
  if (!glnx_mkdtemp ("rpmostree-dbquery-XXXXXX", 0700, &tmpdir, error))
    return FALSE;

  if (!checkout_only_rpmdb (repo, ref, RPMOSTREE_RPMDB_LOCATION, &tmpdir, cancellable, error))
    return FALSE;

  /* Ownership of tmpdir is transferred */
  *out_ts = get_refts_for_rootfs (NULL, &tmpdir);
  return TRUE;
}

gint
rpmostree_pkg_array_compare (DnfPackage **p_pkg1, DnfPackage **p_pkg2)
{
  return dnf_package_cmp (*p_pkg1, *p_pkg2);
}

static void
print_pkglist (GPtrArray *pkglist)
{
  g_ptr_array_sort (pkglist, (GCompareFunc)rpmostree_pkg_array_compare);

  for (guint i = 0; i < pkglist->len; i++)
    {
      auto pkg = static_cast<DnfPackage *> (pkglist->pdata[i]);
      rpmostree_output_message ("  %s (%s)", dnf_package_get_nevra (pkg),
                                dnf_package_get_reponame (pkg));
    }
}

void
rpmostree_print_transaction (DnfContext *dnfctx)
{
  gboolean empty = TRUE;

  {
    g_autoptr (GPtrArray) packages = NULL;
    packages = dnf_goal_get_packages (dnf_context_get_goal (dnfctx), DNF_PACKAGE_INFO_INSTALL,
                                      DNF_PACKAGE_INFO_REINSTALL, DNF_PACKAGE_INFO_DOWNGRADE,
                                      DNF_PACKAGE_INFO_UPDATE, -1);

    if (packages->len > 0)
      {
        empty = FALSE;
        rpmostree_output_message ("Installing %u packages:", packages->len);
        print_pkglist (packages);
      }
  }

  {
    g_autoptr (GPtrArray) packages = NULL;
    packages = dnf_goal_get_packages (dnf_context_get_goal (dnfctx), DNF_PACKAGE_INFO_REMOVE,
                                      DNF_PACKAGE_INFO_OBSOLETE, -1);

    if (packages->len > 0)
      {
        empty = FALSE;
        rpmostree_output_message ("Removing %u packages:", packages->len);
        print_pkglist (packages);
      }
  }

  if (empty)
    rpmostree_output_message ("Empty transaction");
}

struct _cap_struct
{
  struct __user_cap_header_struct head;
  union
  {
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
      rawvfscap->data[i].permitted = GUINT32_TO_LE (cap_d->u[i].flat[CAP_PERMITTED]);
      rawvfscap->data[i].inheritable = GUINT32_TO_LE (cap_d->u[i].flat[CAP_INHERITABLE]);
    }

  if (eff_not_zero == 0)
    rawvfscap->magic_etc = GUINT32_TO_LE (magic);
  else
    rawvfscap->magic_etc = GUINT32_TO_LE (magic | VFS_CAP_FLAGS_EFFECTIVE);
}

// Given a file capability string, convert it to the convention ostree uses
// to an store extended attribute value: (@ay@ay)
GVariant *
rpmostree_fcap_to_ostree_xattr (const char *fcap)
{
  cap_t caps = cap_from_text (fcap);
  struct vfs_cap_data vfscap = {
    0,
  };

  int vfscap_size;
  cap_t_to_vfs (caps, &vfscap, &vfscap_size);
  cap_free (caps);
  g_autoptr (GBytes) vfsbytes = g_bytes_new (&vfscap, vfscap_size);

  return g_variant_ref_sink (
      g_variant_new ("(@ay@ay)", g_variant_new_bytestring ("security.capability"),
                     g_variant_new_from_bytes ((GVariantType *)"ay", vfsbytes, FALSE)));
}

// Given a file capability string, convert it to a single-element array
// of ostree-convention GVariant format.
GVariant *
rpmostree_fcap_to_xattr_variant (const char *fcap)
{

  g_autoptr (GVariant) v = rpmostree_fcap_to_ostree_xattr (fcap);
  g_auto (GVariantBuilder) builder;
  g_variant_builder_init (&builder, (GVariantType *)"a(ayay)");
  g_variant_builder_add_value (&builder, v);
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

namespace rpmostreecxx
{
/* Returns the checksum of the RPM we retrieved from the repodata XML. The
 * actual checksum type used depends on how the repodata was created. Thus, the
 * output is a string representation of the form "TYPE:HASH" where TYPE is the
 * name of the checksum employed. In most cases, it will be "sha256" (the
 * current default for `createrepo_c`). */
rust::String
get_repodata_chksum_repr (DnfPackage &pkg)
{
  static GMutex mutex;
  int chksum_type;
  g_autofree char *chksum = NULL;
  const unsigned char *chksum_raw = NULL;

  /* Have to do this under lock since chksum_raw can be invalidated by another
   * thread; the right fix for this is to not use libsolv in a multi-threaded
   * context, but that's a larger refactor.
   * See https://github.com/coreos/rpm-ostree/issues/4565. */
  g_mutex_lock (&mutex);
  /* for rpmdb packages, use the hdr checksum */
  if (dnf_package_installed (&pkg))
    chksum_raw = dnf_package_get_hdr_chksum (&pkg, &chksum_type);
  else
    chksum_raw = dnf_package_get_chksum (&pkg, &chksum_type);
  if (chksum_raw)
    chksum = hy_chksum_str (chksum_raw, chksum_type);
  g_mutex_unlock (&mutex);

  if (chksum == NULL)
    {
      // TODO try out the tinyformat.hpp in libdnf
      g_autofree char *msg
          = g_strdup_printf ("Couldn't get chksum for pkg %s", dnf_package_get_nevra (&pkg));
      throw std::runtime_error (msg);
    }

  // Would be nice to expose a format!() equivalent on rust::String
  std::ostringstream ss;
  ss << hy_chksum_name (chksum_type) << ":" << chksum;
  return rust::String (ss.str ());
}
}

GPtrArray *
rpmostree_get_matching_packages (DnfSack *sack, const char *pattern)
{
  /* mimic dnf_context_install() */
  g_autoptr (GPtrArray) matches = NULL;
  HySelector selector = NULL;
  HySubject subject = NULL;

  subject = hy_subject_create (pattern);
  selector = hy_subject_get_best_selector (subject, sack, NULL, FALSE, NULL);
  matches = hy_selector_matches (selector);

  hy_selector_free (selector);
  hy_subject_free (subject);

  return util::move_nullify (matches);
}

gboolean
rpmostree_sack_has_subject (DnfSack *sack, const char *pattern)
{
  g_autoptr (GPtrArray) matches = rpmostree_get_matching_packages (sack, pattern);
  return matches->len > 0;
}

/* Errors out if multiple pkgs match pkgname. Otherwise, sets out_pkg to the found pkg, or
 * NULL if not found. */
gboolean
rpmostree_sack_get_by_pkgname (DnfSack *sack, const char *pkgname, DnfPackage **out_pkg,
                               GError **error)
{
  g_autoptr (DnfPackage) ret_pkg = NULL;
  hy_autoquery HyQuery query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkgname);
  g_autoptr (GPtrArray) pkgs = hy_query_run (query);

  if (pkgs->len > 1)
    return glnx_throw (error, "Multiple packages match \"%s\"", pkgname);
  else if (pkgs->len == 1)
    ret_pkg = (DnfPackage *)g_object_ref (pkgs->pdata[0]);
  else /* for obviousness */
    ret_pkg = NULL;

  *out_pkg = util::move_nullify (ret_pkg);
  return TRUE;
}

gboolean
rpmostree_sack_has_pkgname (DnfSack *sack, const char *pkgname)
{
  g_autoptr (DnfPackage) pkg = NULL;
  if (rpmostree_sack_get_by_pkgname (sack, pkgname, &pkg, NULL) && pkg)
    return TRUE;
  return FALSE;
}

GPtrArray *
rpmostree_sack_get_packages (DnfSack *sack)
{
  hy_autoquery HyQuery query = hy_query_create (sack);
  hy_query_filter (query, HY_PKG_REPONAME, HY_EQ, HY_SYSTEM_REPO_NAME);
  return hy_query_run (query);
}

GPtrArray *
rpmostree_sack_get_sorted_packages (DnfSack *sack)
{
  g_autoptr (GPtrArray) pkglist = rpmostree_sack_get_packages (sack);
  g_ptr_array_sort (pkglist, (GCompareFunc)rpmostree_pkg_array_compare);
  return util::move_nullify (pkglist);
}

GVariant *
rpmostree_variant_pkgs_from_sack (RpmOstreeRefSack *refsack)
{
  /* we insert it sorted here so it can efficiently be searched on retrieval */
  g_autoptr (GPtrArray) pkglist = rpmostree_sack_get_sorted_packages (refsack->sack);

  GVariantBuilder pkglist_v_builder;
  g_variant_builder_init (&pkglist_v_builder, (GVariantType *)"a(sssss)");

  const guint n = pkglist->len;
  for (guint i = 0; i < n; i++)
    {
      auto pkg = static_cast<DnfPackage *> (pkglist->pdata[i]);

      /* put epoch as a string so we're indifferent to endianness -- also note that unlike
       * librpm, libdnf doesn't care about unset vs 0 epoch and neither do we */
      g_autofree char *epoch = g_strdup_printf ("%" PRIu64, dnf_package_get_epoch (pkg));
      g_variant_builder_add (&pkglist_v_builder, "(sssss)", dnf_package_get_name (pkg), epoch,
                             dnf_package_get_version (pkg), dnf_package_get_release (pkg),
                             dnf_package_get_arch (pkg));
    }

  return g_variant_ref_sink (g_variant_builder_end (&pkglist_v_builder));
}

gboolean
rpmostree_create_rpmdb_pkglist_variant (int dfd, const char *path, GVariant **out_variant,
                                        GCancellable *cancellable, GError **error)
{
  g_autoptr (RpmOstreeRefSack) refsack = rpmostree_get_refsack_for_root (dfd, path, error);
  if (!refsack)
    return FALSE;

  *out_variant = rpmostree_variant_pkgs_from_sack (refsack);
  return TRUE;
}

namespace rpmostreecxx
{
rust::Vec<rust::String>
rpmdb_package_name_list (gint32 dfd, rust::String path)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (RpmOstreeRefSack) refsack
      = rpmostree_get_refsack_for_root (dfd, path.c_str (), &local_error);
  if (!refsack)
    throw std::runtime_error (local_error->message);

  rust::Vec<rust::String> r;
  g_autoptr (GPtrArray) pkglist = rpmostree_sack_get_sorted_packages (refsack->sack);
  const guint n = pkglist->len;
  for (guint i = 0; i < n; i++)
    {
      auto pkg = static_cast<DnfPackage *> (pkglist->pdata[i]);
      r.push_back (rust::String (dnf_package_get_name (pkg)));
    }

  return r;
}
}

/* Simple wrapper around hy_split_nevra() that adds allow-none and GError convention */
gboolean
rpmostree_decompose_nevra (const char *nevra, char **out_name, /* allow-none */
                           guint64 *out_epoch,                 /* allow-none */
                           char **out_version,                 /* allow-none */
                           char **out_release,                 /* allow-none */
                           char **out_arch,                    /* allow-none */
                           GError **error)
{
  g_autofree char *name = NULL;
  int epoch;
  g_autofree char *version = NULL;
  g_autofree char *release = NULL;
  g_autofree char *arch = NULL;

  if (hy_split_nevra (nevra, &name, &epoch, &version, &release, &arch) != 0)
    return glnx_throw (error, "Failed to decompose NEVRA string '%s'", nevra);

  if (out_name)
    *out_name = util::move_nullify (name);
  if (out_epoch)
    *out_epoch = epoch; /* note widening */
  if (out_version)
    *out_version = util::move_nullify (version);
  if (out_release)
    *out_release = util::move_nullify (release);
  if (out_arch)
    *out_arch = util::move_nullify (arch);

  return TRUE;
}

gboolean
rpmostree_is_valid_nevra (const char *subject)
{
  return rpmostree_decompose_nevra (subject, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* translates NEVRA to its cache branch */
namespace rpmostreecxx
{
rust::String
nevra_to_cache_branch (const std::string &nevra)
{
  /* It's cumbersome, but we have to decompose the NEVRA and the rebuild it into a cache
   * branch. Something something Rust slices... */
  g_autoptr (GError) local_error = NULL;
  g_autofree char *name = NULL;
  guint64 epoch = 0;
  g_autofree char *version = NULL;
  g_autofree char *release = NULL;
  g_autofree char *arch = NULL;

  if (!rpmostree_decompose_nevra (nevra.c_str (), &name, &epoch, &version, &release, &arch,
                                  &local_error))
    util::throw_gerror (local_error);
  g_assert (name);
  g_assert (version);
  g_assert (release);
  g_assert (arch);

  g_autofree char *evr
      = rpmostree_custom_nevra_strdup (name, epoch, version, release, arch, PKG_NEVRA_FLAGS_EVR);
  g_autofree char *branch = rpmostree_get_cache_branch_for_n_evr_a (name, evr, arch);
  return rust::String (util::move_nullify (branch));
}
}

/* Quote/squash all non-(alphanumeric plus `.` and `-`); this
 * is used to map RPM package NEVRAs into ostree branch names.
 */
static void
append_quoted (GString *r, const char *value)
{
  const char *p;

  for (p = value; *p; p++)
    {
      const char c = *p;
      switch (c)
        {
        case '.':
        case '-':
          g_string_append_c (r, c);
          continue;
        }
      if (g_ascii_isalnum (c))
        {
          g_string_append_c (r, c);
          continue;
        }
      if (c == '_')
        {
          g_string_append (r, "__");
          continue;
        }

      g_string_append_printf (r, "_%02X", c);
    }
}

/* Return the ostree cache branch for a nevra */
static char *
get_branch_for_n_evr_a (const char *type, const char *name, const char *evr, const char *arch)
{
  GString *r = g_string_new ("rpmostree/");
  g_string_append (r, type);
  g_string_append_c (r, '/');
  append_quoted (r, name);
  g_string_append_c (r, '/');
  /* Work around the fact that libdnf and librpm have different handling
   * for an explicit Epoch header of zero.  libdnf right now ignores it,
   * but librpm will add an explicit 0:.  Since there's no good reason
   * to have it, let's follow the lead of libdnf.
   *
   * https://github.com/projectatomic/rpm-ostree/issues/349
   */
  if (g_str_has_prefix (evr, "0:"))
    evr += 2;
  append_quoted (r, evr);
  g_string_append_c (r, '.');
  append_quoted (r, arch);
  return g_string_free (r, FALSE);
}

char *
rpmostree_get_cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch)
{
  return get_branch_for_n_evr_a ("pkg", name, evr, arch);
}

/* Return the ostree cache branch from a Header */
char *
rpmostree_get_cache_branch_header (Header hdr)
{
  g_autofree char *name = headerGetAsString (hdr, RPMTAG_NAME);
  g_autofree char *evr = headerGetAsString (hdr, RPMTAG_EVR);
  g_autofree char *arch = headerGetAsString (hdr, RPMTAG_ARCH);
  return rpmostree_get_cache_branch_for_n_evr_a (name, evr, arch);
}

/* Return the ostree cache branch from a libdnf Package */
char *
rpmostree_get_cache_branch_pkg (DnfPackage *pkg)
{
  return rpmostree_get_cache_branch_for_n_evr_a (
      dnf_package_get_name (pkg), dnf_package_get_evr (pkg), dnf_package_get_arch (pkg));
}

GPtrArray *
rpmostree_get_enabled_rpmmd_repos (DnfContext *dnfctx, DnfRepoEnabled enablement)
{
  g_autoptr (GPtrArray) ret = g_ptr_array_new ();
  GPtrArray *repos = dnf_context_get_repos (dnfctx);

  g_assert (repos);
  for (guint i = 0; i < repos->len; i++)
    {
      auto repo = static_cast<DnfRepo *> (repos->pdata[i]);
      if (dnf_repo_get_enabled (repo) & enablement)
        g_ptr_array_add (ret, repo);
    }
  return util::move_nullify (ret);
}

/* convert those now to make the D-Bus API nicer and easier for clients */
static RpmOstreeAdvisorySeverity
str2severity (const char *str)
{
  if (str == NULL)
    return RPM_OSTREE_ADVISORY_SEVERITY_NONE;

  /* these expect RHEL naming conventions; Fedora hopefully should follow soon, see:
   * https://github.com/fedora-infra/bodhi/pull/2099 */
  g_autofree char *str_up = g_ascii_strup (str, -1);
  if (g_str_equal (str_up, "LOW"))
    return RPM_OSTREE_ADVISORY_SEVERITY_LOW;
  if (g_str_equal (str_up, "MODERATE"))
    return RPM_OSTREE_ADVISORY_SEVERITY_MODERATE;
  if (g_str_equal (str_up, "IMPORTANT"))
    return RPM_OSTREE_ADVISORY_SEVERITY_IMPORTANT;
  if (g_str_equal (str_up, "CRITICAL"))
    return RPM_OSTREE_ADVISORY_SEVERITY_CRITICAL;
  return RPM_OSTREE_ADVISORY_SEVERITY_NONE;
}

static int
compare_advisory_refs (gconstpointer ap, gconstpointer bp)
{
  /* We use URLs to sort here; often CVE RHBZs will have multiple duplicates for each stream
   * affected (e.g. Fedora, EPEL, RHEL, etc...), but we want the first one, which contains
   * all the juicy details. A naive strcmp() sort on the URL gives us this. */
  DnfAdvisoryRef *a = *((DnfAdvisoryRef **)ap);
  DnfAdvisoryRef *b = *((DnfAdvisoryRef **)bp);

  g_assert (a);
  g_assert (b);

  /* just use g_strcmp0() here to tolerate NULL URLs for now if that somehow happens... we
   * filter them out later when going through all the references */
  return g_strcmp0 (dnf_advisoryref_get_url (a), dnf_advisoryref_get_url (b));
}

/* Returns a *floating* variant ref representing the advisory */
static GVariant *
advisory_variant_new (DnfAdvisory *adv, GPtrArray *pkgs)
{
  static gsize cve_regex_initialized;
  static GRegex *cve_regex;

#define CVE_REGEXP "CVE-[0-9]+-[0-9]+"

  if (g_once_init_enter (&cve_regex_initialized))
    {
      cve_regex = g_regex_new ("\\b" CVE_REGEXP "\\b", static_cast<GRegexCompileFlags> (0),
                               static_cast<GRegexMatchFlags> (0), NULL);
      g_assert (cve_regex);
      g_once_init_leave (&cve_regex_initialized, 1);
    }

#undef CVE_REGEXP

  g_auto (GVariantBuilder) builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
  g_variant_builder_add (&builder, "s", dnf_advisory_get_id (adv));
  g_variant_builder_add (&builder, "u", dnf_advisory_get_kind (adv));
  g_variant_builder_add (&builder, "u", str2severity (dnf_advisory_get_severity (adv)));

  {
    g_auto (GVariantBuilder) pkgs_array;
    g_variant_builder_init (&pkgs_array, G_VARIANT_TYPE ("as"));
    for (guint i = 0; i < pkgs->len; i++)
      g_variant_builder_add (&pkgs_array, "s",
                             dnf_package_get_nevra (static_cast<DnfPackage *> (pkgs->pdata[i])));
    g_variant_builder_add_value (&builder, g_variant_builder_end (&pkgs_array));
  }

  /* final a{sv} for any additional metadata */
  g_auto (GVariantDict) dict;
  g_variant_dict_init (&dict, NULL);

  {
    g_auto (GVariantBuilder) cve_references;
    g_variant_builder_init (&cve_references, G_VARIANT_TYPE ("a(ss)"));

    /* we maintain a set to make sure we only add the earliest ref for each CVE */
    g_autoptr (GHashTable) created_cves
        = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    g_autoptr (GPtrArray) refs = dnf_advisory_get_references (adv);
    g_ptr_array_sort (refs, compare_advisory_refs);

    /* for each ref, look for CVEs in their title, and add an (ss) for refs which mention
     * new CVEs. */
    for (guint i = 0; i < refs->len; i++)
      {
        auto ref = static_cast<DnfAdvisoryRef *> (refs->pdata[i]);
        if (dnf_advisoryref_get_kind (ref) != DNF_REFERENCE_KIND_BUGZILLA)
          continue;

        g_autoptr (GMatchInfo) match = NULL;
        const char *title = dnf_advisoryref_get_title (ref);
        const char *url = dnf_advisoryref_get_url (ref);

        if (!url || !title)
          continue;

        if (!g_regex_match (cve_regex, title, static_cast<GRegexMatchFlags> (0), &match))
          continue;

        /* collect all the found CVEs first */
        g_autoptr (GPtrArray) found_cves = g_ptr_array_new_with_free_func (g_free);
        while (g_match_info_matches (match))
          {
            g_ptr_array_add (found_cves, g_match_info_fetch (match, 0));
            g_match_info_next (match, NULL);
          }

        gboolean has_new_cve = FALSE;
        for (guint i = 0; i < found_cves->len && !has_new_cve; i++)
          has_new_cve = !g_hash_table_contains (created_cves, found_cves->pdata[i]);

        /* if a single CVE is new, make a GVariant for it */
        if (has_new_cve)
          {
            g_variant_builder_add (&cve_references, "(ss)", url, title);
            /* steal all the cves and transfer to set, autofree'ing dupes */
            g_ptr_array_add (found_cves, NULL);
            g_autofree char **cves
                = (char **)g_ptr_array_free (util::move_nullify (found_cves), FALSE);
            for (char **cve = cves; cve && *cve; cve++)
              g_hash_table_add (created_cves, *cve);
          }
      }
    g_variant_dict_insert_value (&dict, "cve_references", g_variant_builder_end (&cve_references));
  }
  g_variant_builder_add_value (&builder, g_variant_dict_end (&dict));

  return g_variant_builder_end (&builder);
}

/* libdnf creates new DnfAdvisory objects on request */

static guint
advisory_hash (gconstpointer v)
{
  return g_str_hash (dnf_advisory_get_id ((DnfAdvisory *)v));
}

static gboolean
advisory_equal (gconstpointer v1, gconstpointer v2)
{
  return g_str_equal (dnf_advisory_get_id ((DnfAdvisory *)v1),
                      dnf_advisory_get_id ((DnfAdvisory *)v2));
}

/* adds noop-on-NULL semantics so we can steal in advisories_variant() */
static void
advisory_free (gpointer p)
{
  auto adv = static_cast<DnfAdvisory *> (p);
  if (adv)
    dnf_advisory_free (adv);
}

/* Go through the list of @pkgs and check if there are any advisories open for them. If
 * no advisories are found, returns %NULL. Otherwise, returns a GVariant of the type
 * RPMOSTREE_UPDATE_ADVISORY_GVARIANT_FORMAT.
 */
GVariant *
rpmostree_advisories_variant (DnfSack *sack, GPtrArray *pkgs)
{
  g_autoptr (GHashTable) advisories = g_hash_table_new_full (
      advisory_hash, advisory_equal, advisory_free, (GDestroyNotify)g_ptr_array_unref);

  /* libdnf provides pkg -> set of advisories, but we want advisory -> set of pkgs;
   * making sure we only keep the pkgs we actually care about */
  for (guint i = 0; i < pkgs->len; i++)
    {
      auto pkg = static_cast<DnfPackage *> (pkgs->pdata[i]);
      g_autoptr (GPtrArray) advisories_with_pkg = dnf_package_get_advisories (pkg, HY_EQ);
      for (guint j = 0; j < advisories_with_pkg->len; j++)
        {
          auto advisory = static_cast<DnfAdvisory *> (advisories_with_pkg->pdata[j]);

          /* for now we're only interested in security erratas */
          if (dnf_advisory_get_kind (advisory) != DNF_ADVISORY_KIND_SECURITY)
            continue;

          /* reverse mapping */
          auto pkgs_in_advisory
              = static_cast<GPtrArray *> (g_hash_table_lookup (advisories, advisory));
          if (!pkgs_in_advisory)
            {
              /* take it out of the array, transferring ownership to the hash table; there's
               * g_ptr_array_steal_index() we could use, but it's still very new */
              advisories_with_pkg->pdata[j] = NULL;
              pkgs_in_advisory = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
              g_hash_table_insert (advisories, advisory, pkgs_in_advisory);
            }
          g_ptr_array_add (pkgs_in_advisory, g_object_ref (pkg));
        }
    }

  if (g_hash_table_size (advisories) == 0)
    return NULL;

  g_auto (GVariantBuilder) builder;
  g_variant_builder_init (&builder, RPMOSTREE_UPDATE_ADVISORY_GVARIANT_FORMAT);
  GLNX_HASH_TABLE_FOREACH_KV (advisories, DnfAdvisory *, advisory, GPtrArray *, pkgs)
    g_variant_builder_add_value (&builder, advisory_variant_new (advisory, pkgs));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

namespace rpmostreecxx
{

std::unique_ptr<RpmTs>
rpmts_for_commit (const OstreeRepo &repo, rust::Str rev)
{
  g_autoptr (GError) local_error = NULL;
  RpmOstreeRefTs *refts = NULL;
  auto rev_c = std::string (rev);
  if (!rpmostree_get_refts_for_commit (&const_cast<OstreeRepo &> (repo), rev_c.c_str (), &refts,
                                       NULL, &local_error))
    util::throw_gerror (local_error);

  return std::make_unique<RpmTs> (refts);
}

}
