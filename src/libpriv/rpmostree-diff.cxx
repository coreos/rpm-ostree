/*
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

#include <memory>

#include "rpmostree-db.h"
#include "rpmostree-diff.hpp"
#include "rpmostree-util.h"

// Only used by Rust side.
namespace rpmostreecxx
{

RPMDiff::RPMDiff (GPtrArray *removed, GPtrArray *added, GPtrArray *modified_old,
                  GPtrArray *modified_new)
{
  removed_ = g_ptr_array_ref (removed);
  added_ = g_ptr_array_ref (added);
  modified_old_ = g_ptr_array_ref (modified_old);
  modified_new_ = g_ptr_array_ref (modified_new);
}

RPMDiff::~RPMDiff ()
{
  g_ptr_array_unref (removed_);
  g_ptr_array_unref (added_);
  g_ptr_array_unref (modified_old_);
  g_ptr_array_unref (modified_new_);
}

std::unique_ptr<RPMDiff>
rpmdb_diff (OstreeRepo &repo, const std::string &from, const std::string &to, bool allow_noent)
{
  g_autoptr (GPtrArray) removed = NULL;
  g_autoptr (GPtrArray) added = NULL;
  g_autoptr (GPtrArray) modified_old = NULL;
  g_autoptr (GPtrArray) modified_new = NULL;
  g_autoptr (GError) local_error = NULL;

  int flags = 0;
  if (allow_noent)
    flags |= RPM_OSTREE_DB_DIFF_EXT_ALLOW_NOENT;

  if (!rpm_ostree_db_diff_ext (&repo, from.c_str (), to.c_str (), (RpmOstreeDbDiffExtFlags)flags,
                               &removed, &added, &modified_old, &modified_new, NULL, &local_error))
    util::throw_gerror (local_error);
  return std::make_unique<RPMDiff> (removed, added, modified_old, modified_new);
}

void
RPMDiff::print () const
{
  rpmostree_diff_print_formatted (RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE, NULL, 0, removed_,
                                  added_, modified_old_, modified_new_);
}

} /* namespace */
