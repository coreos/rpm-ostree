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

#include "rpmostree-dbus-helpers.h"
#include "libgsystem.h"

/**
* utils_is_valid_object_path
* @string: string to check
*
* Like g_variant_is_object_path except an
* empty object path (/) is considered invalid
**/
gboolean
rpmostree_is_valid_object_path (gchar *string)
{
  return string != NULL && g_strcmp0("/", string) != 0 && \
         g_variant_is_object_path (string);
}


RPMOSTreeSysroot *
rpmostree_get_sysroot_proxy (GDBusConnection *connection,
                             gchar *sysroot_arg,
                             GCancellable *cancellable,
                             GError **error)
{
  gs_unref_object RPMOSTreeManager *manager = NULL;
  RPMOSTreeSysroot *sysroot = NULL;
  gs_free gchar *sysroot_path = NULL;

  manager = rpmostree_manager_proxy_new_sync (connection,
                                              G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                              BUS_NAME,
                                              "/org/projectatomic/rpmostree1/Manager",
                                              cancellable,
                                              error);

  if (!manager)
    goto out;

  if (!rpmostree_manager_call_get_sysroot_sync (manager,
                                                sysroot_arg,
                                                &sysroot_path,
                                                NULL, error))
      goto out;

  sysroot = rpmostree_sysroot_proxy_new_sync (connection,
                                         G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                         BUS_NAME,
                                         sysroot_path,
                                         cancellable,
                                         error);

out:
  return sysroot;
}

/**
* refspec_console_get_progress_line
* @refspec: RPMOSTreeRefSpec
*
* Similar to ostree_repo_pull_default_console_progress_changed
*
* Displays outstanding fetch progress in bytes/sec,
* or else outstanding content or metadata writes to the repository in
* number of objects.
**/
static gchar *
refspec_console_get_progress_line (guint64 start_time,
                                   guint64 elapsed_secs,
                                   guint outstanding_fetches,
                                   guint outstanding_writes,
                                   guint n_scanned_metadata,
                                   guint metadata_fetched,
                                   guint outstanding_metadata_fetches,
                                   guint total_delta_parts,
                                   guint fetched_delta_parts,
                                   guint total_delta_superblocks,
                                   guint64 total_delta_part_size,
                                   guint fetched,
                                   guint requested,
                                   guint64 bytes_transferred,
                                   guint64 bytes_sec)
{
  GString *buf;

  buf = g_string_new ("");

  if (outstanding_fetches)
    {
      gs_free char *formatted_bytes_transferred = g_format_size_full (bytes_transferred, 0);
      gs_free char *formatted_bytes_sec = NULL;

      if (!bytes_sec)
        formatted_bytes_sec = g_strdup ("-");
      else
        formatted_bytes_sec = g_format_size (bytes_sec);

      if (total_delta_parts > 0)
        {
          gs_free char *formatted_total = g_format_size (total_delta_part_size);
          g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/s %s/%s",
                                  fetched_delta_parts, total_delta_parts,
                                  formatted_bytes_sec, formatted_bytes_transferred,
                                  formatted_total);
        }
      else if (outstanding_metadata_fetches)
        {
          g_string_append_printf (buf, "Receiving metadata objects: %u/(estimating) %s/s %s",
                                  metadata_fetched, formatted_bytes_sec, formatted_bytes_transferred);
        }
      else
        {
          g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                                  (guint)((((double)fetched) / requested) * 100),
                                  fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
        }
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  return g_string_free (buf, FALSE);
}


typedef struct
{
  GString *str_buffer;
  gchar *last_line;
  GSConsole *console;
  GError *error;
  GMainLoop *loop;

} ConsoleProgress;


static ConsoleProgress *
console_progress_new (void)
{
  ConsoleProgress *self = NULL;

  self = g_slice_new (ConsoleProgress);
  self->error = NULL;
  self->str_buffer = NULL;
  self->last_line = NULL;
  self->console = NULL;
  self->loop = g_main_loop_new (NULL, FALSE);

  return self;
}


static void
console_progress_free (ConsoleProgress *self)
{
  if (self->str_buffer != NULL)
    {
      g_string_free (self->str_buffer, TRUE);
      self->str_buffer = NULL;
    }

  g_free (self->last_line);

  g_main_loop_unref (self->loop);

  g_slice_free (ConsoleProgress, self);
}


static gboolean
end_status_line (ConsoleProgress *self)
{
  gboolean ret = TRUE;
  if (self->console != NULL)
    ret = gs_console_end_status_line (self->console, NULL, NULL);

  return ret;
}


static gboolean
add_status_line (ConsoleProgress *self,
                 const char *line)
{
  gboolean ret = TRUE;

  if (self->console != NULL)
    {
      ret = gs_console_begin_status_line (self->console, line, NULL, NULL);
    }
  else
    {
      if (self->last_line != NULL)
        {
          if (self->str_buffer == NULL)
              self->str_buffer = g_string_new ("");

          if (g_str_has_suffix (self->last_line, "\n"))
            g_string_append_printf (self->str_buffer, "%s", self->last_line);

          g_free (self->last_line);
          self->last_line = NULL;
        }

      self->last_line = g_strdup(line);
    }

  return ret;
}


static void
console_progress_end (ConsoleProgress *self)
{
  end_status_line (self);
  g_main_loop_quit (self->loop);
}


static void
on_update_completed (GDBusProxy *proxy,
                     gchar *sender_name,
                     gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
  ConsoleProgress *cp = user_data;
  if (g_strcmp0(signal_name, "UpdateCompleted") == 0)
    {
      gs_free gchar *message = NULL;
      gboolean success;

      g_variant_get_child (parameters, 1, "s", &message);
      g_variant_get_child (parameters, 0, "b", &success);

      if (success)
        {
          add_status_line(cp, message);
        }
      else
        {
          cp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                                       message);
        }
      console_progress_end(cp);
    }
}


static void
on_refspec_progress (GDBusProxy *proxy,
                     gchar *sender_name,
                     gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
  ConsoleProgress *cp = user_data;
  if (g_strcmp0(signal_name, "ProgressMessage") == 0)
    {
      gs_free gchar *message = NULL;


      g_variant_get_child (parameters, 0, "s", &message);
      add_status_line(cp, message);
    }
  else if (g_strcmp0(signal_name, "ProgressData") == 0)
    {
      gs_free gchar *line = NULL;

      guint64 start_time;
      guint64 elapsed_secs;
      guint outstanding_fetches;
      guint outstanding_writes;
      guint n_scanned_metadata;
      guint metadata_fetched;
      guint outstanding_metadata_fetches;
      guint total_delta_parts;
      guint fetched_delta_parts;
      guint total_delta_superblocks;
      guint64 total_delta_part_size;
      guint fetched;
      guint requested;
      guint64 bytes_transferred;
      guint64 bytes_sec;
      g_variant_get (parameters, "((tt)(uu)(uuu)(uuut)(uu)(tt))",
                     &start_time, &elapsed_secs,
                     &outstanding_fetches, &outstanding_writes,
                     &n_scanned_metadata, &metadata_fetched,
                     &outstanding_metadata_fetches,
                     &total_delta_parts, &fetched_delta_parts,
                     &total_delta_superblocks, &total_delta_part_size,
                     &fetched, &requested, &bytes_transferred, &bytes_sec);

      line = refspec_console_get_progress_line (start_time, elapsed_secs,
                                         outstanding_fetches,
                                         outstanding_writes,
                                         n_scanned_metadata,
                                         metadata_fetched,
                                         outstanding_metadata_fetches,
                                         total_delta_parts,
                                         fetched_delta_parts,
                                         total_delta_superblocks,
                                         total_delta_part_size,
                                         fetched,
                                         requested,
                                         bytes_transferred,
                                         bytes_sec);
      add_status_line(cp, line);
    }
}

static void
on_owner_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  // Owner shouldn't change durning update operations.
  // that messes with notifications, abort, abort.
  ConsoleProgress *cp = user_data;
  cp->error = g_dbus_error_new_for_dbus_error ("org.projectatomic.rpmostreed.Error.Failed",
                                               "Bus owner changed, aborting.");
  console_progress_end(cp);
}


static void
on_call_finished (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  ConsoleProgress *cp = user_data;
  gs_unref_variant GVariant *ret;

  ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &cp->error);

  if (cp->error)
    goto out;

  cp->console = gs_console_get ();
  if (cp->console)
    {
      gchar *old = NULL;
      if (cp->str_buffer != NULL)
        {
          old = g_string_free (cp->str_buffer, FALSE);
          gs_console_begin_status_line (cp->console, old, NULL, NULL);
          cp->str_buffer = NULL;
        }

      if (cp->last_line != NULL)
        {
          gs_console_begin_status_line (cp->console, cp->last_line, NULL, NULL);
          g_free (cp->last_line);
          cp->last_line = NULL;
        }
      else
        {
          gs_console_begin_status_line (cp->console, "", NULL, NULL);
        }

      g_free(old);
    }

out:
  if (cp->error)
    console_progress_end (cp);
}


static void
cancelled_handler (GCancellable *cancellable,
                   gpointer user_data)
{
  RPMOSTreeSysroot *sysroot = user_data;
  rpmostree_sysroot_call_cancel_update_sync (sysroot, NULL, NULL);
}


static gboolean
dbus_call_sync_on_signal (RPMOSTreeSysroot *sysroot,
                          GDBusProxy *proxy,
                          const gchar *method,
                          GVariant *parameters,
                          GCallback properties_callback,
                          GCallback obj_sig_callback,
                          GCancellable *cancellable,
                          GError **error)
{
  gint id;
  gulong signal_handler = 0;
  gulong property_handler = 0;
  gulong obj_sig_handler = 0;
  gboolean success = FALSE;
  GDBusObjectManager *manager = NULL;
  GDBusConnection *connection = NULL;
  ConsoleProgress *cp = console_progress_new ();

  if (parameters == NULL)
    parameters = g_variant_new ("()", NULL);

  // Setup manager connection
  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (sysroot));
  manager = rpmostree_object_manager_client_new_sync (connection,
                                                      G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                      BUS_NAME,
                                                      "/org/projectatomic/rpmostree1",
                                                      cancellable,
                                                      error);

  if (manager != NULL)
    {
      g_signal_connect (manager,
                        "notify::name-owner",
                        G_CALLBACK (on_owner_changed),
                        cp);
    }
  else
    {
      goto out;
    }

  // setup cancel handler
  id = 0;
  if (cancellable)
    {
      id = g_cancellable_connect (cancellable,
                                  G_CALLBACK (cancelled_handler),
                                  sysroot, NULL);
    }

  // Setup finished signal handlers
  signal_handler = g_signal_connect (sysroot, "g-signal",
                                     G_CALLBACK (on_update_completed),
                                     cp);
  if (properties_callback != NULL)
    {
      property_handler = g_signal_connect (proxy, "g-properties-changed",
                                           properties_callback,
                                           cp);
    }

  if (obj_sig_callback != NULL)
    {
      obj_sig_handler = g_signal_connect (proxy, "g-signal",
                                          obj_sig_callback,
                                          cp);
    }

  // Call that kicks everything off.
  g_dbus_proxy_call (proxy,
                     method,
                     parameters,
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     on_call_finished,
                     cp);

  g_main_loop_run (cp->loop);

  g_cancellable_disconnect (cancellable, id);

  if (!g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      if (cp->error)
        {
          g_propagate_error (error, cp->error);
        }
      else
        {
          success = TRUE;
        }
    }

out:
  if (signal_handler)
    g_signal_handler_disconnect (sysroot, signal_handler);

  if (property_handler)
    g_signal_handler_disconnect (proxy, property_handler);

  if (obj_sig_handler)
    g_signal_handler_disconnect (proxy, obj_sig_handler);

  console_progress_free (cp);
  return success;
}


gboolean
rpmostree_refspec_update_sync (RPMOSTreeSysroot *sysroot,
                               RPMOSTreeRefSpec *refspec,
                               const gchar *method,
                               GVariant *parameters,
                               GCancellable *cancellable,
                               GError **error)
{
  return dbus_call_sync_on_signal (sysroot, G_DBUS_PROXY (refspec), method, parameters,
                                   NULL, G_CALLBACK (on_refspec_progress),
                                   cancellable, error);
}

gboolean
rpmostree_deployment_deploy_sync (RPMOSTreeSysroot *sysroot,
                                  RPMOSTreeDeployment *deployment,
                                  GCancellable *cancellable,
                                  GError **error)
{
  return dbus_call_sync_on_signal (sysroot, G_DBUS_PROXY (deployment), "MakeDefault",
                                   NULL, NULL, NULL, cancellable, error);
}

//XXX: this is variant processing part of
// ostree_gpg_verify_result_describe
// it really should be in libostree
// but just copying for now till that
// can be refactored.
static void
_gpg_verify_variant_describe (GVariant *variant,
                              GString *output_buffer,
                              const gchar *line_prefix)
{
  g_autoptr(GDateTime) date_time_utc = NULL;
  g_autoptr(GDateTime) date_time_local = NULL;
  g_autofree char *formatted_date_time = NULL;
  gint64 timestamp;
  gint64 exp_timestamp;
  const char *fingerprint;
  const char *pubkey_algo;
  const char *user_name;
  const char *user_email;
  const char *key_id;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_missing;
  gsize len;

  /* The default format roughly mimics the verify output generated by
   * check_sig_and_print() in gnupg/g10/mainproc.c, though obviously
   * greatly simplified. */

  g_return_if_fail (variant != NULL);

  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_VALID,
                       "b", &valid);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
                       "b", &sig_expired);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
                       "b", &key_missing);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT,
                       "&s", &fingerprint);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP,
                       "x", &timestamp);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP,
                       "x", &exp_timestamp);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME,
                       "&s", &pubkey_algo);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_USER_NAME,
                       "&s", &user_name);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL,
                       "&s", &user_email);

  len = strlen (fingerprint);
  key_id = (len > 16) ? fingerprint + len - 16 : fingerprint;

  date_time_utc = g_date_time_new_from_unix_utc (timestamp);
  date_time_local = g_date_time_to_local (date_time_utc);
  formatted_date_time = g_date_time_format (date_time_local, "%c");

  if (line_prefix != NULL)
    g_string_append (output_buffer, line_prefix);

  g_string_append_printf (output_buffer,
                          "Signature made %s using %s key ID %s\n",
                          formatted_date_time, pubkey_algo, key_id);

  g_clear_pointer (&date_time_utc, g_date_time_unref);
  g_clear_pointer (&date_time_local, g_date_time_unref);
  g_clear_pointer (&formatted_date_time, g_free);

  if (line_prefix != NULL)
    g_string_append (output_buffer, line_prefix);

  if (key_missing)
    {
      g_string_append (output_buffer,
                       "Can't check signature: public key not found\n");
    }
  else if (valid)
    {
      g_string_append_printf (output_buffer,
                              "Good signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }
  else if (sig_expired)
    {
      g_string_append_printf (output_buffer,
                              "Expired signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }
  else
    {
      g_string_append_printf (output_buffer,
                              "BAD signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }

  if (exp_timestamp > 0)
    {
      date_time_utc = g_date_time_new_from_unix_utc (exp_timestamp);
      date_time_local = g_date_time_to_local (date_time_utc);
      formatted_date_time = g_date_time_format (date_time_local, "%c");

      if (line_prefix != NULL)
        g_string_append (output_buffer, line_prefix);

      if (sig_expired)
        {
          g_string_append_printf (output_buffer,
                                  "Signature expired %s\n",
                                  formatted_date_time);
        }
      else
        {
          g_string_append_printf (output_buffer,
                                  "Signature expires %s\n",
                                  formatted_date_time);
        }
    }
}


void
rpmostree_print_signatures (GVariant *variant,
                            const gchar *sep)
{
  GString *sigs_buffer;
  guint i;
  guint n_sigs = g_variant_n_children (variant);
  sigs_buffer = g_string_sized_new (256);

  for (i = 0; i < n_sigs; i++)
    {
      gs_unref_variant GVariant *v = NULL;
      g_string_append_c (sigs_buffer, '\n');
      g_variant_get_child(variant, i, "v", &v);
      _gpg_verify_variant_describe (v, sigs_buffer, sep);
    }

  g_print ("%s", sigs_buffer->str);
  g_string_free (sigs_buffer, TRUE);
}
