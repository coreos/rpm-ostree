/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 James Antil <james@fedoraproject.org>
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
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <glib-unix.h>

#include "rpmostree-builtins.h"
#include "rpmostree-treepkgdiff.h"
#include "rpmostree-util.h"

/* FIXME: */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")


#include "libgsystem.h"


#include <glib.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmdb.h>
#include <rpm/rpmts.h>


static char *opt_repo;
static char *opt_rpmdbdir;

static GOptionEntry option_entries[] = {
  { "repo", 'r', 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository", "REPO" },
  { "rpmdbdir", 0, 0, G_OPTION_ARG_STRING, &opt_rpmdbdir, "Working directory", "WORKDIR" },
  { NULL }
};


static void
header_free_p (gpointer data)
{
  headerFree (data);
}

static int
header_name_cmp (Header h1, Header h2)
{
  const char*    n1 = headerGetString (h1, RPMTAG_NAME);
  const char*    n2 = headerGetString (h2, RPMTAG_NAME);
  int cmp = strcmp (n1, n2);
  /* printf("%s <=> %s = %u\n", n1, n2, cmp); */
  return cmp;
}

static int
header_cmp_p (gconstpointer gph1, gconstpointer gph2)
{
  const Header *ph1 = gph1;
  const Header *ph2 = gph2;
  Header h1 = *ph1;
  Header h2 = *ph2;
  int cmp = header_name_cmp (h1, h2);
  if (!cmp)
    cmp = rpmVersionCompare (h1, h2);
  return cmp;
}

/* generate pkg nevra/etc. strings from Header objects */
static char *
pkg_envra_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  uint64_t      epoch = headerGetNumber (h1, RPMTAG_EPOCH);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);
  char *envra = NULL;

  if (!epoch)
    envra = g_strdup_printf ("%s-%s-%s.%s", name, version, release, arch);
  else
    {
      unsigned long long ullepoch = epoch;
      envra = g_strdup_printf ("%llu:%s-%s-%s.%s", ullepoch, name,
			       version, release, arch);
    }

  return envra;
}

static char *
pkg_nevra_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  uint64_t      epoch = headerGetNumber (h1, RPMTAG_EPOCH);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);
  char *nevra = NULL;

  if (!epoch)
    nevra = g_strdup_printf ("%s-%s-%s.%s", name, version, release, arch);
  else
    {
      unsigned long long ullepoch = epoch;
      nevra = g_strdup_printf ("%s-%llu:%s-%s.%s", name,
			       ullepoch, version, release, arch);
    }

  return nevra;
}

static char *
pkg_na_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  const char*    arch = headerGetString (h1, RPMTAG_ARCH);

  return g_strdup_printf ("%s.%s", name, arch);
}

static char *
pkg_nvr_strdup (Header h1)
{
  const char*    name = headerGetString (h1, RPMTAG_NAME);
  const char* version = headerGetString (h1, RPMTAG_VERSION);
  const char* release = headerGetString (h1, RPMTAG_RELEASE);

  return g_strdup_printf ("%s-%s-%s", name, version, release);
}


struct RpmHeaders
{
  rpmts ts; /* rpm transaction set the headers belong to */
  GPtrArray *hs; /* list of rpm header objects from <rpm.h> = Header */
};

#define CASENCMP_EQ(x, y, n) (g_ascii_strncasecmp (x, y, n) == 0)
#define CASEFNMATCH_EQ(x, y) (fnmatch (x, y, FNM_CASEFOLD) == 0)

/* find a common prefix length that doesn't need fnmatch */
static size_t
pat_fnmatch_prefix (GPtrArray *patterns)
{
  gsize ret = G_MAXSIZE;
  int num = 0;

  if (!patterns)
    return 0;

  for (num = 0; num < patterns->len; num++)
    {
      const char *pat = patterns->pdata[num];
      gsize prefix = 0;

      while (*pat)
	{
	  if (*pat == ':')
	    break;
	  if (*pat == '-')
	    break;
	  if (*pat == '*')
	    break;
	  if (*pat == '?')
	    break;
	  if (*pat == '.')
	    break;
	  if (*pat == '[')
	    break;

	  ++prefix;
	  ++pat;
	}

      ret = MIN(ret, prefix);
    }

  return ret;
}

static gboolean
pat_fnmatch_match (Header pkg, const char *name,
		   gsize patprefixlen, GPtrArray *patterns)
{
  int num = 0;
  gs_free char *pkg_na    = NULL;
  gs_free char *pkg_nevra = NULL;
  gs_free char *pkg_nvr   = NULL;

  if (!patterns)
    return TRUE;

  for (num = 0; num < patterns->len; num++)
    {
      const char *pattern = patterns->pdata[num];

      if (patprefixlen && !CASENCMP_EQ (name, pattern, patprefixlen))
	continue;

      if (!pkg_na)
	{
	  pkg_nevra = pkg_nevra_strdup (pkg);
	  pkg_na    = pkg_na_strdup (pkg);
	  pkg_nvr   = pkg_nvr_strdup (pkg);
	}

      if (CASEFNMATCH_EQ (pattern, name) ||
	  CASEFNMATCH_EQ (pattern, pkg_nevra) ||
	  CASEFNMATCH_EQ (pattern, pkg_na) ||
	  CASEFNMATCH_EQ (pattern, pkg_nvr) ||
	  FALSE)
	return TRUE;
    }

  return FALSE;
}

static struct RpmHeaders *
rpmhdrs_new (const char *root, GPtrArray *patterns)
{
 rpmts ts = rpmtsCreate();
 int status = -1;
 rpmdbMatchIterator iter;
 Header h1;
 GPtrArray *hs = NULL;
 struct RpmHeaders *ret = NULL;
 gsize patprefixlen = pat_fnmatch_prefix (patterns);;

 rpmtsSetVSFlags (ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES);

 status = rpmtsSetRootDir (ts, root);
 if (status) exit (2); // FIXME: better fail.

 /* iter = rpmtsInitIterator (ts, RPMTAG_NAME, "yum", 0); */
 iter = rpmtsInitIterator (ts, RPMDBI_PACKAGES, NULL, 0);

 hs = g_ptr_array_new_with_free_func (header_free_p);
 while ((h1 = rpmdbNextIterator (iter)))
 {
   const char*    name = headerGetString (h1, RPMTAG_NAME);
   
   if (g_str_equal (name, "gpg-pubkey")) continue; /* rpmdb abstraction leak */

   if (!pat_fnmatch_match (h1, name, patprefixlen, patterns))
     continue;

   h1 = headerLink (h1);
   g_ptr_array_add (hs, h1);
 }
 iter = rpmdbFreeIterator (iter);

 g_ptr_array_sort (hs, header_cmp_p);

 ret = g_malloc0 (sizeof (struct RpmHeaders));

 ret->ts = ts;
 ret->hs = hs;

 return ret;
}

static void
rpmhdrs_free (struct RpmHeaders *l1)
{
 g_ptr_array_free (l1->hs, TRUE);
 l1->hs = NULL;
 l1->ts = rpmtsFree (l1->ts);

 g_free (l1);
}

static char *
pkg_yumdb_relpath (Header h1)
{
   const char*    name = headerGetString (h1, RPMTAG_NAME);
   const char* version = headerGetString (h1, RPMTAG_VERSION);
   const char* release = headerGetString (h1, RPMTAG_RELEASE);
   const char*    arch = headerGetString (h1, RPMTAG_ARCH);
   const char*   pkgid = headerGetString (h1, RPMTAG_SHA1HEADER);
   char *path = NULL;

   g_assert (name[0]);

   // FIXME: sanitize name ... remove '/' and '~'

   // FIXME: If pkgid doesn't exist use: name.buildtime

   path = g_strdup_printf ("%c/%s-%s-%s-%s-%s", name[0],
			   pkgid, name, version, release, arch);

   return path;
}

static GInputStream *
pkg_yumdb_file_read (GFile *root, Header pkg, const char *yumdb_key,
		     GCancellable   *cancellable,
		     GError        **error)
{
  gs_unref_object GFile *f = NULL;
  gs_free char *pkgpath = pkg_yumdb_relpath (pkg);
  gs_free char *path = g_strconcat ("/var/lib/yum/yumdb/", pkgpath, "/",
				    yumdb_key, NULL);

  f = g_file_resolve_relative_path (root, path);
  return (GInputStream*)g_file_read (f, cancellable, error);
}

static char *
pkg_yumdb_strdup (GFile *root, Header pkg, const char *yumdb_key,
		  GCancellable   *cancellable,
		  GError        **error)
{
  gs_unref_object GFile *f = NULL;
  gs_free char *pkgpath = pkg_yumdb_relpath (pkg);
  gs_free char *path = g_strconcat ("/var/lib/yum/yumdb/", pkgpath, "/",
				    yumdb_key, NULL);
  char *ret = NULL;
  
  f = g_file_resolve_relative_path (root, path);

  if (!_rpmostree_file_load_contents_utf8_allow_noent (f, &ret,
						       cancellable, error))
    return g_strdup ("");

  return ret;
}

static gsize
_console_get_width(int fd)
{
  struct winsize w;

  ioctl (STDOUT_FILENO, TIOCGWINSZ, &w);

  return w.ws_col;
}

static gsize
_console_get_width_stdout_cached(void)
{
  static gsize align = 0;

  if (!align)
      align = _console_get_width (1);

  return align;
}

static void
pkg_print (GFile *root, Header pkg,
	   GCancellable   *cancellable,
	   GError        **error)
{
  gs_free char *nevra = pkg_nevra_strdup (pkg);
  gs_free char *from_repo = pkg_yumdb_strdup (root, pkg, "from_repo",
					      cancellable, error);
  gsize align = _console_get_width_stdout_cached ();

  if (*from_repo)
    {
      if (align)
        {
          gsize plen = strlen (nevra);
          gsize rlen = strlen (from_repo) + 1;
          int off = 0;

          --align; // hacky ... for leading spaces.

          off = align - (plen + rlen);

          if (align > (plen + rlen))
            printf ("%s%*s@%s\n", nevra, off, "", from_repo);
          else
            align = 0;
        }
      if (!align)
        printf ("%s @%s\n", nevra, from_repo);
    }
  else
    printf("%s\n", nevra);
}

static void
rpmhdrs_list (GFile *root, struct RpmHeaders *l1,
	      GCancellable   *cancellable,
	      GError        **error)
{
 int num = 0;

 while (num < l1->hs->len)
 {
   Header h1 = l1->hs->pdata[num++];
   printf (" ");
   pkg_print (root, h1, cancellable, error);
 }
}

static char *
rpmhdrs_rpmdbv (GFile *root, struct RpmHeaders *l1,
		GCancellable   *cancellable,
		GError        **error)
{
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA1);
  gs_free char *checksum_cstr = NULL;
  int num = 0;

  while (num < l1->hs->len)
    {
      Header pkg = l1->hs->pdata[num++];
      gs_unref_object GInputStream *tin = NULL;
      gs_unref_object GInputStream *din = NULL;
      char tbuf[1024];
      char dbuf[1024];
      gs_free char *envra = pkg_envra_strdup (pkg);
      gsize tbytes_read = 0;
      gsize dbytes_read = 0;
      
      g_checksum_update (checksum, (guint8*)envra, strlen(envra));
      
      tin = pkg_yumdb_file_read (root, pkg, "checksum_type", cancellable,error);
      if (!tin)
	continue;
    
      din = pkg_yumdb_file_read (root, pkg, "checksum_data", cancellable,error);
      if (!din)
	continue;

      if (!g_input_stream_read_all (tin, tbuf, sizeof(tbuf), &tbytes_read,
				    cancellable, error))
	continue;
      if (!g_input_stream_read_all (din, dbuf, sizeof(dbuf), &dbytes_read,
				    cancellable, error))
	continue;
      
      if (tbytes_read >= 512)
	continue; // should be == len(md5) or len(sha256) etc.
      if (dbytes_read >= 1024)
	continue; // should be digest size of md5/sha256/sha512/etc.
      
      g_checksum_update (checksum, (guint8*)tbuf, tbytes_read);
      g_checksum_update (checksum, (guint8*)dbuf, dbytes_read);
    }
  
  checksum_cstr = g_strdup (g_checksum_get_string (checksum));
  
  g_checksum_free (checksum);
  
  return g_strdup_printf ("%u:%s", num, checksum_cstr);
}

static void
rpmhdrs_diff (GFile *root1, struct RpmHeaders *l1,
	      GFile *root2, struct RpmHeaders *l2,
	      GCancellable   *cancellable,
	      GError        **error)
{
 int n1 = 0;
 int n2 = 0;

 while (n1 < l1->hs->len)
   {
     Header h1 = l1->hs->pdata[n1];
     if (n2 >= l2->hs->len)
       {
	 printf ("-");
	 pkg_print (root1, h1, cancellable, error);
	 ++n1;
       }
     else
       {
	 Header h2 = l2->hs->pdata[n2];
	 int cmp = header_name_cmp (h1, h2);
	 
	 if (cmp > 0)
	   {
	     printf("+");
	     pkg_print (root2, h2, cancellable, error);
	     ++n2;
	   }
	 else if (cmp < 0)
	   {
	     printf("-");
	     pkg_print (root1, h1, cancellable, error);
	     ++n1;
	   }
	 else
	   {
	     cmp = rpmVersionCompare (h1, h2);
	     if (!cmp) { ++n1; ++n2; continue; }
	     
	     printf("!<= ");
	     pkg_print (root1, h1, cancellable, error);
	     ++n1;
	     printf("!>= ");
	     pkg_print (root2, h2, cancellable, error);
	     ++n2;
	   }
       }
   }

 while (n2 < l2->hs->len)
   {
     Header h2 = l2->hs->pdata[n2];
     
     printf("+");
     pkg_print (root2, h2, cancellable, error);
     ++n2;
   }
}

/* data needed to extract rpm/yum data from a commit revision */
struct RpmRevisionData
{
  struct RpmHeaders *rpmdb;
  GFile *root;
  char *commit;
};

static void
rpmrev_free (struct RpmRevisionData *ptr)
{
  gs_unref_object GFile *root = NULL;
  gs_free char *commit = NULL;

  if (!ptr)
    return;
  
  rpmhdrs_free (ptr->rpmdb);
  ptr->rpmdb = NULL;
  
  root = ptr->root;
  ptr->root = NULL;

  commit = ptr->commit;
  ptr->commit = NULL;

  g_free (ptr);
}

_RPMOSTREE_DEFINE_TRIVIAL_CLEANUP_FUNC(struct RpmRevisionData *, rpmrev_free);

#define _cleanup_rpmrev_ __attribute__((cleanup(rpmrev_freep)))

static struct RpmRevisionData *
rpmrev_new (OstreeRepo *repo, GFile *rpmdbdir, const char *rev,
	    GPtrArray *patterns,
	    GCancellable   *cancellable,
	    GError        **error)
{
  gs_unref_object GFile *subtree = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *targetp = NULL;
  gs_unref_object GFile *target = NULL;
  gs_free char *targetp_path = NULL;
  gs_free char *target_path = NULL;
  gs_unref_object GFile *revdir = NULL;
  GError *tmp_error = NULL;
  struct RpmRevisionData *rpmrev = NULL;
  gs_unref_object GFile *root = NULL;
  gs_free char *commit = NULL;
  
  if (!ostree_repo_read_commit (repo, rev, &root, &commit, NULL, error))
    goto out;
  
  subtree = g_file_resolve_relative_path (root, "/var/lib/rpm");
  if (!g_file_query_exists (subtree, cancellable))
    {
      g_object_unref (subtree);
      subtree = g_file_resolve_relative_path (root, "/usr/share/rpm");
    }

  file_info = g_file_query_info (subtree, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, &tmp_error);
  if (!file_info) // g_file_query_exists (subtree, cancellable))
  {
    g_propagate_error (error, tmp_error);
    //    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
    //		 "No rpmdb directory found");
    goto out;
  }

  revdir = g_file_resolve_relative_path (rpmdbdir, commit);

  targetp_path = g_strconcat (commit, "/var/lib", NULL);
  targetp = g_file_resolve_relative_path (rpmdbdir, targetp_path);
  target_path = g_strconcat (commit, "/var/lib/rpm", NULL);
  target = g_file_resolve_relative_path (rpmdbdir, target_path);
  if (!g_file_query_exists (target, cancellable) &&
      (!gs_file_ensure_directory (targetp, TRUE, cancellable, error) ||
       !ostree_repo_checkout_tree (repo, OSTREE_REPO_CHECKOUT_MODE_NONE,
				   OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
				   target, OSTREE_REPO_FILE (subtree),
				   file_info, cancellable, error)))
    goto out;

  rpmrev = g_malloc0 (sizeof(struct RpmRevisionData));
  
  rpmrev->root   = root;   root = NULL;
  rpmrev->commit = commit; commit = NULL;
  rpmrev->rpmdb  = rpmhdrs_new (gs_file_get_path_cached (revdir), patterns);

 out:
  return rpmrev;
}

gboolean
rpmostree_builtin_rpm (int             argc,
		       char          **argv,
		       GCancellable   *cancellable,
		       GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *deployments = NULL;  // list of all depoyments
  GOptionContext *context = g_option_context_new ("- Run rpm commands on systems");
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *rpmdbdir = NULL;
  gboolean rpmdbdir_is_tmp = FALSE;
  const char *cmd = NULL;
  int argnum = 2;
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;


  if (argc < 3)
    {
      g_printerr ("usage: rpm-ostree rpm diff|list|version COMMIT...\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Argument processing failed");
      goto out;
    }
  cmd = argv[1];

  if ((g_str_equal (cmd, "diff")) && (argc != 4))
    {
      g_printerr ("usage: rpm-ostree rpm diff COMMIT COMMIT\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Argument processing failed");
      goto out;
    }

  if (!opt_repo)
  {
    gs_unref_object OstreeSysroot *sysroot = NULL;

    sysroot = ostree_sysroot_new_default ();
    if (!ostree_sysroot_load (sysroot, cancellable, error))
      goto out;

    if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
      goto out;
  }
  else
    {
      gs_unref_object GFile *repo_path = NULL;

      repo_path = g_file_new_for_path (opt_repo);
      repo = ostree_repo_new (repo_path);
      if (!ostree_repo_open (repo, cancellable, error))
	goto out;
    }
  
  if (rpmReadConfigFiles (NULL, NULL))
    exit (2); // FIXME: Better fail

  if (opt_rpmdbdir)
    {
      rpmdbdir = g_file_new_for_path (opt_rpmdbdir);
    }
  else
    {
      gs_free char *tmpd = g_mkdtemp (g_strdup ("/var/tmp/rpm-ostree.XXXXXX"));
      rpmdbdir = g_file_new_for_path (tmpd);
      rpmdbdir_is_tmp = TRUE;
    }

  if (FALSE) {}
  else if (g_str_equal(cmd, "version"))
    for (; argnum < argc; ++argnum)
      {
	const char *rev = argv[argnum];
	_cleanup_rpmrev_ struct RpmRevisionData *rpmrev = NULL;
	gs_free char *rpmdbv = NULL;

	rpmrev = rpmrev_new (repo, rpmdbdir, rev, NULL, cancellable, error);
	if (!rpmrev)
	  goto out;

	rpmdbv = rpmhdrs_rpmdbv (rpmrev->root, rpmrev->rpmdb,
				 cancellable, error);
	
	// FIXME: g_console
	if (!g_str_equal (rev, rpmrev->commit))
	  printf ("ostree commit: %s (%s)\n", rev, rpmrev->commit);
	else
	  printf ("ostree commit: %s\n", rev);
	printf ("  rpmdbv is: %24s%s\n", "", rpmdbv);
      }
  else if (g_str_equal (cmd, "diff"))
    {
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev1 = NULL;
      _cleanup_rpmrev_ struct RpmRevisionData *rpmrev2 = NULL;
      
      if (!(rpmrev1 = rpmrev_new (repo, rpmdbdir, argv[2], NULL,
				  cancellable, error)))
	goto out;
      if (!(rpmrev2 = rpmrev_new (repo, rpmdbdir, argv[3], NULL,
				  cancellable, error)))
	goto out;

      if (!g_str_equal (argv[2], rpmrev1->commit))
	printf ("ostree diff commit old: %s (%s)\n", argv[2], rpmrev1->commit);
      else
	printf ("ostree diff commit old: %s\n", argv[2]);
      if (!g_str_equal (argv[3], rpmrev2->commit))
	printf ("ostree diff commit new: %s (%s)\n", argv[3], rpmrev2->commit);
      else
	printf ("ostree diff commit new: %s\n", argv[3]);

      rpmhdrs_diff (rpmrev1->root, rpmrev1->rpmdb, rpmrev2->root,rpmrev2->rpmdb,
		    cancellable, error);
    }
  else if (g_str_equal (cmd, "list"))
    {
      gs_unref_ptrarray GPtrArray *patterns = NULL;
      int listnum = argc - 3;
      char **listargv = argv + 2;
      char *commit;

      // Find first commit arg. ... all before it are list args.
      while ((listnum > 0) &&
	     ostree_repo_resolve_rev (repo, listargv[listnum-1], TRUE,
				      &commit, NULL) && commit)
	{
	  g_free (commit); // FiXME: broken allow_noent?
	  listnum--;
	}

      argc -= listnum;
      argv += listnum;
      if (listnum)
	{
	  patterns = g_ptr_array_new ();
	  while (listnum--)
	    g_ptr_array_add (patterns, *listargv++);
	}

      for (; argnum < argc; ++argnum)
	{
	  const char *rev = argv[argnum];
	  _cleanup_rpmrev_ struct RpmRevisionData *rpmrev = NULL;

	  rpmrev = rpmrev_new (repo, rpmdbdir, rev, patterns,
			       cancellable, error);
	  if (!rpmrev)
	    goto out;
	  
	  if (!g_str_equal (rev, rpmrev->commit))
	    printf ("ostree commit: %s (%s)\n", rev, rpmrev->commit);
	  else
	    printf ("ostree commit: %s\n", rev);
	  
	  rpmhdrs_list (rpmrev->root, rpmrev->rpmdb, cancellable, error);
	}
    }
  else
    {
      g_printerr ("rpm-ostree rpm SUB-COMMANDS:\n");
      g_printerr ("  diff COMMIT COMMIT\n");
      g_printerr ("  list [prefix-pkgname...] COMMIT...\n");
      g_printerr ("  version COMMIT...\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Command processing failed");
      goto out;
    }
  
  ret = TRUE;
 out:
  if (rpmdbdir_is_tmp)
    (void) gs_shutil_rm_rf (rpmdbdir, NULL, NULL);
  if (context)
    g_option_context_free (context);

  return ret;
}
