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
#include <memory>

#include "rpmostree-output.h"
#include "rpmostree-util.h"
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
        rpmostreecxx::console_progress_begin_percent (begin->prefix);
      else if (begin->n > 0)
        rpmostreecxx::console_progress_begin_n_items (begin->prefix, begin->n);
      else
        rpmostreecxx::console_progress_begin_task (begin->prefix);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_UPDATE:
    {
      auto upd = static_cast<RpmOstreeOutputProgressUpdate *>(data);
      rpmostreecxx::console_progress_update (upd->c);
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE:
    {
      auto msg = static_cast<const char *>(data);
      rpmostreecxx::console_progress_set_sub_message (util::ruststr_or_empty(msg));
    }
    break;
  case RPMOSTREE_OUTPUT_PROGRESS_END:
    {
      auto end = static_cast<RpmOstreeOutputProgressEnd *>(data);
      rpmostreecxx::console_progress_end (util::ruststr_or_empty(end->msg));
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

// For a lot of originally-C code it's just convenient to
// use this global C-style format API and not have to deal
// with a progress reference.
void
rpmostree_output_message (const char *format, ...)
{
  g_autofree char *final_msg = strdup_vprintf (format);
  RpmOstreeOutputMessage task = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_MESSAGE, &task, active_cb_opaque);
}

namespace rpmostreecxx {

void
output_message (const rust::Str msg)
{
  auto msg_c = std::string(msg);
  RpmOstreeOutputMessage task = { msg_c.c_str() };
  active_cb (RPMOSTREE_OUTPUT_MESSAGE, &task, active_cb_opaque);
}

// Begin a task (that can't easily be "nitems" or percentage).
// This will render as a spinner.
std::unique_ptr<Progress> 
progress_begin_task(const rust::Str msg) noexcept
{
  auto msg_c = std::string(msg);
  RpmOstreeOutputProgressBegin begin = { msg_c.c_str(), false, 0 };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
  return std::make_unique<Progress>(ProgressType::TASK);
}

// When working on a task/percent/nitems, often we want to display a particular
// item (such as a package).
void 
Progress::set_sub_message(const rust::Str msg)
{
  g_autofree char *msg_c = util::ruststr_dup_c_optempty(msg);
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_SUB_MESSAGE, (void*)msg_c, active_cb_opaque);
}

// Start working on a 0-n task.
std::unique_ptr<Progress> 
progress_nitems_begin(guint n, const rust::Str msg) noexcept
{
  auto msg_c = std::string(msg);
  RpmOstreeOutputProgressBegin begin = { msg_c.c_str(), false, n };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
  return std::make_unique<Progress>(ProgressType::N_ITEMS);
}

// Update the nitems counter.
void 
Progress::nitems_update(guint n)
{
  RpmOstreeOutputProgressUpdate progress = { n };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_UPDATE, &progress, active_cb_opaque);
}

// Start a percentage task.
std::unique_ptr<Progress>
progress_percent_begin(const rust::Str msg) noexcept
{
  auto msg_c = std::string(msg);
  RpmOstreeOutputProgressBegin begin = { msg_c.c_str(), true, 0 };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_BEGIN, &begin, active_cb_opaque);
  return std::make_unique<Progress>(ProgressType::PERCENT);
}

// Update the percentage.
void
Progress::percent_update(guint n)
{
  RpmOstreeOutputProgressUpdate progress = { (guint)n };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_UPDATE, &progress, active_cb_opaque);
}

// End the current task.
void
Progress::end(const rust::Str msg)
{
  g_assert (!this->ended);
  g_autofree char *final_msg = util::ruststr_dup_c_optempty(msg);
  RpmOstreeOutputProgressEnd done = { final_msg };
  active_cb (RPMOSTREE_OUTPUT_PROGRESS_END, &done, active_cb_opaque);
  this->ended = true;
}

} /* namespace */
