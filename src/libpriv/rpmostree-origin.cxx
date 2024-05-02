/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include "string.h"
#include <systemd/sd-journal.h>

#include "libglnx.h"
#include "rpmostree-core.h"
#include "rpmostree-origin.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-util.h"

struct RpmOstreeOrigin
{
  guint refcount;

  /* this is the single source of truth */
  std::optional<rust::Box<rpmostreecxx::Treefile> > treefile;
};

RpmOstreeOrigin *
rpmostree_origin_ref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  origin->refcount++;
  return origin;
}

void
rpmostree_origin_unref (RpmOstreeOrigin *origin)
{
  g_assert (origin);
  g_assert_cmpint (origin->refcount, >, 0);
  origin->refcount--;
  if (origin->refcount > 0)
    return;
  origin->treefile.reset ();
  g_free (origin);
}

RpmOstreeOrigin *
rpmostree_origin_parse_deployment (OstreeDeployment *deployment, GError **error)
{
  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  if (!origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment));
      return NULL;
    }
  return rpmostree_origin_parse_keyfile (origin, error);
}

RpmOstreeOrigin *
rpmostree_origin_parse_keyfile (GKeyFile *origin, GError **error)
{
  g_autoptr (RpmOstreeOrigin) ret = NULL;
  ret = g_new0 (RpmOstreeOrigin, 1);
  ret->refcount = 1;
  ret->treefile = ROSCXX_VAL (origin_to_treefile (*origin), error);
  return util::move_nullify (ret);
}

/* Mutability: getter */
RpmOstreeOrigin *
rpmostree_origin_dup (RpmOstreeOrigin *origin)
{
  g_autoptr (GKeyFile) kf = rpmostreecxx::treefile_to_origin (**origin->treefile);
  g_autoptr (GError) local_error = NULL;
  RpmOstreeOrigin *ret = rpmostree_origin_parse_keyfile (kf, &local_error);
  g_assert_no_error (local_error);
  return ret;
}

/* Mutability: getter */
rpmostreecxx::Refspec
rpmostree_origin_get_refspec (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_base_refspec ();
}

rust::String
rpmostree_origin_get_custom_url (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_origin_custom_url ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_custom_description (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_origin_custom_description ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages ();
}

bool
rpmostree_origin_has_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_packages ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_local_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_local_packages ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_local_fileoverride_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_local_fileoverride_packages ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_overrides_remove (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages_override_remove ();
}

bool
rpmostree_origin_has_overrides_remove_name (RpmOstreeOrigin *origin, const char *name)
{
  return (*origin->treefile)->has_packages_override_remove_name (rust::Str (name));
}

/* Mutability: getter */
rust::Vec<rpmostreecxx::OverrideReplacement>
rpmostree_origin_get_overrides_replace (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages_override_replace ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_overrides_local_replace (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_packages_override_replace_local ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_override_commit (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_override_commit ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_etc_files ();
}

bool
rpmostree_origin_has_initramfs_etc_files (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_initramfs_etc_files ();
}

/* Mutability: getter */
bool
rpmostree_origin_get_regenerate_initramfs (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_regenerate ();
}

/* Mutability: getter */
rust::Vec<rust::String>
rpmostree_origin_get_initramfs_args (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_initramfs_args ();
}

/* Mutability: getter */
rust::String
rpmostree_origin_get_unconfigured_state (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_unconfigured_state ();
}

/* Mutability: getter */
bool
rpmostree_origin_may_require_local_assembly (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->may_require_local_assembly ();
}

/* Mutability: getter */
bool
rpmostree_origin_has_any_packages (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->has_any_packages ();
}

/* Mutability: getter */
GKeyFile *
rpmostree_origin_dup_keyfile (RpmOstreeOrigin *origin)
{
  // XXX: we should be able to make this conversion infallible
  return rpmostreecxx::treefile_to_origin (**origin->treefile);
}

/* Mutability: setter */
bool
rpmostree_origin_initramfs_etc_files_track (RpmOstreeOrigin *origin, rust::Vec<rust::String> paths)
{
  return (*origin->treefile)->initramfs_etc_files_track (paths);
}

/* Mutability: setter */
bool
rpmostree_origin_initramfs_etc_files_untrack (RpmOstreeOrigin *origin,
                                              rust::Vec<rust::String> paths)
{
  return (*origin->treefile)->initramfs_etc_files_untrack (paths);
}

/* Mutability: setter */
bool
rpmostree_origin_initramfs_etc_files_untrack_all (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->initramfs_etc_files_untrack_all ();
}

/* Mutability: setter */
void
rpmostree_origin_set_regenerate_initramfs (RpmOstreeOrigin *origin, gboolean regenerate,
                                           rust::Vec<rust::String> args)
{
  (*origin->treefile)->set_initramfs_regenerate (regenerate, args);
}

/* Mutability: setter */
void
rpmostree_origin_set_override_commit (RpmOstreeOrigin *origin, const char *checksum)
{
  (*origin->treefile)->set_override_commit (checksum ?: "");
}

/* Mutability: getter */
bool
rpmostree_origin_get_cliwrap (RpmOstreeOrigin *origin)
{
  return (*origin->treefile)->get_cliwrap ();
}

/* Mutability: setter */
void
rpmostree_origin_set_cliwrap (RpmOstreeOrigin *origin, bool cliwrap)
{
  (*origin->treefile)->set_cliwrap (cliwrap);
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase_custom (RpmOstreeOrigin *origin, const char *new_refspec,
                                    const char *custom_origin_url,
                                    const char *custom_origin_description)
{
  (*origin->treefile)
      ->rebase (new_refspec, custom_origin_url ?: "", custom_origin_description ?: "");
}

/* Mutability: setter */
void
rpmostree_origin_set_rebase (RpmOstreeOrigin *origin, const char *new_refspec)
{
  rpmostree_origin_set_rebase_custom (origin, new_refspec, NULL, NULL);
}

static void
set_changed (gboolean *out, gboolean c)
{
  if (!out)
    return;
  *out = *out || c;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                               gboolean allow_existing, gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->add_packages (packages, allow_existing), error);
  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_local_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                                     gboolean allow_existing, gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->add_local_packages (packages, allow_existing), error);
  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_local_fileoverride_packages (RpmOstreeOrigin *origin,
                                                  rust::Vec<rust::String> packages,
                                                  gboolean allow_existing, gboolean *out_changed,
                                                  GError **error)
{
  CXX_TRY_VAR (changed,
               (*origin->treefile)->add_local_fileoverride_packages (packages, allow_existing),
               error);
  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_packages (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                                  gboolean allow_noent, gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->remove_packages (packages, allow_noent), error);
  set_changed (out_changed, changed);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_all_packages (RpmOstreeOrigin *origin)
{
  auto changed = (*origin->treefile)->remove_all_packages ();
  return changed;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_override_remove (RpmOstreeOrigin *origin, rust::Vec<rust::String> packages,
                                      GError **error)
{
  CXX_TRY ((*origin->treefile)->add_packages_override_remove (packages), error);
  return TRUE;
}

/* Mutability: setter */
gboolean
rpmostree_origin_add_override_replace_local (RpmOstreeOrigin *origin,
                                             rust::Vec<rust::String> packages, GError **error)
{
  CXX_TRY ((*origin->treefile)->add_packages_override_replace_local (packages), error);
  return TRUE;
}

/* Returns FALSE if the override does not exist. */
/* Mutability: setter */
gboolean
rpmostree_origin_remove_override_remove (RpmOstreeOrigin *origin, const char *package)
{
  auto changed = (*origin->treefile)->remove_package_override_remove (package);
  return changed;
}

gboolean
rpmostree_origin_remove_override_replace_local (RpmOstreeOrigin *origin, const char *package)
{
  auto changed = (*origin->treefile)->remove_package_override_replace_local (package);
  return changed;
}

gboolean
rpmostree_origin_remove_override_replace (RpmOstreeOrigin *origin, const char *package)
{
  auto changed = (*origin->treefile)->remove_package_override_replace (package);
  return changed;
}

/* Mutability: setter */
gboolean
rpmostree_origin_remove_all_overrides (RpmOstreeOrigin *origin)
{
  auto changed = (*origin->treefile)->remove_all_overrides ();
  return changed;
}

/* Mutability: setter */
gboolean
rpmostree_origin_merge_treefile (RpmOstreeOrigin *origin, const char *treefile,
                                 gboolean *out_changed, GError **error)
{
  CXX_TRY_VAR (changed, (*origin->treefile)->merge_treefile (treefile), error);
  set_changed (out_changed, changed);
  return TRUE;
}
