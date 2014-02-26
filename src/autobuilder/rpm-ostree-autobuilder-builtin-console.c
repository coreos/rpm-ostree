/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>
#include <readline/readline.h>

#include "libgsystem.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

int
main (int argc, char **argv)
{
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  GOptionContext *context = g_option_context_new ("- netcat console");
  gs_unref_object GSocketAddress *address = NULL;
  gs_unref_object GSocketClient *socketclient = NULL;
  gs_unref_object GSocketConnection *socketconn = NULL;
  GOutputStream *out;
  GInputStream *in;
  gs_unref_object GDataInputStream *datain = NULL;
  const char *address_arg;

  g_option_context_add_main_entries (context, option_entries, NULL);

  if (argc > 1)
    address_arg = argv[1];
  else
    address_arg = "cmd.socket";

  address = g_unix_socket_address_new (address_arg);
  socketclient = g_socket_client_new ();
  socketconn = g_socket_client_connect (socketclient,
                                        (GSocketConnectable*)address,
                                        cancellable, error);
  if (!socketconn)
    goto out;

  out = g_io_stream_get_output_stream ((GIOStream*)socketconn);
  in = g_io_stream_get_input_stream ((GIOStream*)socketconn);
  datain = g_data_input_stream_new (in);
  
  while (TRUE)
    {
      char *line = readline ("> ");
      gsize bytes_written;
      const guint8 nl = '\n';

      if (!line)
        break;
      
      if (!g_output_stream_write_all (out, line, strlen (line),
                                      &bytes_written,
                                      cancellable, error))
        goto out;
      
      free (line);
      
      if (!g_output_stream_write_all (out, &nl, 1,
                                      &bytes_written,
                                      cancellable, error))
        goto out;
                                      
      {
      gsize bytes_read;
        gs_free char *resline =
          g_data_input_stream_read_line_utf8 (datain,
                                              &bytes_read,
                                              cancellable, error);

        if (!resline)
          goto out;

        g_print ("%s\n", resline);
      }
    }
  
 out:
  if (local_error)
    {
      g_printerr ("%s\n", local_error->message);
      g_error_free (local_error);
      return 1;
    }
  return 0;
}

