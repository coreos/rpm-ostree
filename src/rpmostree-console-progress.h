/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#include <libgsystem.h>

G_BEGIN_DECLS

void	 rpmostree_console_progress_start		(void);

void	 rpmostree_console_progress_text_percent        (const char     *text,
                                                         guint		 percentage);

void	 rpmostree_console_progress_end			(void);

static inline void
rpmostree_console_progress_cleanup (void **p)
{
  rpmostree_console_progress_end ();
}

#define _cleanup_rpmostree_console_progress_ __attribute__((cleanup(rpmostree_console_progress_cleanup)))

G_END_DECLS
