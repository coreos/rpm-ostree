/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Anne LoVerso <anne.loverso@students.olin.edu>
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

#pragma once

#include <ostree.h>

#include "rpmostree-builtins.h"

G_BEGIN_DECLS

gboolean rpmostree_db_builtin_diff (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                    GCancellable *cancellable, GError **error);
gboolean rpmostree_db_builtin_list (int argc, char **argv, RpmOstreeCommandInvocation *invocation,
                                    GCancellable *cancellable, GError **error);
gboolean rpmostree_db_builtin_version (int argc, char **argv,
                                       RpmOstreeCommandInvocation *invocation,
                                       GCancellable *cancellable, GError **error);

gboolean rpmostree_db_option_context_parse (GOptionContext *context,
                                            const GOptionEntry *main_entries, int *argc,
                                            char ***argv, RpmOstreeCommandInvocation *invocation,
                                            OstreeRepo **out_repo, GCancellable *cancellable,
                                            GError **error);

G_END_DECLS
