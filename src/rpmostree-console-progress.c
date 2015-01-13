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

#include "config.h"

#include "rpmostree-console-progress.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>

static char *current_text = NULL;
static gint current_percent = -1;

static volatile guint cached_columns = 0;
static volatile guint cached_lines = 0;

static int
fd_columns (int fd)
{
  struct winsize ws = {};

  if (ioctl (fd, TIOCGWINSZ, &ws) < 0)
    return -errno;

  if (ws.ws_col <= 0)
    return -EIO;

  return ws.ws_col;
}

static guint
columns (void)
{
  if (G_UNLIKELY (cached_columns == 0))
    {
      int c;

      c = fd_columns (STDOUT_FILENO);

      if (c <= 0)
        c = 80;

      if (c > 256)
        c = 256;

      cached_columns = c;
    }

  return cached_columns;
}

#if 0
static int
fd_lines (int fd)
{
  struct winsize ws = {};

  if (ioctl (fd, TIOCGWINSZ, &ws) < 0)
    return -errno;

  if (ws.ws_row <= 0)
    return -EIO;

  return ws.ws_row;
}

static guint
lines (void)
{
  if (G_UNLIKELY (cached_lines == 0))
    {
      int l;

      l = fd_lines (STDOUT_FILENO);

      if (l <= 0)
        l = 24;

      cached_lines = l;
    }

  return cached_lines;
}
#endif

static void
on_sigwinch (int signum)
{
  cached_columns = 0;
  cached_lines = 0;
}

void
rpmostree_console_progress_start (void)
{
  GSConsole *console = gs_console_get ();
  static gsize sigwinch_initialized = 0;

  if (!console)
    return;
  
  current_percent = 0;

  if (g_once_init_enter (&sigwinch_initialized))
    {
      signal (SIGWINCH, on_sigwinch);
      g_once_init_leave (&sigwinch_initialized, 1);
    }

  { static const char initbuf[] = { '\n', 0x1B, 0x37 };
    (void) fwrite (initbuf, 1, sizeof (initbuf), stdout);
  }
}

static void
printpad (const char *padbuf,
          guint       padbuf_len,
          guint       n)
{
  const guint d = n / padbuf_len;
  const guint r = n % padbuf_len;
  guint i;

  for (i = 0; i < d; i++)
    fwrite (padbuf, 1, padbuf_len, stdout);
  fwrite (padbuf, 1, r, stdout);
}

void
rpmostree_console_progress_text_percent (const char *text,
                                         guint percentage)
{
  static const char equals[] = "====================";
  const guint n_equals = sizeof (equals) - 1;
  static const char spaces[] = "                    ";
  const guint n_spaces = sizeof (spaces) - 1;
  GSConsole *console = gs_console_get ();
  const guint ncolumns = columns ();
  const guint bar_min = 10;
  const guint input_textlen = text ? strlen (text) : 0;
  guint textlen;
  guint barlen;

  if (!console)
    return;

  g_return_if_fail (percentage <= 100);
  if (text && !*text)
    text = NULL;

  if (percentage == current_percent
      && g_strcmp0 (text, current_text) == 0)
    return;

  if (ncolumns < bar_min)
    return; /* TODO: spinner */

  /* Restore cursor */
  { const char beginbuf[2] = { 0x1B, 0x38 };
    (void) fwrite (beginbuf, 1, sizeof (beginbuf), stdout);
  }

  textlen = MIN (input_textlen, ncolumns - bar_min);
  barlen = ncolumns - textlen;

  if (textlen > 0)
    {
      fwrite (text, 1, textlen - 1, stdout);
      fputc (' ', stdout);
    }
  
  { 
    const guint nbraces = 2;
    const guint textpercent_len = 5;
    const guint bar_internal_len = barlen - nbraces - textpercent_len;
    const guint eqlen = bar_internal_len * (percentage / 100.0);
    const guint spacelen = bar_internal_len - eqlen; 

    fputc ('[', stdout);
    printpad (equals, n_equals, eqlen);
    printpad (spaces, n_spaces, spacelen);
    fputc (']', stdout);
    fprintf (stdout, " %3d%%", percentage);
  }

  { const guint spacelen = ncolumns - textlen - barlen;
    printpad (spaces, n_spaces, spacelen);
  }

  fflush (stdout);
}

void
rpmostree_console_progress_end (void)
{
  current_percent = -1;
  g_clear_pointer (&current_text, g_free);
  fputc ('\n', stdout);
}
