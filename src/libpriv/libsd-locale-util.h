#pragma once

/***
  This file was originally part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  TREE_VERTICAL,
  TREE_BRANCH,
  TREE_RIGHT,
  TREE_SPACE,
  TRIANGULAR_BULLET,
  BLACK_CIRCLE,
  ARROW,
  MDASH,
  _SPECIAL_GLYPH_MAX
} SpecialGlyph;

const char *libsd_special_glyph (SpecialGlyph code) __attribute__ ((const));

G_END_DECLS
