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

#pragma once

#include <stdbool.h>

typedef enum {
  RPMOSTREE_OUTPUT_MESSAGE,
  RPMOSTREE_OUTPUT_TASK_BEGIN,
  RPMOSTREE_OUTPUT_TASK_END,
  RPMOSTREE_OUTPUT_PROGRESS_N_ITEMS,
  RPMOSTREE_OUTPUT_PROGRESS_PERCENT,
  RPMOSTREE_OUTPUT_PROGRESS_END,
} RpmOstreeOutputType;

void
rpmostree_output_default_handler (RpmOstreeOutputType type, void *data, void *opaque);

void
rpmostree_output_set_callback (void (*cb)(RpmOstreeOutputType, void*, void*), void*);

typedef struct {
  const char *text;
} RpmOstreeOutputMessage;

void
rpmostree_output_message (const char *format, ...) G_GNUC_PRINTF (1,2);

typedef RpmOstreeOutputMessage RpmOstreeOutputTaskBegin;


typedef RpmOstreeOutputMessage RpmOstreeOutputTaskEnd;

typedef bool RpmOstreeOutputTask;
RpmOstreeOutputTask rpmostree_output_task_begin (const char *format, ...) G_GNUC_PRINTF (1,2);
void rpmostree_output_task_done_msg (RpmOstreeOutputTask *task, const char *format, ...) G_GNUC_PRINTF (2,3);
static inline void
rpmostree_output_task_clear (RpmOstreeOutputTask *task)
{
  if (*task)
    rpmostree_output_task_done_msg (task, NULL);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmOstreeOutputTask, rpmostree_output_task_clear)


typedef struct {
  const char *text;
  guint32 percentage;
} RpmOstreeOutputProgressPercent;

typedef struct {
  const char *text;
  guint current;
  guint total;
} RpmOstreeOutputProgressNItems;

void
rpmostree_output_progress_n_items (const char *text, guint current, guint total);

void
rpmostree_output_progress_percent (const char *text, int percentage);

void
rpmostree_output_progress_end (void);
