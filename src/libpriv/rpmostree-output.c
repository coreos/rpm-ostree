/*
* Copyright (C) 2016 Red Hat, Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "config.h"

#include <ostree.h>
#include <libglnx.h>

#include "rpmostree-output.h"

/* These are helper functions that automatically determine whether data should
 * be sent through an appropriate D-Bus signal or sent directly to the local
 * terminal. This is helpful in situations in which code may be executed both
 * from the daemon and daemon-less. */

static GLnxConsoleRef console;

void
rpmostree_output_default_handler (RpmOstreeOutputType type,
                                  void *data,
                                  void *opaque)
{
  switch (type)
  {
  case RPMOSTREE_OUTPUT_MESSAGE:
    g_print ("%s\n", ((RpmOstreeOutputMessage*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_TASK_BEGIN:
    /* XXX: move to libglnx spinner once it's implemented */
    g_print ("%s... ", ((RpmOstreeOutputTaskBegin*)data)->text);
    break;
  case RPMOSTREE_OUTPUT_TASK_END:
    g_print ("%s\n", ((RpmOstreeOutputTaskEnd*)data)->text ?: "");
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_PERCENT:
    if (!console.locked)
      glnx_console_lock (&console);
    RpmOstreeOutputProgressPercent *prog = data;
    /* let's not spam stdout if it's not a tty; see dbus-helpers.c */
    if (prog->percentage == 100 || console.is_tty)
      glnx_console_progress_text_percent (prog->text, prog->percentage);
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_N_ITEMS:
    {
      RpmOstreeOutputProgressNItems *nitems = data;
      if (!console.locked)
        glnx_console_lock (&console);

      glnx_console_progress_n_items (nitems->text, nitems->current, nitems->total);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_END:
    if (console.locked)
      glnx_console_unlock (&console);
    break;
  }
}

static void (*active_cb)(RpmOstreeOutputType, void*, void*) =
  rpmostree_output_default_handler;

static void *active_cb_opaque;

void
rpmostree_output_set_callback (void (*cb)(RpmOstreeOutputType, void*, void*),
                               void* opaque)
{
  active_cb = cb ?: rpmostree_output_default_handler;
  active_cb_opaque = opaque;
}

#define strdup_vprintf(format)                  \
  ({ va_list args; va_start (args, format);     \
     char *s = g_strdup_vprintf (format, args); \
     va_end (args); s; })

void
rpmostree_output_message (const char *format, ...)
{
  g_autofree char *final_msg = strdup_vprintf (format);
  RpmOstreeOutputMessage task = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_MESSAGE, &task, active_cb_opaque);
}

RpmOstreeOutputTask
rpmostree_output_task_begin (const char *format, ...)
{
  g_autofree char *final_msg = strdup_vprintf (format);
  RpmOstreeOutputTaskBegin task = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_TASK_BEGIN, &task, active_cb_opaque);
  return true;
}

void
rpmostree_output_task_done_msg (RpmOstreeOutputTask *taskp, const char *format, ...)
{
  g_assert (*taskp);
  *taskp = false;
  g_autofree char *final_msg = strdup_vprintf (format);
  RpmOstreeOutputTaskEnd task = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_TASK_END, &task, active_cb_opaque);
}

void
rpmostree_output_progress_percent (const char *text, int percentage)
{
  RpmOstreeOutputProgressPercent progress = { text, percentage };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_PERCENT, &progress, active_cb_opaque);
}

void
rpmostree_output_progress_n_items (const char *text, guint current, guint total)
{
  RpmOstreeOutputProgressNItems progress = { text, current, total };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_N_ITEMS, &progress, active_cb_opaque);
}

void
rpmostree_output_progress_end (void)
{
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_END, NULL, active_cb_opaque);
}
