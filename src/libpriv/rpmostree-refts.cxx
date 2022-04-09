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

RpmTs::RpmTs (RpmOstreeRefTs *ts) { _ts = ts; }

RpmTs::~RpmTs () { rpmostree_refts_unref (_ts); }

rust::Vec<rust::String>
RpmTs::packages_providing_file (const rust::Str path) const
{
  auto path_c = std::string (path);
  g_auto (rpmdbMatchIterator) mi
      = rpmtsInitIterator (_ts->ts, RPMDBI_INSTFILENAMES, path_c.c_str (), 0);
  if (mi == NULL)
    mi = rpmtsInitIterator (_ts->ts, RPMDBI_PROVIDENAME, path_c.c_str (), 0);
  rust::Vec<rust::String> ret;
  if (mi != NULL)
    {
      Header h;
      while ((h = rpmdbNextIterator (mi)) != NULL)
        {
          ret.push_back (rpmostreecxx::header_get_nevra (h));
        }
    }
  return ret;
}

std::unique_ptr<PackageMeta>
RpmTs::package_meta (const rust::Str name) const
{
  auto name_c = std::string (name);
  g_auto (rpmdbMatchIterator) mi = rpmtsInitIterator (_ts->ts, RPMDBI_NAME, name_c.c_str (), 0);
  if (mi == NULL)
    {
      g_autofree char *err = g_strdup_printf ("Package not found: %s", name_c.c_str ());
      throw std::runtime_error (err);
    }
  Header h;
  std::optional<rust::String> previous;
  auto retval = std::make_unique<PackageMeta> ();
  while ((h = rpmdbNextIterator (mi)) != NULL)
    {
      auto nevra = rpmostreecxx::header_get_nevra (h);
      if (!previous.has_value ())
        {
          previous = std::move (nevra);
          retval->_size = headerGetNumber (h, RPMTAG_LONGARCHIVESIZE);
          retval->_buildtime = headerGetNumber (h, RPMTAG_BUILDTIME);
          retval->_src_pkg = headerGetString (h, RPMTAG_SOURCERPM);
        }
      else
        {
          // TODO: Somehow we get two `libgcc-8.5.0-10.el8.x86_64` in current RHCOS, I don't
          // understand that.
          if (previous != nevra)
            {
              g_autofree char *buf
                  = g_strdup_printf ("Multiple installed '%s' (%s, %s)", name_c.c_str (),
                                     previous.value ().c_str (), nevra.c_str ());
              throw std::runtime_error (buf);
            }
        }
    }
  if (!previous)
    g_assert_not_reached ();
  return retval;
}

}
