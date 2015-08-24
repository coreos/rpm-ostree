/*
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "rpmostreed-utils.h"
#include "rpmostreed-errors.h"

#include <libglnx.h>

static void
append_to_object_path (GString *str,
                       const gchar *s)
{
  guint n;

  for (n = 0; s[n] != '\0'; n++)
    {
      gint c = s[n];
      /* D-Bus spec sez:
       *
       * Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_-"
       */
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
        {
          g_string_append_c (str, c);
        }
      else if (c == '-' || c == '/')
        {
          /* Swap / or - for _ to keep names easier to read */
          g_string_append_c (str, '_');
        }
      else
        {
          /* Escape bytes not in [A-Z][a-z][0-9] as _<hex-with-two-digits> */
          g_string_append_printf (str, "_%02x", c & 0xFF);
        }
    }
}

/**
 * rpmostreed_generate_object_path:
 * @base: The base object path (without trailing '/').
 * @part...: UTF-8 strings.
 *
 * Appends @s to @base in a way such that only characters that can be
 * used in a D-Bus object path will be used. E.g. a character not in
 * <literal>[A-Z][a-z][0-9]_</literal> will be escaped as _HEX where
 * HEX is a two-digit hexadecimal number.
 *
 * Note that his mapping is not bijective - e.g. you cannot go back
 * to the original string.
 *
 * Returns: An allocated string that must be freed with g_free ().
 */
gchar *
rpmostreed_generate_object_path (const gchar *base,
                                 const gchar *part,
                                 ...)
{
  gchar *result;
  va_list va;

  va_start (va, part);
  result = rpmostreed_generate_object_path_from_va (base, part, va);
  va_end (va);

  return result;
}

gchar *
rpmostreed_generate_object_path_from_va (const gchar *base,
                                         const gchar *part,
                                         va_list va)
{
  GString *path;

  g_return_val_if_fail (base != NULL, NULL);
  g_return_val_if_fail (g_variant_is_object_path (base), NULL);
  g_return_val_if_fail (!g_str_has_suffix (base, "/"), NULL);

  path = g_string_new (base);

  while (part != NULL)
    {
      if (!g_utf8_validate (part, -1, NULL))
        {
          g_string_free (path, TRUE);
          return NULL;
        }
      else
        {
          g_string_append_c (path, '/');
          append_to_object_path (path, part);
          part = va_arg (va, const gchar *);
        }
    }

  return g_string_free (path, FALSE);
}

/**
 * rpmostreed_load_sysroot_and_repo:
 * @path: The path to the sysroot.
 * @cancellable: Cancelable
 * @out_sysroot (out): The OstreeSysroot at the given path
 * @out_repo (out): The OstreeRepo for the sysroot
 * @error (out): Error

 * Returns: True on success.
 */
gboolean
rpmostreed_load_sysroot_and_repo (const gchar *path,
                                  GCancellable *cancellable,
                                  OstreeSysroot **out_sysroot,
                                  OstreeRepo **out_repo,
                                  GError **error)
{
  glnx_unref_object GFile *sysroot_path = NULL;
  glnx_unref_object OstreeSysroot *ot_sysroot = NULL;
  gboolean ret = FALSE;

  sysroot_path = g_file_new_for_path (path);
  ot_sysroot = ostree_sysroot_new (sysroot_path);

  if (!ostree_sysroot_load (ot_sysroot,
                            cancellable,
                            error))
      goto out;

  /* ostree_sysroot_get_repo now just adds a
   * ref to its singleton */
  if (!ostree_sysroot_get_repo (ot_sysroot,
                                out_repo,
                                cancellable,
                                error))
      goto out;

  if (out_sysroot != NULL)
    *out_sysroot = g_steal_pointer (&ot_sysroot);

  ret = TRUE;

out:
  return ret;
}

/**
 * rpmostreed_refspec_parse_partial:
 * @new_provided_refspec: The provided refspec
 * @base_refspec: The refspec string to base on.
 * @out_refspec: Pointer to the new refspec
 * @error: Pointer to an error pointer.
 *
 * Takes a refspec string and adds any missing bits based on the
 * base_refspec argument. Errors if a full valid refspec can't
 * be derived.
 *
 * Returns: True on success.
 */
gboolean
rpmostreed_refspec_parse_partial (const gchar *new_provided_refspec,
                                  gchar *base_refspec,
                                  gchar **out_refspec,
                                  GError **error)
{

  g_autofree gchar *ref = NULL;
  g_autofree gchar *remote = NULL;
  g_autofree gchar *origin_ref = NULL;
  g_autofree gchar *origin_remote = NULL;
  GError *parse_error = NULL;

  gboolean ret = FALSE;

  /* Allow just switching remotes */
  if (g_str_has_suffix (new_provided_refspec, ":"))
    {
      remote = g_strdup (new_provided_refspec);
      remote[strlen (remote) - 1] = '\0';
    }
  else
    {
      if (!ostree_parse_refspec (new_provided_refspec, &remote,
                                 &ref, &parse_error))
        {
          g_set_error_literal (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                       parse_error->message);
          g_clear_error (&parse_error);
          goto out;
        }
    }

  if (base_refspec != NULL)
    {
      if (!ostree_parse_refspec (base_refspec, &origin_remote,
                               &origin_ref, &parse_error))
        goto out;
    }

  if (ref == NULL)
    {
      if (origin_ref)
        {
          ref = g_strdup (origin_ref);
        }

      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                      "Could not determine default ref to pull.");
          goto out;
        }

    }
  else if (remote == NULL)
    {
      if (origin_remote)
        {
          remote = g_strdup (origin_remote);
        }
      else
        {
          g_set_error (error, RPM_OSTREED_ERROR,
                       RPM_OSTREED_ERROR_INVALID_REFSPEC,
                       "Could not determine default remote to pull.");
          goto out;
        }
    }

  if (g_strcmp0 (origin_remote, remote) == 0 &&
      g_strcmp0 (origin_ref, ref) == 0)
    {
      g_set_error (error, RPM_OSTREED_ERROR,
                   RPM_OSTREED_ERROR_INVALID_REFSPEC,
                   "Old and new refs are equal: %s:%s",
                   remote, ref);
      goto out;
    }

  *out_refspec = g_strconcat (remote, ":", ref, NULL);
  ret = TRUE;

out:
  return ret;
}
