/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

#include <gio/gio.h>
#include <systemd/sd-journal.h>
#include "rpmostree-output.h"
#include "rpmostree-bwrap.h"
#include <err.h>
#include <systemd/sd-journal.h>
#include "libglnx.h"

#include "rpmostree-scripts.h"

#define RPMOSTREE_MESSAGE_PREPOST SD_ID128_MAKE(42,d3,72,22,dc,a2,4a,3b,9d,30,ce,d4,bb,bc,ac,d2)
#define RPMOSTREE_MESSAGE_FILETRIGGER SD_ID128_MAKE(ef,dd,0e,4e,79,ca,45,d3,88,76,ac,45,e1,28,23,68)

/* This bit is currently private in librpm */
enum rpmscriptFlags_e {
  RPMSCRIPT_FLAG_NONE		= 0,
  RPMSCRIPT_FLAG_EXPAND	= (1 << 0), /* macro expansion */
  RPMSCRIPT_FLAG_QFORMAT	= (1 << 1), /* header queryformat expansion */
};

typedef struct {
    const char *desc;
    rpmsenseFlags sense;
    rpmTagVal tag;
    rpmTagVal progtag;
    rpmTagVal flagtag;
} KnownRpmScriptKind;

#if 0
static const KnownRpmScriptKind ignored_scripts[] = {
  /* Ignore all of the *un variants since we never uninstall
   * anything in the RPM sense.
   */
  { "%preun", 0,
    RPMTAG_PREUN, RPMTAG_PREUNPROG, RPMTAG_PREUNFLAGS },
  { "%postun", 0,
    RPMTAG_POSTUN, RPMTAG_POSTUNPROG, RPMTAG_POSTUNFLAGS },
  { "%triggerun", RPMSENSE_TRIGGERUN,
    RPMTAG_TRIGGERUN, 0, 0 },
  { "%triggerpostun", RPMSENSE_TRIGGERPOSTUN,
    RPMTAG_TRIGGERPOSTUN, 0, 0 },

  /* In practice, %pretrans are hack-arounds for broken upgrades.
   * Again, since we always assemble a new root, there's no point
   * to running them.
   * http://lists.rpm.org/pipermail/rpm-ecosystem/2016-August/000391.html
   */
  { "%pretrans", 0,
    RPMTAG_PRETRANS, RPMTAG_PRETRANSPROG, RPMTAG_PRETRANSFLAGS },
};
#endif

static const KnownRpmScriptKind pre_scripts[] = {
  { "%prein", 0,
    RPMTAG_PREIN, RPMTAG_PREINPROG, RPMTAG_PREINFLAGS },
};

static const KnownRpmScriptKind posttrans_scripts[] = {
  /* For now, we treat %post as equivalent to %posttrans */
  { "%post", 0,
    RPMTAG_POSTIN, RPMTAG_POSTINPROG, RPMTAG_POSTINFLAGS },
  { "%posttrans", 0,
    RPMTAG_POSTTRANS, RPMTAG_POSTTRANSPROG, RPMTAG_POSTTRANSFLAGS },
};

static const KnownRpmScriptKind unsupported_scripts[] = {
  { "%triggerprein", RPMSENSE_TRIGGERPREIN,
    RPMTAG_TRIGGERPREIN, 0, 0 },
  { "%triggerin", RPMSENSE_TRIGGERIN,
    RPMTAG_TRIGGERIN, 0, 0 },
  { "%verify", 0,
    RPMTAG_VERIFYSCRIPT, RPMTAG_VERIFYSCRIPTPROG, RPMTAG_VERIFYSCRIPTFLAGS},
};

static gboolean
fail_if_interp_is_lua (const char *pkg_name,
                       const char *interp,
                       const char *script_desc,
                       GError    **error)
{
  static const char lua_builtin[] = "<lua>";
  if (g_strcmp0 (interp, lua_builtin) == 0)
    return glnx_throw (error, "Package '%s' has (currently) unsupported %s script in '%s'",
                       pkg_name, lua_builtin, script_desc);

  return TRUE;
}

static RpmOstreeScriptAction
lookup_script_action (const char *pkg_name,
                      const char *scriptdesc)
{
  const char *pkg_script = glnx_strjoina (pkg_name, ".", scriptdesc+1);
  const struct RpmOstreePackageScriptHandler *handler = rpmostree_script_gperf_lookup (pkg_script, strlen (pkg_script));
  if (!handler)
    return RPMOSTREE_SCRIPT_ACTION_DEFAULT;
  return handler->action;
}

gboolean
rpmostree_script_txn_validate (DnfPackage    *package,
                               Header         hdr,
                               GCancellable  *cancellable,
                               GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (unsupported_scripts); i++)
    {
      const char *desc = unsupported_scripts[i].desc;
      rpmTagVal tagval = unsupported_scripts[i].tag;
      rpmTagVal progtagval = unsupported_scripts[i].progtag;

      if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
        continue;

      RpmOstreeScriptAction action = lookup_script_action (dnf_package_get_name (package), desc);
      switch (action)
        {
        case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
          return glnx_throw (error, "Package '%s' has (currently) unsupported script of type '%s'",
                             dnf_package_get_name (package), desc);
        case RPMOSTREE_SCRIPT_ACTION_IGNORE:
          continue;
        }
    }

  return TRUE;
}

static void
script_child_setup_stdin (gpointer data)
{
  int fd = GPOINTER_TO_INT (data);

  if (dup2 (fd, 0) < 0)
    err (1, "dup2");
}

/* Lowest level script handler in this file; create a bwrap instance and run it
 * synchronously.
 */
static gboolean
run_script_in_bwrap_container (int rootfs_fd,
                               const char *name,
                               const char *scriptdesc,
                               const char *interp,
                               const char *script,
                               const char *script_arg,
                               int         stdin_fd,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  const char *pkg_script = glnx_strjoina (name, ".", scriptdesc+1);
  const char *postscript_name = glnx_strjoina ("/", pkg_script);
  const char *postscript_path_container = glnx_strjoina ("/usr", postscript_name);
  const char *postscript_path_host = postscript_path_container + 1;
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;
  gboolean created_var_tmp = FALSE;

  /* TODO - Create a pipe and send this to bwrap so it's inside the
   * tmpfs.  Note the +1 on the path to skip the leading /.
   */
  if (!glnx_file_replace_contents_at (rootfs_fd, postscript_path_host,
                                      (guint8*)script, -1,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      NULL, error))
    {
      g_prefix_error (error, "Writing script to %s: ", postscript_path_host);
      goto out;
    }

  /* We need to make the mount point in the case where we're doing
   * package layering, since the host `/var` tree is empty.  We
   * *could* point at the real `/var`...but that seems
   * unnecessary/dangerous to me.  Daemons that need to perform data
   * migrations should do them as part of their systemd units and not
   * in %post.
   *
   * Another alternative would be to make a tmpfs with the compat
   * symlinks.
   */
  if (mkdirat (rootfs_fd, "var/tmp", 0755) < 0)
    {
      if (errno == EEXIST)
        ;
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    created_var_tmp = TRUE;

  /* ⚠⚠⚠ If you change this, also update scripts/bwrap-script-shell.sh ⚠⚠⚠ */

  /* We just did a ro bind mount over /var above. However we want a writable
   * var/tmp, so we need to tmpfs mount on top of it. See also
   * https://github.com/projectatomic/bubblewrap/issues/182
   */
  bwrap = rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_MUTATE_ROFILES, error,
                               /* Scripts can see a /var with compat links like alternatives */
                               "--ro-bind", "./var", "/var",
                               "--tmpfs", "/var/tmp",
                               /* Allow RPM scripts to change the /etc defaults; note we use bind
                                * to ensure symlinks work, see https://github.com/projectatomic/rpm-ostree/pull/640 */
                               "--bind", "./usr/etc", "/etc",
                               NULL);
  if (!bwrap)
    goto out;

  if (stdin_fd >= 0)
    rpmostree_bwrap_set_child_setup (bwrap, script_child_setup_stdin, GINT_TO_POINTER (stdin_fd));

  rpmostree_bwrap_append_child_argv (bwrap,
                                     interp,
                                     postscript_path_container,
                                     script_arg,
                                     NULL);

  if (!rpmostree_bwrap_run (bwrap, error))
    goto out;

  ret = TRUE;
 out:
  (void) unlinkat (rootfs_fd, postscript_path_host, 0);
  if (created_var_tmp)
    (void) unlinkat (rootfs_fd, "var/tmp", AT_REMOVEDIR);
  return ret;
}

/* Medium level script entrypoint; we already validated it exists and isn't
 * ignored. Here we mostly compute arguments/input, then proceed into the lower
 * level bwrap execution.
 */
static gboolean
impl_run_rpm_script (const KnownRpmScriptKind *rpmscript,
                     DnfPackage    *pkg,
                     Header         hdr,
                     int            rootfs_fd,
                     GCancellable  *cancellable,
                     GError       **error)
{
  g_autofree char *script_owned = NULL;
  const char *script = headerGetString (hdr, rpmscript->tag);
  g_assert (script);
  const rpmFlags flags = headerGetNumber (hdr, rpmscript->flagtag);
  if (flags & RPMSCRIPT_FLAG_EXPAND)
    script = script_owned = rpmExpand (script, NULL);

  struct rpmtd_s td;
  g_autofree char **args = NULL;
  if (headerGet (hdr, rpmscript->progtag, &td, (HEADERGET_ALLOC|HEADERGET_ARGV)))
    args = td.data;

  const char *interp = (args && args[0]) ? args[0] : "/bin/sh";
  /* Check for lua; see also https://github.com/projectatomic/rpm-ostree/issues/749 */
  if (!fail_if_interp_is_lua (interp, dnf_package_get_name (pkg), rpmscript->desc, error))
    return FALSE;
  /* http://ftp.rpm.org/max-rpm/s1-rpm-inside-scripts.html#S2-RPM-INSIDE-ERASE-TIME-SCRIPTS */
  const char *script_arg = NULL;
  switch (dnf_package_get_action (pkg))
    {
      /* XXX: we're not running *un scripts for removals yet, though it'd look like:
         case DNF_STATE_ACTION_REMOVE:
         case DNF_STATE_ACTION_OBSOLETE:
         script_arg = obsoleted_by_update ? "1" : "0";
         break;
      */
    case DNF_STATE_ACTION_INSTALL:
      script_arg = "1";
      break;
    case DNF_STATE_ACTION_UPDATE:
      script_arg = "2";
      break;
    case DNF_STATE_ACTION_DOWNGRADE:
      script_arg = "2";
      break;
    default:
      /* we shouldn't have been asked to perform for any other kind of action */
      g_assert_not_reached ();
      break;
    }

  guint64 start_time_ms = g_get_monotonic_time () / 1000;
  if (!run_script_in_bwrap_container (rootfs_fd, dnf_package_get_name (pkg),
                                      rpmscript->desc, interp, script, script_arg,
                                      -1, cancellable, error))
    return glnx_prefix_error (error, "Running %s for %s", rpmscript->desc, dnf_package_get_name (pkg));
  guint64 end_time_ms = g_get_monotonic_time () / 1000;
  guint64 elapsed_ms = end_time_ms - start_time_ms;

  sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_PREPOST),
                   "MESSAGE=Executed %s for %s in %ums", rpmscript->desc, dnf_package_get_name (pkg), elapsed_ms,
                   "SCRIPT_TYPE=%s", rpmscript->desc,
                   "PKG=%s", dnf_package_get_name (pkg),
                   "EXEC_TIME_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                   NULL);

  return TRUE;
}

/* High level script entrypoint; check a package to see whether a script exists,
 * execute it if it exists (and it's not ignored).
 */
static gboolean
run_script (const KnownRpmScriptKind *rpmscript,
            DnfPackage               *pkg,
            Header                    hdr,
            int                       rootfs_fd,
            gboolean                 *out_did_run,
            GCancellable             *cancellable,
            GError                  **error)
{
  rpmTagVal tagval = rpmscript->tag;
  rpmTagVal progtagval = rpmscript->progtag;

  *out_did_run = FALSE;

  if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
    return TRUE;

  const char *script = headerGetString (hdr, tagval);
  if (!script)
    return TRUE;

  const char *desc = rpmscript->desc;
  RpmOstreeScriptAction action = lookup_script_action (dnf_package_get_name (pkg), desc);
  switch (action)
    {
    case RPMOSTREE_SCRIPT_ACTION_IGNORE:
      return TRUE; /* Note early return */
    case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
      break; /* Continue below */
    }

  *out_did_run = TRUE;
  return impl_run_rpm_script (rpmscript, pkg, hdr, rootfs_fd,
                              cancellable, error);
}

#ifdef BUILDOPT_HAVE_RPM_FILETRIGGERS
static gboolean
write_filename (FILE *f, GString *prefix,
                GError **error)
{
  if (fwrite_unlocked (prefix->str, 1, prefix->len, f) != prefix->len)
    return glnx_throw_errno_prefix (error, "fwrite");
  if (fputc_unlocked ('\n', f) == EOF)
    return glnx_throw_errno_prefix (error, "fputc");
  return TRUE;
}

/* Used for %transfiletriggerin - basically an implementation of `find -type f` that
 * writes the filenames to a tmpfile.
 */
static gboolean
write_subdir (int dfd, const char *path,
              GString *prefix,
              FILE *f,
              guint *inout_n_matched,
              GCancellable *cancellable,
              GError **error)
{
  /* This is an inlined copy of ot_dfd_iter_init_allow_noent()...if more users
   * appear we should probably push to libglnx.
   */
  g_assert (*path != '/');
  glnx_fd_close int target_dfd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (target_dfd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendirat");
      /* Not early return */
      return TRUE;
    }
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_take_fd (&target_dfd, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      const size_t origlen = prefix->len;
      g_string_append_c (prefix, '/');
      g_string_append (prefix, dent->d_name);
      if (dent->d_type == DT_DIR)
        {
          if (!write_subdir (dfd_iter.fd, dent->d_name, prefix, f,
                             inout_n_matched,
                             cancellable, error))
            return FALSE;
        }
      else
        {
          if (!write_filename (f, prefix, error))
            return FALSE;
          (*inout_n_matched)++;
        }
      g_string_truncate (prefix, origlen);
    }

  return TRUE;
}

/* Given file trigger @pattern (really a subdirectory), traverse the
 * filesystem @rootfs_fd and write all matches as file names to @f.  Used
 * for %transfiletriggerin.
 */
static gboolean
find_and_write_matching_files (int rootfs_fd, const char *pattern,
                               FILE *f,
                               guint *out_n_matches,
                               GCancellable *cancellable,
                               GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finding matches", error);

  /* Fontconfig in fedora has /usr/local; we don't support RPM
   * touching /usr/local.  While I'm here, proactively
   * require /usr as a prefix too.
   */
  if (g_str_has_prefix (pattern, "usr/local") ||
      !g_str_has_prefix (pattern, "usr/"))
    return TRUE;

  /* The printed buffer does have a leading / */
  g_autoptr(GString) buf = g_string_new ("/");
  g_string_append (buf, pattern);
  /* Strip trailing '/' in the mutable copy we have here */
  while (buf->len > 0 && buf->str[buf->len-1] == '/')
    g_string_truncate (buf, buf->len - 1);

  guint n_pattern_matches = 0;
  if (!write_subdir (rootfs_fd, pattern, buf, f, &n_pattern_matches,
                     cancellable, error))
    return glnx_prefix_error (error, "pattern '%s'", pattern);
  *out_n_matches += n_pattern_matches;

  return TRUE;
}
#endif

/* Execute all post/post-transaction scripts for @pkg */
gboolean
rpmostree_posttrans_run_sync (DnfPackage    *pkg,
                              Header         hdr,
                              int            rootfs_fd,
                              guint         *out_n_run,
                              GCancellable  *cancellable,
                              GError       **error)
{
  /* We treat %post and %posttrans equivalently, so do those in one go */
  for (guint i = 0; i < G_N_ELEMENTS (posttrans_scripts); i++)
    {
      gboolean did_run = FALSE;

      if (!run_script (&posttrans_scripts[i], pkg, hdr, rootfs_fd,
                       &did_run, cancellable, error))
        return FALSE;

      if (did_run)
        (*out_n_run)++;
    }
  return TRUE;
}

/* File triggers, as used by e.g. glib2.spec and vagrant.spec in Fedora. More
 * info at <http://rpm.org/user_doc/file_triggers.html>.
 */
gboolean
rpmostree_transfiletriggers_run_sync (Header        hdr,
                                      int           rootfs_fd,
                                      guint        *out_n_run,
                                      GCancellable *cancellable,
                                      GError      **error)
{
#ifdef BUILDOPT_HAVE_RPM_FILETRIGGERS
  const char *pkg_name = headerGetString (hdr, RPMTAG_NAME);
  g_assert (pkg_name);

  g_autofree char *error_prefix = g_strconcat ("Executing %transfiletriggerin for ", pkg_name, NULL);
  GLNX_AUTO_PREFIX_ERROR (error_prefix, error);

  RpmOstreeScriptAction action = lookup_script_action (pkg_name, "%transfiletriggerin");
  switch (action)
    {
    case RPMOSTREE_SCRIPT_ACTION_IGNORE:
      return TRUE; /* Note early return */
    case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
      break; /* Continue below */
    }

  headerGetFlags hgflags = HEADERGET_MINMEM;
  struct rpmtd_s tname, tscripts, tprogs, tflags, tscriptflags;
  struct rpmtd_s tindex;
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERNAME, &tname, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTS, &tscripts, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTPROG, &tprogs, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERFLAGS, &tflags, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERSCRIPTFLAGS, &tscriptflags, hgflags);
  headerGet (hdr, RPMTAG_TRANSFILETRIGGERINDEX, &tindex, hgflags);
  g_debug ("pkg %s transtrigger count %u/%u/%u/%u/%u/%u\n",
           pkg_name, rpmtdCount (&tname), rpmtdCount (&tscripts),
           rpmtdCount (&tprogs), rpmtdCount (&tflags), rpmtdCount (&tscriptflags),
           rpmtdCount (&tindex));

  if (rpmtdCount (&tscripts) == 0)
    return TRUE;

  const guint n_scripts = rpmtdCount (&tscripts);
  const guint n_names = rpmtdCount (&tname);
  /* Given multiple matching patterns, RPM expands it into multiple copies.
   * The trigger index (AIUI) defines where to find the pattern (and flags) given a
   * script.
   *
   * Some librpm source references:
   *  - tagexts.c:triggercondsTagFor()
   *  - rpmscript.c:rpmScriptFromTriggerTag()
   */
  for (guint i = 0; i < n_scripts; i++)
    {
      rpmtdInit (&tname);
      rpmtdInit (&tflags);

      rpmFlags flags = 0;
      if (rpmtdSetIndex (&tscriptflags, i) >= 0)
        flags = rpmtdGetNumber (&tscriptflags);

      g_assert_cmpint (rpmtdSetIndex (&tprogs, i), ==, i);
      const char *interp = rpmtdGetString (&tprogs);
      if (!interp)
        interp = "/bin/sh";
      if (!fail_if_interp_is_lua (interp, pkg_name, "%transfiletriggerin", error))
        return FALSE;

      g_autofree char *script_owned = NULL;
      g_assert_cmpint (rpmtdSetIndex (&tscripts, i), ==, i);
      const char *script = rpmtdGetString (&tscripts);
      if (!script)
        continue;
      if (flags & RPMSCRIPT_FLAG_EXPAND)
        script = script_owned = rpmExpand (script, NULL);

      g_autoptr(GPtrArray) patterns = g_ptr_array_new_with_free_func (g_free);

      /* Iterate over the trigger "names" which are file patterns */
      for (guint j = 0; j < n_names; j++)
        {
          g_assert_cmpint (rpmtdSetIndex (&tindex, j), ==, j);
          guint32 tindex_num = *rpmtdGetUint32 (&tindex);

          if (tindex_num != i)
            continue;

          rpmFlags sense = 0;
          if (rpmtdSetIndex (&tflags, j) >= 0)
            sense = rpmtdGetNumber (&tflags);
          /* See if this is a triggerin (as opposed to triggerun, which we)
           * don't execute.
           */
          const gboolean trigger_in = (sense & RPMSENSE_TRIGGERIN) > 0;
          if (!trigger_in)
            continue;

          g_assert_cmpint (rpmtdSetIndex (&tname, j), ==, j);
          const char *pattern = rpmtdGetString (&tname);
          if (!pattern)
            continue;
          /* Skip leading `/`, since we use fd-relative access */
          pattern += strspn (pattern, "/");
          /* Silently ignore broken patterns for now */
          if (!*pattern)
            continue;

          g_ptr_array_add (patterns, g_strdup (pattern));
        }

      if (patterns->len == 0)
        continue;

      /* Build up the list of files matching the patterns. librpm uses a pipe and
       * doesn't do async writes, and hence is subject to deadlock. We could use
       * a pipe and do async, but an O_TMPFILE is easier for now. There
       * shouldn't be megabytes of data here, and the parallelism loss in
       * practice I think is going to be small.
       */
      g_auto(GLnxTmpfile) matching_files_tmpf = { 0, };
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &matching_files_tmpf, error))
        return FALSE;
      g_autoptr(FILE) tmpf_file = fdopen (matching_files_tmpf.fd, "w");
      if (!tmpf_file)
        return glnx_throw_errno_prefix (error, "fdopen");

      g_autoptr(GString) patterns_joined = g_string_new ("");
      guint n_total_matched = 0;
      for (guint j = 0; j < patterns->len; j++)
        {
          guint n_matched = 0;
          const char *pattern = patterns->pdata[j];
          if (j > 0)
            g_string_append (patterns_joined, ", ");
          g_string_append (patterns_joined, pattern);
          if (!find_and_write_matching_files (rootfs_fd, pattern, tmpf_file, &n_matched,
                                              cancellable, error))
            return FALSE;
          if (n_matched == 0)
            {
              /* This is probably a bug...let's log it */
              sd_journal_print (LOG_INFO, "No files matched %%transfiletriggerin(%s) for %s", pattern, pkg_name);
            }
          n_total_matched += n_matched;
        }

      if (n_total_matched == 0)
        continue;

      if (!glnx_stdio_file_flush (tmpf_file, error))
        return FALSE;
      /* Now, point back to the beginning so the script reads it from the start
         as stdin */
      if (lseek (matching_files_tmpf.fd, 0, SEEK_SET) < 0)
        return glnx_throw_errno_prefix (error, "lseek");

      /* Run it, and log the result */
      guint64 start_time_ms = g_get_monotonic_time () / 1000;
      if (!run_script_in_bwrap_container (rootfs_fd, pkg_name,
                                          "%transfiletriggerin", interp, script, NULL,
                                          matching_files_tmpf.fd, cancellable, error))
        return FALSE;
      guint64 end_time_ms = g_get_monotonic_time () / 1000;
      guint64 elapsed_ms = end_time_ms - start_time_ms;

      (*out_n_run)++;

      sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(RPMOSTREE_MESSAGE_FILETRIGGER),
                       "MESSAGE=Executed %%transfiletriggerin(%s) for %s in %ums; %u matched files",
                       pkg_name, patterns_joined->str, elapsed_ms, n_total_matched,
                       "SCRIPT_TYPE=%%transfiletriggerin"
                       "PKG=%s", pkg_name,
                       "PATTERNS=%s", patterns_joined->str,
                       "TRIGGER_N_MATCHES=%u", n_total_matched,
                       "EXEC_TIME_MS=%" G_GUINT64_FORMAT, elapsed_ms,
                       NULL);
    }
#endif
  return TRUE;
}

/* Execute all pre-install scripts for @pkg */
gboolean
rpmostree_pre_run_sync (DnfPackage    *pkg,
                        Header         hdr,
                        int            rootfs_fd,
                        guint         *out_n_run,
                        GCancellable  *cancellable,
                        GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (pre_scripts); i++)
    {
      gboolean did_run = FALSE;

      if (!run_script (&pre_scripts[i], pkg, hdr, rootfs_fd,
                       &did_run, cancellable, error))
        return FALSE;

      if (did_run)
        (*out_n_run)++;
    }

  return TRUE;
}

/* Ensure that we can at least execute /usr/bin/true inside the new root.
 * See https://github.com/projectatomic/rpm-ostree/pull/888
 *
 * Currently at least on Fedora this will run through e.g. the dynamic linker
 * and hence some bits of glibc.
 *
 * We could consider doing more here, perhaps even starting systemd in a
 * volatile mode, but that could just as easily be a separate tool.
 */
gboolean
rpmostree_deployment_sanitycheck (int           rootfs_fd,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  /* Used by the test suite */
  if (getenv ("RPMOSTREE_SKIP_SANITYCHECK"))
    return TRUE;

  GLNX_AUTO_PREFIX_ERROR ("sanitycheck", error);
  g_autoptr(RpmOstreeBwrap) bwrap =
    rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_IMMUTABLE, error,
                         "--ro-bind", "./usr/etc", "/etc",
                         NULL);
  rpmostree_bwrap_append_child_argv (bwrap, "/usr/bin/true", NULL);
  if (!bwrap)
    return FALSE;
  if (!rpmostree_bwrap_run (bwrap, error))
    return FALSE;
  sd_journal_print (LOG_INFO, "sanitycheck(/usr/bin/true) successful");
  return TRUE;
}
