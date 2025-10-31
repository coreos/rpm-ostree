/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "rpmostree-builtins.h"
#include "rpmostree-compose-builtins.h"

#include "libglnx.h"
#include <ostree.h>

#include <glib/gi18n.h>

static RpmOstreeCommand compose_subcommands[] = {
  { "tree", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Process a \"treefile\"; install packages and commit the result to an OSTree repository",
    rpmostree_compose_builtin_tree },
  { "install",
    (RpmOstreeBuiltinFlags)(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD
                            | RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT),
    "Install packages into a target path", rpmostree_compose_builtin_install },
  { "postprocess",
    (RpmOstreeBuiltinFlags)(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD
                            | RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT),
    "Perform final postprocessing on an installation root", rpmostree_compose_builtin_postprocess },
  { "commit",
    (RpmOstreeBuiltinFlags)(RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD
                            | RPM_OSTREE_BUILTIN_FLAG_REQUIRES_ROOT),
    "Commit a target path to an OSTree repository", rpmostree_compose_builtin_commit },
  { "extensions", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Download RPM packages guaranteed to depsolve with a base OSTree",
    rpmostree_compose_builtin_extensions },
  { "container-encapsulate", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Generate a reproducible \"chunked\" container image (using RPM data) from an OSTree commit",
    rpmostree_compose_builtin_container_encapsulate },
  { "image", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Generate a reproducible \"chunked\" container image (using RPM data) from a treefile",
    rpmostree_compose_builtin_image },
  { "rootfs", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD, "Generate a root filesystem tree from a treefile",
    rpmostree_compose_builtin_rootfs },
  { "build-chunked-oci", RPM_OSTREE_BUILTIN_FLAG_LOCAL_CMD,
    "Generate a \"chunked\" OCI archive from an input rootfs",
    rpmostree_compose_builtin_build_chunked_oci },
  { NULL, (RpmOstreeBuiltinFlags)0, NULL, NULL }
};

gboolean
rpmostree_builtin_compose (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  return rpmostree_handle_subcommand (argc, argv, compose_subcommands, invocation, cancellable,
                                      error);
}

gboolean
rpmostree_compose_builtin_image (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                 GCancellable *cancellable, GError **error)
{
  rust::Vec<rust::String> rustargv;
  g_assert_cmpint (argc, >, 0);
  for (int i = 0; i < argc; i++)
    rustargv.push_back (std::string (argv[i]));
  CXX_TRY (rpmostreecxx::compose_image (rustargv), error);
  return TRUE;
}

gboolean
rpmostree_compose_builtin_rootfs (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                  GCancellable *cancellable, GError **error)
{
  rust::Vec<rust::String> rustargv;
  g_assert_cmpint (argc, >, 0);
  for (int i = 0; i < argc; i++)
    rustargv.push_back (std::string (argv[i]));
  CXX_TRY (rpmostreecxx::compose_rootfs_entrypoint (rustargv), error);
  return TRUE;
}

gboolean
rpmostree_compose_builtin_build_chunked_oci (int argc, char **argv,
                                             RpmOstreeCommandInvocation *invocation,
                                             GCancellable *cancellable, GError **error)
{
  rust::Vec<rust::String> rustargv;
  g_assert_cmpint (argc, >, 0);
  for (int i = 0; i < argc; i++)
    rustargv.push_back (std::string (argv[i]));
  CXX_TRY (rpmostreecxx::compose_build_chunked_oci_entrypoint (rustargv), error);
  return TRUE;
}
