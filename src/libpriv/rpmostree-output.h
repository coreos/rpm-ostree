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

G_BEGIN_DECLS

typedef enum {
  RPMOSTREE_OUTPUT_MESSAGE,
  RPMOSTREE_OUTPUT_PROGRESS_BEGIN,
  RPMOSTREE_OUTPUT_PROGRESS_UPDATE,
  RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE,
  RPMOSTREE_OUTPUT_PROGRESS_END,
} RpmOstreeOutputType;

typedef enum {
   RPMOSTREE_PROGRESS_TASK,
   RPMOSTREE_PROGRESS_N_ITEMS,
   RPMOSTREE_PROGRESS_PERCENT,
} RpmOstreeProgressType;

void
rpmostree_output_default_handler (RpmOstreeOutputType type, void *data, void *opaque);

void
rpmostree_output_set_callback (void (*cb)(RpmOstreeOutputType, void*, void*), void*);

typedef struct {
  const char *text;
} RpmOstreeOutputMessage;

void
rpmostree_output_message (const char *format, ...) G_GNUC_PRINTF (1,2);

typedef struct {
  bool initialized;
  RpmOstreeProgressType type;
} RpmOstreeProgress;
void rpmostree_output_task_begin (RpmOstreeProgress *prog, const char *format, ...) G_GNUC_PRINTF (2,3);

void rpmostree_output_set_sub_message (const char *sub_message);

void rpmostree_output_progress_nitems_begin (RpmOstreeProgress *prog, guint n, const char *format, ...) G_GNUC_PRINTF (3,4);
void rpmostree_output_progress_n_items (guint i);

void rpmostree_output_progress_percent_begin (RpmOstreeProgress *prog, const char *format, ...) G_GNUC_PRINTF (2,3);
void rpmostree_output_progress_percent (int percentage);

void rpmostree_output_progress_end_msg (RpmOstreeProgress *prog, const char *format, ...) G_GNUC_PRINTF (2,3);
static inline void rpmostree_output_progress_end (RpmOstreeProgress *task)
{
  rpmostree_output_progress_end_msg (task, NULL);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RpmOstreeProgress, rpmostree_output_progress_end)

/* For implementers of the output backend. If percent is TRUE, then n is
 * ignored. If n is zero, then it is taken to be an indefinite task.  Otherwise,
 * n is used for n_items.
 */
typedef struct {
  const char *prefix;
  bool percent;
  guint n;
} RpmOstreeOutputProgressBegin;

/* Update progress */
typedef struct {
  /* If we're in percent mode, this should be between 0 and 100,
   * otherwise less than the total.
   */
  guint c;
} RpmOstreeOutputProgressUpdate;

/* End progress */
typedef struct {
  const char *msg;
} RpmOstreeOutputProgressEnd;

G_END_DECLS