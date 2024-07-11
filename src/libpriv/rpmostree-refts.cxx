/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "rpmostree-refts.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"
#include <rpm/header.h>
#include <rpm/rpmtag.h>
#include <string.h>

static inline void
cleanup_rpmtdFreeData (rpmtd *tdp)
{
  rpmtd td = *tdp;
  if (td)
    rpmtdFreeData (td);
}
#define _cleanup_rpmtddata_ __attribute__ ((cleanup (cleanup_rpmtdFreeData)))

/*
 * A wrapper for an `rpmts` that supports:
 *
 *  - Reference counting
 *  - Possibly holding a pointer to a tempdir, and cleaning it when unref'd
 */

RpmOstreeRefTs *
rpmostree_refts_new (rpmts ts, GLnxTmpDir *tmpdir)
{
  RpmOstreeRefTs *rts = g_new0 (RpmOstreeRefTs, 1);
  rts->ts = ts;
  rts->refcount = 1;
  if (tmpdir)
    {
      rts->tmpdir = *tmpdir;
      tmpdir->initialized = FALSE; /* Steal ownership */
    }
  return rts;
}

RpmOstreeRefTs *
rpmostree_refts_ref (RpmOstreeRefTs *rts)
{
  g_atomic_int_inc (&rts->refcount);
  return rts;
}

void
rpmostree_refts_unref (RpmOstreeRefTs *rts)
{
  if (!g_atomic_int_dec_and_test (&rts->refcount))
    return;
  rpmtsFree (rts->ts);
  (void)glnx_tmpdir_delete (&rts->tmpdir, NULL, NULL);
  g_free (rts);
}

namespace rpmostreecxx
{

PackageMeta::PackageMeta (::Header h) { _h = headerLink (h); }

PackageMeta::~PackageMeta () { headerFree (_h); }

uint64_t
PackageMeta::size () const
{
  return headerGetNumber (_h, RPMTAG_LONGARCHIVESIZE);
}

uint64_t
PackageMeta::buildtime () const
{
  return headerGetNumber (_h, RPMTAG_BUILDTIME);
}

rust::Str
PackageMeta::src_pkg () const
{
  return headerGetString (_h, RPMTAG_SOURCERPM);
}

rust::String
PackageMeta::nevra () const
{
  return header_get_nevra (_h);
}

rust::Vec<uint64_t>
PackageMeta::changelogs () const
{
  // Get the changelogs
  struct rpmtd_s nchanges_date_s;
  _cleanup_rpmtddata_ rpmtd nchanges_date = NULL;
  nchanges_date = &nchanges_date_s;
  headerGet (_h, RPMTAG_CHANGELOGTIME, nchanges_date, HEADERGET_MINMEM);
  int ncnum = rpmtdCount (nchanges_date);
  rust::Vec<uint64_t> epochs;
  for (int i = 0; i < ncnum; i++)
    {
      uint64_t nchange_date = 0;
      rpmtdNext (nchanges_date);
      nchange_date = rpmtdGetNumber (nchanges_date);
      epochs.push_back (nchange_date);
    }
  return epochs;
}

rust::Vec<rust::String>
PackageMeta::provided_paths () const
{
  g_auto (rpmfi) fi = rpmfiNew (NULL, _h, 0, 0);
  if (fi == NULL)
    throw std::runtime_error ("Failed to allocate file iterator");

  rust::Vec<rust::String> paths;

  rpmfiInit (fi, 0);
  while (rpmfiNext (fi) >= 0)
    {
      // Only include files that are marked as installed
      if (RPMFILE_IS_INSTALLED (rpmfiFState (fi)))
        paths.push_back (rust::String (rpmfiFN (fi)));
    }

  return paths;
}

RpmTs::RpmTs (RpmOstreeRefTs *ts) { _ts = ts; }

RpmTs::~RpmTs () { rpmostree_refts_unref (_ts); }

std::unique_ptr<PackageMeta>
RpmTs::package_meta (const rust::Str name, const rust::Str arch) const
{
  auto name_c = std::string (name);
  auto arch_c = std::string (arch);
  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_NAME, name_c.c_str (), 0);
  if (mi == NULL)
    {
      g_autofree char *err = g_strdup_printf ("Package not found: %s", name_c.c_str ());
      throw std::runtime_error (err);
    }
  Header h;
  std::unique_ptr<PackageMeta> retval;
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      if (strcmp (headerGetString (h, RPMTAG_ARCH), arch_c.c_str ()) == 0)
        {
          // TODO: Somehow we get two `libgcc-8.5.0-10.el8.x86_64` in current RHCOS, I don't
          // understand that.
          if (retval != nullptr)
            {
              auto nevra = header_get_nevra (h);
              g_autofree char *buf
                  = g_strdup_printf ("Multiple installed '%s' (%s, %s)", name_c.c_str (),
                                     retval->nevra ().c_str (), nevra.c_str ());
              throw std::runtime_error (buf);
            }

          retval = std::make_unique<PackageMeta> (h);
        }
    }
  if (retval == nullptr)
    g_assert_not_reached ();
  return retval;
}

}
