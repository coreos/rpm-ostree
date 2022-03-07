/*** -*- indent-tabs-mode: nil; tab-width: 8 -*-

  This file was originally part of systemd.

  Copyright 2014 Lennart Poettering
  Copyright 2016 Red Hat, Inc.

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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <libintl.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "libsd-locale-util.h"

const char *
libsd_special_glyph (SpecialGlyph code)
{
  static const char *const draw_table_ascii[_SPECIAL_GLYPH_MAX] = {
    [TREE_VERTICAL] = "| ",    [TREE_BRANCH] = "|-", [TREE_RIGHT] = "`-", [TREE_SPACE] = "  ",
    [TRIANGULAR_BULLET] = ">", [BLACK_CIRCLE] = "*", [ARROW] = "->",      [MDASH] = "-",
  };
  static const char *const draw_table_utf8[_SPECIAL_GLYPH_MAX] = {
    [TREE_VERTICAL] = "\342\224\202 ",          /* │  */
    [TREE_BRANCH] = "\342\224\234\342\224\200", /* ├─ */
    [TREE_RIGHT] = "\342\224\224\342\224\200",  /* └─ */
    [TREE_SPACE] = "  ",                        /*    */
    [TRIANGULAR_BULLET] = "\342\200\243",       /* ‣ */
    [BLACK_CIRCLE] = "\342\227\217",            /* ● */
    [ARROW] = "\342\206\222",                   /* → */
    [MDASH] = "\342\200\223",                   /* – */
  };

  gboolean locale_is_utf8 = g_get_charset (NULL);

  if (locale_is_utf8)
    return draw_table_utf8[code];
  return draw_table_ascii[code];
}
