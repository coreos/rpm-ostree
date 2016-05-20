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

typedef enum {
  RPMOSTREE_OUTPUT_TASK_BEGIN,
  RPMOSTREE_OUTPUT_TASK_END,
  RPMOSTREE_OUTPUT_PERCENT_PROGRESS,
  RPMOSTREE_OUTPUT_PERCENT_PROGRESS_END,
} RpmOstreeOutputType;

void
rpmostree_output_default_handler (RpmOstreeOutputType type, void *data, void *opaque);

void
rpmostree_output_set_callback (void (*cb)(RpmOstreeOutputType, void*, void*), void*);

typedef struct {
  const char *text;
} RpmOstreeOutputTaskBegin;

void
rpmostree_output_task_begin (const char *format, ...) G_GNUC_PRINTF (1,2);

typedef RpmOstreeOutputTaskBegin RpmOstreeOutputTaskEnd;

void
rpmostree_output_task_end (const char *format, ...) G_GNUC_PRINTF (1,2);

typedef struct {
  const char *text;
  guint32 percentage;
} RpmOstreeOutputPercentProgress;

void
rpmostree_output_percent_progress (const char *text, int percentage);

void
rpmostree_output_percent_progress_end (void);
