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
#include "rpmostree-rust.h"
#include "rpmostree-cxxrs.h"

/* These are helper functions that automatically determine whether data should
 * be sent through an appropriate D-Bus signal or sent directly to the local
 * terminal. This is helpful in situations in which code may be executed both
 * from the daemon and daemon-less. */

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
  case RPMOSTREE_OUTPUT_PROGRESS_BEGIN:
    {
      auto begin = static_cast<RpmOstreeOutputProgressBegin *>(data);
      if (begin->percent)
        rpmostreecxx::progress_begin_percent (begin->prefix);
      else if (begin->n > 0)
        rpmostreecxx::progress_begin_n_items (begin->prefix, begin->n);
      else
        rpmostreecxx::progress_begin_task (begin->prefix);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_UPDATE:
    {
      auto upd = static_cast<RpmOstreeOutputProgressUpdate *>(data);
      rpmostreecxx::progress_update (upd->c);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE:
    {
      auto msg = static_cast<const char *>(data);
      rpmostreecxx::progress_set_sub_message (rust::Str(msg ?: ""));
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_END:
    {
      auto end = static_cast<RpmOstreeOutputProgressEnd *>(data);
      rpmostreecxx::progress_end (rust::Str(end->msg ?: ""));
      break;
    }
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

void
rpmostree_output_task_begin (RpmOstreeProgress *taskp, const char *format, ...)
{
  g_assert (taskp && !taskp->initialized);
  taskp->initialized = TRUE;
  taskp->type = RPMOSTREE_PROGRESS_TASK;
  g_autofree char *msg = strdup_vprintf (format);
  RpmOstreeOutputProgressBegin begin = { msg, false, 0 };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
}

void
rpmostree_output_set_sub_message (const char *sub_message)
{
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE, (void*)sub_message, active_cb_opaque);
}

void
rpmostree_output_progress_end_msg (RpmOstreeProgress *taskp, const char *format, ...)
{
  g_assert (taskp);
  if (!taskp->initialized)
    return;
  taskp->initialized = false;
  g_autofree char *final_msg = format ? strdup_vprintf (format) : NULL;
  RpmOstreeOutputProgressEnd done = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_END, &done, active_cb_opaque);
}

void
rpmostree_output_progress_percent (int percentage)
{
  RpmOstreeOutputProgressUpdate progress = { (guint)percentage };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_UPDATE, &progress, active_cb_opaque);
}

void
rpmostree_output_progress_nitems_begin (RpmOstreeProgress *taskp,
                                        guint n, const char *format, ...)
{
  g_assert (taskp && !taskp->initialized);
  taskp->initialized = TRUE;
  taskp->type = RPMOSTREE_PROGRESS_N_ITEMS;
  g_autofree char *msg = strdup_vprintf (format);
  RpmOstreeOutputProgressBegin begin = { msg, false, n };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
}

void
rpmostree_output_progress_percent_begin (RpmOstreeProgress *taskp,
                                         const char *format, ...)
{
  g_assert (taskp && !taskp->initialized);
  taskp->initialized = TRUE;
  taskp->type = RPMOSTREE_PROGRESS_PERCENT;
  g_autofree char *msg = strdup_vprintf (format);
  RpmOstreeOutputProgressBegin begin = { msg, true, 0 };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
}

void
rpmostree_output_progress_n_items (guint current)
{
  RpmOstreeOutputProgressUpdate progress = { current };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_UPDATE, &progress, active_cb_opaque);
}
