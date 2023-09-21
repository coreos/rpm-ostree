/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include <optional>
#include <sstream>

#include "rpmostree-util.h"
#include "rpmostreed-utils.h"
#include <gio/gunixoutputstream.h>
#include <libglnx.h>
#include <systemd/sd-journal.h>

#include "rpmostree-core.h"
#include "rpmostree-cxxrs.h"
#include "rpmostree-kernel.h"
#include "rpmostree-origin.h"
#include "rpmostree-output.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-scripts.h"
#include "rpmostree-sysroot-upgrader.h"
#include "rpmostreed-daemon.h"
#include "rpmostreed-deployment-utils.h"

#include "ostree-repo.h"

#define RPMOSTREE_NEW_DEPLOYMENT_MSG                                                               \
  SD_ID128_MAKE (9b, dd, bd, a1, 77, cd, 44, d8, 91, b1, b5, 61, a8, a0, ce, 9e)

/**
 * SECTION:rpmostree-sysroot-upgrader
 * @title: Process an origin file to look for updates
 * @short_description: Client side update for rpm-ostree systems
 *
 * The base libostree "upgrade" logic is ultimately very simple - a client
 * system is tracking a single ostree branch.  This appears as
 * `origin/refspec` key in the origin file.
 *
 * rpm-ostree has much more logic and sophistication - optional package layering, overrides,
 * client side initramfs regeneration, etc.
 *
 * This upgrader is intended to be similar to
 * [OstreeSysrootUpgrader](https://ostreedev.github.io/ostree/reference/ostree-Simple-upgrade-class.html).
 * If none of those features are enabled, then all we do is pull new OSTree updates.
 *
 * If extended features are enabled, rpm-ostree adds keys the base ostree "origin" file logic;
 * see also `rpmostree-origin.h`.  Most of the manipulation of the origin file is performed
 * via DBus in `rpmostree-transaction-types.cxx`.
 *
 * One important subtlety here is optimizing the case of
 * e.g. `rpm-ostree install --allow-inactive docker` on a system that already has `docker`
 * installed.  The intended semantics here are that we just notice `docker` is installed
 * and treat it as a no-op.  If however on an upgrade/rebase to a new version the package
 * is no longer in the base, then we enable package layering.  To implement this we
 * also maintain a `computed_origin`.  If for example we detect `docker` is requested but
 * installed, then we remove it from the `computed_origin` package list.  If the final set
 * of requested packages is empty, we then know not to enable the client-side libdnf bits.
 */
typedef struct
{
  GObjectClass parent_class;
} RpmOstreeSysrootUpgraderClass;

struct RpmOstreeSysrootUpgrader
{
  GObject parent;

  OstreeSysroot *sysroot;
  OstreeRepo *repo;
  char *osname;
  RpmOstreeSysrootUpgraderFlags flags;
  char *command_line;
  char *agent;
  char *sd_unit;

  OstreeDeployment *cfg_merge_deployment;
  OstreeDeployment *origin_merge_deployment;
  // The origin that was passed in and should be written to the deployment.
  RpmOstreeOrigin *original_origin;
  // An in-memory computed subset of the origin; used mainly as an optimization
  // to e.g. avoid fetching rpm-md if all we have is "inactive requests".
  // Longer term, this functionality should be moved down into the core.
  RpmOstreeOrigin *computed_origin;
  std::optional<rust::Box<rpmostreecxx::Treefile> > treefile;

  /* Used during tree construction */
  OstreeRepoDevInoCache *devino_cache;
  int tmprootfs_dfd;
  RpmOstreeRefSack *rsack; /* sack of base layer */
  GLnxTmpDir metatmpdir;
  RpmOstreeContext *ctx;
  DnfSack *rpmmd_sack; /* sack from core */

  gboolean layering_initialized; /* Whether layering_type is known */
  RpmOstreeSysrootUpgraderLayeringType layering_type;
  gboolean layering_changed; /* Whether changes to layering should result in a new commit */
  gboolean pkgs_imported;    /* Whether pkgs to be layered have been downloaded & imported */
  char *base_revision;       /* Non-layered replicated commit */
  char *final_revision;      /* Computed by layering; if NULL, only using base_revision */

  char **kargs_strv; /* Kernel argument list to be written into deployment  */
};

enum
{
  PROP_0,

  PROP_SYSROOT,
  PROP_OSNAME,
  PROP_FLAGS
};

static void rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmOstreeSysrootUpgrader, rpmostree_sysroot_upgrader, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                rpmostree_sysroot_upgrader_initable_iface_init))

static gboolean
parse_origin_deployment (RpmOstreeSysrootUpgrader *self, OstreeDeployment *deployment,
                         GCancellable *cancellable, GError **error)
{
  self->original_origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!self->original_origin)
    return FALSE;
  rpmostree_origin_set_override_commit (self->original_origin, NULL);

  auto unconfigured = rpmostree_origin_get_unconfigured_state (self->original_origin);
  if (!unconfigured.empty ()
      && !(self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED))
    {
      /* explicit action is required OS creator to upgrade, print their text as an error */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "origin unconfigured-state: %s",
                   unconfigured.c_str ());
      return FALSE;
    }

  self->computed_origin = rpmostree_origin_dup (self->original_origin);

  return TRUE;
}

static gboolean
rpmostree_sysroot_upgrader_initable_init (GInitable *initable, GCancellable *cancellable,
                                          GError **error)
{
  RpmOstreeSysrootUpgrader *self = (RpmOstreeSysrootUpgrader *)initable;

  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (self->sysroot);
  if (booted_deployment == NULL && self->osname == NULL)
    return glnx_throw (error, "Not currently booted into an OSTree system and no OS specified");

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    return glnx_throw (error, "Invalid empty osname");

  if (!ostree_sysroot_get_repo (self->sysroot, &self->repo, cancellable, error))
    return FALSE;

  self->cfg_merge_deployment = ostree_sysroot_get_merge_deployment (self->sysroot, self->osname);
  self->origin_merge_deployment
      = rpmostree_syscore_get_origin_merge_deployment (self->sysroot, self->osname);
  if (self->cfg_merge_deployment == NULL || self->origin_merge_deployment == NULL)
    return glnx_throw (error, "No previous deployment for OS '%s'", self->osname);

  /* Should we consider requiring --discard-hotfix here?
   * See also `ostree admin upgrade` bits.
   */
  if (!parse_origin_deployment (self, self->origin_merge_deployment, cancellable, error))
    return FALSE;

  const char *merge_deployment_csum = ostree_deployment_get_csum (self->origin_merge_deployment);

  /* Now, load starting base/final checksums.  We may end up changing
   * one or both if we upgrade.  But we also want to support redeploying
   * without changing them.
   */
  if (!rpmostree_deployment_get_base_layer (self->repo, self->origin_merge_deployment,
                                            &self->base_revision, error))
    return FALSE;

  if (self->base_revision)
    self->final_revision = g_strdup (merge_deployment_csum);
  else
    self->base_revision = g_strdup (merge_deployment_csum);

  g_assert (self->base_revision);
  return TRUE;
}

static void
rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface)
{
  iface->init = rpmostree_sysroot_upgrader_initable_init;
}

static void
rpmostree_sysroot_upgrader_finalize (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_clear_pointer (&self->rsack, rpmostree_refsack_unref);
  g_clear_object (&self->rpmmd_sack);
  g_clear_object (&self->ctx); /* Note this is already cleared in the happy path */
  glnx_close_fd (&self->tmprootfs_dfd);

  (void)glnx_tmpdir_delete (&self->metatmpdir, NULL, NULL);

  g_clear_pointer (&self->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->repo);
  g_free (self->osname);
  g_free (self->command_line);
  g_free (self->agent);
  g_free (self->sd_unit);

  g_clear_object (&self->cfg_merge_deployment);
  g_clear_object (&self->origin_merge_deployment);
  g_clear_pointer (&self->original_origin, (GDestroyNotify)rpmostree_origin_unref);
  g_clear_pointer (&self->computed_origin, (GDestroyNotify)rpmostree_origin_unref);
  g_free (self->base_revision);
  g_free (self->final_revision);
  g_strfreev (self->kargs_strv);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->finalize (object);
}

static void
rpmostree_sysroot_upgrader_set_property (GObject *object, guint prop_id, const GValue *value,
                                         GParamSpec *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      self->sysroot = (OstreeSysroot *)g_value_dup_object (value);
      break;
    case PROP_OSNAME:
      self->osname = g_value_dup_string (value);
      break;
    case PROP_FLAGS:
      self->flags = static_cast<RpmOstreeSysrootUpgraderFlags> (g_value_get_flags (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_get_property (GObject *object, guint prop_id, GValue *value,
                                         GParamSpec *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      g_value_set_object (value, self->sysroot);
      break;
    case PROP_OSNAME:
      g_value_set_string (value, self->osname);
      break;
    case PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_constructed (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_assert (self->sysroot != NULL);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->constructed (object);
}

static void
rpmostree_sysroot_upgrader_class_init (RpmOstreeSysrootUpgraderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = rpmostree_sysroot_upgrader_constructed;
  object_class->get_property = rpmostree_sysroot_upgrader_get_property;
  object_class->set_property = rpmostree_sysroot_upgrader_set_property;
  object_class->finalize = rpmostree_sysroot_upgrader_finalize;

  g_object_class_install_property (
      object_class, PROP_SYSROOT,
      g_param_spec_object ("sysroot", "", "", OSTREE_TYPE_SYSROOT,
                           static_cast<GParamFlags> (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)));

  g_object_class_install_property (
      object_class, PROP_OSNAME,
      g_param_spec_string ("osname", "", "", NULL,
                           static_cast<GParamFlags> (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)));

  g_object_class_install_property (
      object_class, PROP_FLAGS,
      g_param_spec_flags ("flags", "", "", rpmostree_sysroot_upgrader_flags_get_type (), 0,
                          static_cast<GParamFlags> (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)));
}

static void
rpmostree_sysroot_upgrader_init (RpmOstreeSysrootUpgrader *self)
{
  self->tmprootfs_dfd = -1;
}

/**
 * rpmostree_sysroot_upgrader_new_for_os_with_flags:
 * @sysroot: An #OstreeSysroot
 * @osname: (allow-none): Operating system name
 * @flags: Flags
 *
 * Returns: (transfer full): An upgrader
 */
RpmOstreeSysrootUpgrader *
rpmostree_sysroot_upgrader_new (OstreeSysroot *sysroot, const char *osname,
                                RpmOstreeSysrootUpgraderFlags flags, GCancellable *cancellable,
                                GError **error)
{
  return (RpmOstreeSysrootUpgrader *)g_initable_new (RPMOSTREE_TYPE_SYSROOT_UPGRADER, cancellable,
                                                     error, "sysroot", sysroot, "osname", osname,
                                                     "flags", flags, NULL);
}

void
rpmostree_sysroot_upgrader_set_caller_info (RpmOstreeSysrootUpgrader *self,
                                            const char *initiating_command_line, const char *agent,
                                            const char *sd_unit)
{
  g_free (self->command_line);
  self->command_line = g_strdup (initiating_command_line);
  g_free (self->agent);
  self->agent = g_strdup (agent);
  g_free (self->sd_unit);
  self->sd_unit = g_strdup (sd_unit);
}

RpmOstreeOrigin *
rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self)
{
  return rpmostree_origin_dup (self->original_origin);
}

void
rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self, RpmOstreeOrigin *new_origin)
{
  rpmostree_origin_unref (self->original_origin);
  self->original_origin = rpmostree_origin_dup (new_origin);
  rpmostree_origin_unref (self->computed_origin);
  self->computed_origin = rpmostree_origin_dup (self->original_origin);
}

const char *
rpmostree_sysroot_upgrader_get_base (RpmOstreeSysrootUpgrader *self)
{
  return self->base_revision;
}

OstreeDeployment *
rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self)
{
  return self->origin_merge_deployment;
}

/* Returns DnfSack used, if any. */
DnfSack *
rpmostree_sysroot_upgrader_get_sack (RpmOstreeSysrootUpgrader *self, GError **error)
{
  return self->rpmmd_sack;
}

/*
 * Like ostree_sysroot_upgrader_pull(), but also handles the `baserefspec` we
 * use when doing layered packages.
 */
gboolean
rpmostree_sysroot_upgrader_pull_base (RpmOstreeSysrootUpgrader *self, const char *dir_to_pull,
                                      OstreeRepoPullFlags flags, OstreeAsyncProgress *progress,
                                      gboolean *out_changed, GCancellable *cancellable,
                                      GError **error)
{
  g_assert (cancellable);

  const gboolean check = (flags & OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY) > 0;
  const gboolean allow_older = (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;
  const gboolean synthetic = (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL) > 0;

  auto override_commit_s = rpmostree_origin_get_override_commit (self->computed_origin);
  const char *override_commit = NULL;
  if (!override_commit_s.empty ())
    override_commit = override_commit_s.c_str ();

  auto r = rpmostree_origin_get_refspec (self->computed_origin);

  g_autofree char *new_base_rev = NULL;

  switch (r.kind)
    {
    case rpmostreecxx::RefspecType::Container:
      {
        if (override_commit)
          return glnx_throw (error, "Specifying commit overrides for container-image-reference "
                                    "type refspecs is not supported");
        if (check)
          {
            CXX_TRY_VAR (changed,
                         rpmostreecxx::check_container_update (*self->repo, *cancellable,
                                                               r.refspec.c_str ()),
                         error);
            *out_changed = changed;
            return TRUE;
          }
        else
          {
            CXX_TRY_VAR (
                import,
                rpmostreecxx::pull_container (*self->repo, *cancellable, r.refspec.c_str ()),
                error);

            new_base_rev = g_strdup (import->merge_commit.c_str ());
          }
        break;
      }
    case rpmostreecxx::RefspecType::Checksum:
    case rpmostreecxx::RefspecType::Ostree:
      {
        g_autofree char *origin_remote = NULL;
        g_autofree char *origin_ref = NULL;
        if (!ostree_parse_refspec (r.refspec.c_str (), &origin_remote, &origin_ref, error))
          return FALSE;

        const gboolean is_commit = ostree_validate_checksum_string (origin_ref, NULL);

        g_assert (self->origin_merge_deployment);
        if (origin_remote && !synthetic && !is_commit)
          {
            g_autoptr (GVariantBuilder) optbuilder
                = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
            if (dir_to_pull && *dir_to_pull)
              g_variant_builder_add (optbuilder, "{s@v}", "subdir",
                                     g_variant_new_variant (g_variant_new_string (dir_to_pull)));
            g_variant_builder_add (optbuilder, "{s@v}", "flags",
                                   g_variant_new_variant (g_variant_new_int32 (flags)));
            /* Add the timestamp check, unless disabled. The option was added in
             * libostree v2017.11 */
            if (!allow_older)
              {
                g_variant_builder_add (optbuilder, "{s@v}", "timestamp-check",
                                       g_variant_new_variant (g_variant_new_boolean (TRUE)));
                /* XXX: Short-term hack until we switch to timestamp-check-from-rev:
                 * https://github.com/coreos/rpm-ostree/pull/2094. This ensures that
                 * timestamp-check is comparing against our deployment csum's timestamp, not
                 * whatever the ref is pointing to.
                 */
                if (override_commit
                    && !ostree_repo_set_ref_immediate (self->repo, origin_remote, origin_ref,
                                                       self->base_revision, cancellable, error))
                  return FALSE;
              }
            g_variant_builder_add (
                optbuilder, "{s@v}", "refs",
                g_variant_new_variant (g_variant_new_strv ((const char *const *)&origin_ref, 1)));
            if (override_commit)
              g_variant_builder_add (optbuilder, "{s@v}", "override-commit-ids",
                                     g_variant_new_variant (g_variant_new_strv (
                                         (const char *const *)&override_commit, 1)));

            g_autoptr (GVariant) opts = g_variant_ref_sink (g_variant_builder_end (optbuilder));
            if (!ostree_repo_pull_with_options (self->repo, origin_remote, opts, progress,
                                                cancellable, error))
              return glnx_prefix_error (error, "While pulling %s", override_commit ?: origin_ref);

            if (progress)
              ostree_async_progress_finish (progress);
          }

        if (override_commit)
          new_base_rev = g_strdup (override_commit);
        else
          {
            if (!ostree_repo_resolve_rev (self->repo, r.refspec.c_str (), FALSE, &new_base_rev,
                                          error))
              return FALSE;
          }
      }
      break;
    }

  gboolean changed = !g_str_equal (new_base_rev, self->base_revision);
  if (changed)
    {
      /* check timestamps here too in case the commit was already pulled, or the pull was
       * synthetic, or the refspec is local */
      if (!allow_older)
        {
          if (!ostree_sysroot_upgrader_check_timestamps (self->repo, self->base_revision,
                                                         new_base_rev, error))
            return glnx_prefix_error (error, "While checking against deployment timestamp");
        }

      g_free (self->base_revision);
      self->base_revision = util::move_nullify (new_base_rev);
    }

  *out_changed = changed;
  return TRUE;
}

static gboolean
checkout_base_tree (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  if (self->tmprootfs_dfd != -1)
    return TRUE; /* already checked out! */

  /* let's give the user some feedback so they don't think we're blocked */
  auto msg = g_strdup_printf ("Checking out tree %.7s", self->base_revision);
  auto task = rpmostreecxx::progress_begin_task (msg);

  int repo_dfd = ostree_repo_get_dfd (self->repo); /* borrowed */
  /* Always delete this */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_OLD_TMP_ROOTFS_DIR, cancellable, error))
    return FALSE;

  /* Make the parents with default mode */
  if (!glnx_shutil_mkdir_p_at (repo_dfd, dirname (strdupa (RPMOSTREE_TMP_PRIVATE_DIR)), 0755,
                               cancellable, error))
    return FALSE;

  /* And this dir should always be 0700, to ensure that when we checkout
   * world-writable dirs like /tmp it's not accessible to unprivileged users.
   */
  if (!glnx_shutil_mkdir_p_at (repo_dfd, RPMOSTREE_TMP_PRIVATE_DIR, 0700, cancellable, error))
    return FALSE;

  /* delete dir in case a previous run didn't finish successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR, cancellable, error))
    return FALSE;

  /* NB: we let ostree create the dir for us so that the root dir has the
   * correct xattrs (e.g. selinux label) */
  self->devino_cache = ostree_repo_devino_cache_new ();
  OstreeRepoCheckoutAtOptions checkout_options = { .devino_to_csum_cache = self->devino_cache };
  if (!ostree_repo_checkout_at (self->repo, &checkout_options, repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                                self->base_revision, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR, FALSE, &self->tmprootfs_dfd, error))
    return FALSE;

  return TRUE;
}

/* Optimization: use the already checked out base rpmdb of the pending deployment if the
 * base layer matches. Returns FALSE on error, TRUE otherwise. Check self->rsack to
 * determine if it worked. */
static gboolean
try_load_base_rsack_from_pending (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable,
                                  GError **error)
{
  CXX_TRY_VAR (is_live,
               rpmostreecxx::has_live_apply_state (*self->sysroot, *self->origin_merge_deployment),
               error);
  /* livefs invalidates the deployment */
  if (is_live)
    return TRUE;

  auto repo = ostree_sysroot_repo (self->sysroot);
  CXX_TRY_VAR (layeredmeta,
               rpmostreecxx::deployment_layeredmeta_load (*repo, *self->origin_merge_deployment),
               error);
  /* older client layers have a bug blocking us from using their base rpmdb:
   * https://github.com/projectatomic/rpm-ostree/pull/1560 */
  if (layeredmeta.is_layered && layeredmeta.clientlayer_version < 4)
    return TRUE;

  const char *base_rev = layeredmeta.base_commit.c_str ();
  /* it's no longer the base layer we're looking for (e.g. likely pulled a fresh one) */
  if (!g_str_equal (self->base_revision, base_rev))
    return TRUE;

  int sysroot_fd = ostree_sysroot_get_fd (self->sysroot);
  g_autofree char *path
      = ostree_sysroot_get_deployment_dirpath (self->sysroot, self->origin_merge_deployment);

  /* this may not actually populate the rsack if it's an old deployment */
  if (!rpmostree_get_base_refsack_for_root (sysroot_fd, path, &self->rsack, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
load_base_rsack (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  if (!try_load_base_rsack_from_pending (self, cancellable, error))
    return FALSE;

  if (self->rsack == NULL)
    {
      /* fallback to checking out the tree early; will be reused later for assembly */
      if (!checkout_base_tree (self, cancellable, error))
        return FALSE;

      self->rsack = rpmostree_get_refsack_for_root (self->tmprootfs_dfd, ".", error);
      if (self->rsack == NULL)
        return FALSE;
    }

  g_assert (self->rsack != NULL);
  return TRUE;
}

static gboolean
initialize_metatmpdir (RpmOstreeSysrootUpgrader *self, GError **error)
{
  if (self->metatmpdir.initialized)
    return TRUE; /* already initialized; return early */

  if (!glnx_mkdtemp ("rpmostree-localpkgmeta-XXXXXX", 0700, &self->metatmpdir, error))
    return FALSE;

  return TRUE;
}

static gboolean
finalize_removal_overrides (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable,
                            GError **error)
{
  g_assert (self->rsack);

  auto removals = rpmostree_origin_get_overrides_remove (self->computed_origin);
  g_autoptr (GPtrArray) inactive_removals = g_ptr_array_new ();
  for (auto &pkgname : removals)
    {
      g_autoptr (DnfPackage) pkg = NULL;
      if (!rpmostree_sack_get_by_pkgname (self->rsack->sack, pkgname.c_str (), &pkg, error))
        return FALSE;

      if (!pkg)
        g_ptr_array_add (inactive_removals, (gpointer)pkgname.c_str ());
    }

  if (inactive_removals->len > 0)
    {
      rpmostree_output_message ("Inactive base removals:");
      for (guint i = 0; i < inactive_removals->len; i++)
        {
          const char *item = (const char *)inactive_removals->pdata[i];
          rpmostree_output_message ("  %s", item);
          rpmostree_origin_remove_override_remove (self->computed_origin, item);
        }
    }

  return TRUE;
}

static gboolean
finalize_replacement_overrides (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable,
                                GError **error)
{
  g_assert (self->rsack);

  auto local_replacements = rpmostree_origin_get_overrides_local_replace (self->computed_origin);
  auto remote_replacements = rpmostree_origin_get_overrides_replace (self->computed_origin);
  g_autoptr (GPtrArray) inactive_replacements = g_ptr_array_new ();

  for (auto &nevra_v : local_replacements)
    {
      const char *nevra = nevra_v.c_str ();
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_autofree char *pkgname = NULL;
      if (!rpmostree_decompose_nevra (nevra, &pkgname, NULL, NULL, NULL, NULL, error))
        return FALSE;

      g_autoptr (DnfPackage) pkg = NULL;
      if (!rpmostree_sack_get_by_pkgname (self->rsack->sack, pkgname, &pkg, error))
        return FALSE;

      /* make inactive if it's missing or if that exact nevra is already present */
      if (!pkg || rpmostree_sack_has_subject (self->rsack->sack, nevra))
        g_ptr_array_add (inactive_replacements, (gpointer)nevra);
    }

  for (auto &ovr : remote_replacements)
    {
      for (auto &pkgname_s : ovr.packages)
        {
          const char *pkgname = pkgname_s.c_str ();
          /* make inactive if it's missing */
          if (!rpmostree_sack_has_pkgname (self->rsack->sack, pkgname))
            g_ptr_array_add (inactive_replacements, (gpointer)pkgname);
        }
    }

  if (inactive_replacements->len > 0)
    {
      rpmostree_output_message ("Inactive base replacements:");
      for (guint i = 0; i < inactive_replacements->len; i++)
        {
          const char *item = (const char *)inactive_replacements->pdata[i];
          rpmostree_output_message ("  %s", item);
          rpmostree_origin_remove_override_replace_local (self->computed_origin, item);
          rpmostree_origin_remove_override_replace (self->computed_origin, item);
        }
    }

  return TRUE;
}

static gboolean
finalize_overrides (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  return finalize_removal_overrides (self, cancellable, error)
         && finalize_replacement_overrides (self, cancellable, error);
}

static gboolean
add_local_pkgset_to_sack (RpmOstreeSysrootUpgrader *self, rust::Vec<rust::String> &pkgset,
                          GCancellable *cancellable, GError **error)
{
  if (pkgset.empty ())
    return TRUE; /* nothing to do! */

  if (!initialize_metatmpdir (self, error))
    return FALSE;

  for (auto &nevra_v : pkgset)
    {
      const char *nevra = nevra_v.c_str ();
      g_autofree char *sha256 = NULL;
      if (!rpmostree_decompose_sha256_nevra (&nevra, &sha256, error))
        return FALSE;

      g_autoptr (GVariant) header = NULL;
      g_autofree char *path = g_strdup_printf ("%s/%s.rpm", self->metatmpdir.path, nevra);

      if (!rpmostree_pkgcache_find_pkg_header (self->repo, nevra, sha256, &header, cancellable,
                                               error))
        return FALSE;

      if (!glnx_file_replace_contents_at (
              AT_FDCWD, path, static_cast<const guint8 *> (g_variant_get_data (header)),
              g_variant_get_size (header), GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
        return FALSE;

      /* Also check if that exact NEVRA is already in the root (if the pkg
       * exists, but is a different EVR, depsolve will catch that). In the
       * future, we'll allow packages to replace base pkgs. */
      if (rpmostree_sack_has_subject (self->rsack->sack, nevra))
        return glnx_throw (error, "Package '%s' is already in the base", nevra);

      dnf_sack_add_cmdline_package (self->rsack->sack, path);
    }

  return TRUE;
}

/* Go through rpmdb and jot down the missing pkgs from the given set. Really, we
 * don't *have* to do this: we could just give everything to libdnf and let it
 * figure out what is already installed. The advantage of doing it ourselves is
 * that we can optimize for the case where no packages are missing and we don't
 * have to fetch metadata. Another advantage is that we can make sure that the
 * embedded treespec only contains the provides we *actually* layer, which is
 * needed for displaying to the user (though we could fetch that info from
 * libdnf after resolution).
 * */
static gboolean
finalize_overlays (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  g_assert (self->rsack);

  /* request --> providing nevra */
  g_autoptr (GHashTable) inactive_requests
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* Add the local pkgs as if they were installed: since they're unconditionally
   * layered, we treat them as part of the base wrt regular requested pkgs. E.g.
   * you can have foo-1.0-1.x86_64 layered, and foo or /usr/bin/foo as dormant.
   * */
  auto local_pkgs = rpmostree_origin_get_local_packages (self->computed_origin);
  if (!add_local_pkgset_to_sack (self, local_pkgs, cancellable, error))
    return FALSE;

  auto local_fileoverride_pkgs
      = rpmostree_origin_get_local_fileoverride_packages (self->computed_origin);
  if (!add_local_pkgset_to_sack (self, local_fileoverride_pkgs, cancellable, error))
    return FALSE;

  /* check for each package if we have a provides or a path match */
  for (auto &pattern : rpmostree_origin_get_packages (self->computed_origin))
    {
      g_autoptr (GPtrArray) matches
          = rpmostree_get_matching_packages (self->rsack->sack, pattern.c_str ());

      if (matches->len == 0)
        {
          /* no matches, so we'll need to layer it (i.e. not remove it from the
           * computed origin) */
          continue;
        }

      /* Error out if it matches a base package that was also requested to be removed.
       * Conceptually, we want users to use override replacements, not remove+overlay.
       * Really, we could just not do this check; it'd just end up being dormant, though
       * that might be confusing to users. */
      for (guint i = 0; i < matches->len; i++)
        {
          auto pkg = static_cast<DnfPackage *> (matches->pdata[i]);
          const char *name = dnf_package_get_name (pkg);
          const char *repo = dnf_package_get_reponame (pkg);

          if (g_strcmp0 (repo, HY_CMDLINE_REPO_NAME) == 0)
            continue; /* local RPM added up above */

          if (rpmostree_origin_has_overrides_remove_name (self->computed_origin, name))
            return glnx_throw (error, "Cannot request '%s' provided by removed package '%s'",
                               pattern.c_str (), dnf_package_get_nevra (pkg));
        }

      /* Otherwise, it's an inactive request: remember them so we can print a nice notice.
       * Just use the first package as the "providing" pkg. */
      const char *providing_nevra
          = dnf_package_get_nevra (static_cast<DnfPackage *> (matches->pdata[0]));
      g_hash_table_insert (inactive_requests, g_strdup (pattern.c_str ()),
                           g_strdup (providing_nevra));
    }

  /* XXX: Currently, we don't retain information about which modules were
   * installed in the commit metadata. So we can't really detect "inactive"
   * requests. See related discussions in
   * https://github.com/rpm-software-management/libdnf/pull/1207.
   *
   * In the future, here we could extract e.g. the NSVCAPs from the base
   * commit, and mark as inactive all the requests for module names (and
   * optionally streams) already installed.
   *
   * For now, we just implicitly pass through via computed_origin.
   */

  if (g_hash_table_size (inactive_requests) > 0)
    {
      gboolean changed = FALSE;
      rpmostree_output_message ("Inactive requests:");
      GLNX_HASH_TABLE_FOREACH_KV (inactive_requests, const char *, req, const char *, nevra)
        {
          const char *pkgs[] = { req, NULL };
          g_autoptr (GError) local_error = NULL;
          rpmostree_output_message ("  %s (already provided by %s)", req, nevra);
          if (!rpmostree_origin_remove_packages (self->computed_origin,
                                                 util::rust_stringvec_from_strv ((char **)pkgs),
                                                 FALSE, &changed, &local_error))
            g_error ("%s", local_error->message);
          g_assert (changed);
          changed = FALSE;
        }
    }

  return TRUE;
}

static gboolean
prepare_context_for_assembly (RpmOstreeSysrootUpgrader *self, const char *tmprootfs,
                              GCancellable *cancellable, GError **error)
{
  /* make sure yum repos and passwd used are from our cfg merge */
  rpmostree_context_configure_from_deployment (self->ctx, self->sysroot,
                                               self->cfg_merge_deployment);

  /* load the sepolicy to use during import */
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, &sepolicy, cancellable, error))
    return FALSE;

  rpmostree_context_set_sepolicy (self->ctx, sepolicy);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY)
    rpmostree_context_set_pkgcache_only (self->ctx, TRUE);

  return TRUE;
}

/* Initialize libdnf context from our configuration */
static gboolean
prep_local_assembly (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  g_assert (!self->ctx);

  /* before doing any serious work; do some basic sanity checks that the origin is valid */

  /* If initramfs regeneration is enabled, it's silly to support /etc overlays on top of
   * that. Just point users at dracut's -I instead. I guess we could auto-convert
   * ourselves? */
  if (rpmostree_origin_get_regenerate_initramfs (self->computed_origin)
      && rpmostree_origin_has_initramfs_etc_files (self->computed_origin))
    return glnx_throw (
        error, "initramfs regeneration and /etc overlay not compatible; use dracut arg -I instead");

  if (!checkout_base_tree (self, cancellable, error))
    return FALSE;

  self->ctx = rpmostree_context_new_client (self->repo);

  g_autofree char *tmprootfs_abspath = glnx_fdrel_abspath (self->tmprootfs_dfd, ".");

  if (!prepare_context_for_assembly (self, tmprootfs_abspath, cancellable, error))
    return FALSE;

  {
    g_autoptr (GKeyFile) computed_origin_kf = rpmostree_origin_dup_keyfile (self->computed_origin);
    CXX_TRY_VAR (tf, rpmostreecxx::origin_to_treefile (*computed_origin_kf), error);
    self->treefile = std::move (tf);
  }
  rpmostree_context_set_treefile (self->ctx, **self->treefile);

  if (!rpmostree_context_setup (self->ctx, tmprootfs_abspath, tmprootfs_abspath, cancellable,
                                error))
    return FALSE;

  if (rpmostree_origin_has_any_packages (self->computed_origin))
    {
      if (!rpmostree_context_prepare (self->ctx, cancellable, error))
        return FALSE;
      self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS;

      /* keep a ref on it in case the level higher up needs it */
      self->rpmmd_sack
          = (DnfSack *)g_object_ref (dnf_context_get_sack (rpmostree_context_get_dnf (self->ctx)));
    }
  else
    {
      /* We still want to prepare() even if there's only enabled modules to validate.
       * See comment in rpmostree_origin_may_require_local_assembly(). */
      if (rpmostree_origin_has_modules_enable (self->computed_origin))
        {
          if (!rpmostree_context_prepare (self->ctx, cancellable, error))
            return FALSE;
        }
      rpmostree_context_set_is_empty (self->ctx);
      self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_LOCAL;
    }

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      if (rpmostree_origin_has_any_packages (self->computed_origin))
        rpmostree_print_transaction (rpmostree_context_get_dnf (self->ctx));
    }

  /* If the current state has layering, compare the depsolved set for changes. */
  if (self->final_revision)
    {
      g_autoptr (GVariant) prev_commit = NULL;

      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT, self->final_revision,
                                     &prev_commit, error))
        return FALSE;

      g_autoptr (GVariant) metadata = g_variant_get_child_value (prev_commit, 0);
      g_autoptr (GVariantDict) metadata_dict = g_variant_dict_new (metadata);
      g_autoptr (GVariant) previous_sha512_v = _rpmostree_vardict_lookup_value_required (
          metadata_dict, "rpmostree.state-sha512", (GVariantType *)"s", error);
      if (!previous_sha512_v)
        return FALSE;
      const char *previous_state_sha512 = g_variant_get_string (previous_sha512_v, NULL);
      g_autofree char *new_state_sha512
          = rpmostree_context_get_state_digest (self->ctx, G_CHECKSUM_SHA512, error);
      if (!new_state_sha512)
        return FALSE;

      self->layering_changed = strcmp (previous_state_sha512, new_state_sha512) != 0;
    }
  else
    /* Otherwise, we're transitioning from not-layered to layered, so it
       definitely changed */
    self->layering_changed = TRUE;

  return TRUE;
}

/* Overlay pkgs, run scripts, and commit final rootfs to ostree */
static gboolean
perform_local_assembly (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable, GError **error)
{
  g_assert (self->layering_initialized);
  g_assert (self->pkgs_imported);

  /* If we computed no layering is required, we're done */
  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE)
    return TRUE;

  /* this should've been checked by rpmostree_sysroot_upgrader_prep_layering */
  g_assert (!rpmostreed_get_lock_layering (rpmostreed_daemon_get ()));

  rpmostree_context_set_devino_cache (self->ctx, self->devino_cache);
  rpmostree_context_set_tmprootfs_dfd (self->ctx, self->tmprootfs_dfd);

  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS)
    {
      g_clear_pointer (&self->final_revision, g_free);

      /* --- override/overlay --- */
      if (!rpmostree_context_assemble (self->ctx, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!rpmostree_context_assemble_end (self->ctx, cancellable, error))
        return FALSE;
    }

  if (!rpmostree_rootfs_postprocess_common (self->tmprootfs_dfd, cancellable, error))
    return FALSE;

  /* If either the kernel or the initramfs config changed,
   * we need to load all of the kernel state.
   */
  const gboolean kernel_or_initramfs_changed
      = rpmostree_context_get_kernel_changed (self->ctx)
        || rpmostree_origin_get_regenerate_initramfs (self->computed_origin);
  g_autoptr (GVariant) kernel_state = NULL;
  g_autoptr (GPtrArray) initramfs_args = g_ptr_array_new_with_free_func (g_free);
  const char *bootdir = NULL;
  const char *kver = NULL;
  const char *kernel_path = NULL;
  const char *initramfs_path = NULL;
  if (kernel_or_initramfs_changed)
    {
      kernel_state = rpmostree_find_kernel (self->tmprootfs_dfd, cancellable, error);
      if (!kernel_state)
        return FALSE;

      /* note we extract the initramfs path here but we only use it as a fallback to use
       * with `--rebuild` if we're missing `initramfs-args` in the commit metadata -- in the
       * future, we could remove this entirely */
      g_variant_get (kernel_state, "(&s&s&sm&s)", &kver, &bootdir, &kernel_path, &initramfs_path);

      g_assert (self->base_revision);
      g_autoptr (GVariant) base_commit = NULL;
      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT, self->base_revision,
                                     &base_commit, error))
        return FALSE;

      g_autoptr (GVariant) metadata = g_variant_get_child_value (base_commit, 0);
      g_autoptr (GVariantDict) metadata_dict = g_variant_dict_new (metadata);

      g_autofree char **args = NULL;
      if (g_variant_dict_lookup (metadata_dict, "rpmostree.initramfs-args", "^a&s", &args))
        {
          initramfs_path = NULL; /* we got the canonical args, so don't use --rebuild */
          for (char **it = args; it && *it; it++)
            g_ptr_array_add (initramfs_args, g_strdup (*it));
        }
      else
        {
          /* If no initramfs-args were specified on the server side, for backwards compatibility
           * let's still assume `--no-hostonly`; losing that will break things because
           * we intentionally don't leak the system state to dracut.
           * See https://discussion.fedoraproject.org/t/downgrading-kernel-on-silverblue/1928
           * https://github.com/coreos/rpm-ostree/issues/2343
           */
          g_ptr_array_add (initramfs_args, g_strdup ("--no-hostonly"));
        }
    }

  /* If *just* the kernel changed, all we need to do is run depmod here.
   * see also process_kernel_and_initramfs() in the postprocess code
   * for server-side assembly.
   */
  if (rpmostree_context_get_kernel_changed (self->ctx))
    {
      g_assert (kernel_state && kver);
      ROSCXX_TRY (run_depmod (self->tmprootfs_dfd, kver, true), error);
    }

  if (kernel_or_initramfs_changed)
    {
      /* append the extra args */
      rust::Vec<rust::String> add_dracut_argv;
      if (rpmostree_origin_get_regenerate_initramfs (self->computed_origin))
        /* Note this option is deprecated, but we still read it for now. See
         * https://github.com/coreos/rpm-ostree/issues/3799. */
        add_dracut_argv = rpmostree_origin_get_initramfs_args (self->computed_origin);
      for (auto &arg : add_dracut_argv)
        g_ptr_array_add (initramfs_args, g_strdup (arg.c_str ()));
      g_ptr_array_add (initramfs_args, NULL);

      auto task = rpmostreecxx::progress_begin_task ("Generating initramfs");

      g_assert (kernel_state && kernel_path);

      g_auto (GLnxTmpfile) initramfs_tmpf = {
        0,
      };
      /* NB: We only use the real root's /etc if initramfs regeneration is explicitly
       * requested. IOW, just replacing the kernel still gets use stock settings, like the
       * server side. */
      if (!rpmostree_run_dracut (self->tmprootfs_dfd, (const char *const *)initramfs_args->pdata,
                                 kver, initramfs_path,
                                 rpmostree_origin_get_regenerate_initramfs (self->computed_origin),
                                 NULL, &initramfs_tmpf, cancellable, error))
        return FALSE;

      if (!rpmostree_finalize_kernel (self->tmprootfs_dfd, bootdir, kver, kernel_path,
                                      &initramfs_tmpf, RPMOSTREE_FINALIZE_KERNEL_AUTO, cancellable,
                                      error))
        return glnx_prefix_error (error, "Finalizing kernel");
    }

  if (!rpmostree_context_commit (self->ctx, self->base_revision,
                                 RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING, &self->final_revision,
                                 cancellable, error))
    return glnx_prefix_error (error, "Committing");

  /* Ensure we aren't holding any references to the tmpdir now that we're done;
   * rpmostree_sysroot_upgrader_deploy() eventually calls
   * rpmostree_syscore_cleanup() which deletes 🗑 the tmpdir.  See also similar
   * bits in the compose and container path.
   */
  g_clear_object (&self->ctx);
  glnx_close_fd (&self->tmprootfs_dfd);

  return TRUE;
}

/* Determine whether or not we require local modifications,
 * and if so, prepare layering (download rpm-md, depsolve etc).
 */
gboolean
rpmostree_sysroot_upgrader_prep_layering (RpmOstreeSysrootUpgrader *self,
                                          RpmOstreeSysrootUpgraderLayeringType *out_layering,
                                          gboolean *out_changed, GCancellable *cancellable,
                                          GError **error)
{
  /* Default to no assembly required, and not changed */
  self->layering_initialized = TRUE;
  self->layering_type = RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE;
  *out_layering = self->layering_type;
  *out_changed = FALSE;

  if (!rpmostree_origin_may_require_local_assembly (self->computed_origin))
    {
      g_clear_pointer (&self->final_revision, g_free);
      /* No assembly? We're done then */
      return TRUE;
    }

  if (rpmostreed_get_lock_layering (rpmostreed_daemon_get ()))
    return glnx_throw (error, "Cannot operate; `LockLayering=true` in configuration.");

  /* Do a bit more work to see whether or not we have to do assembly */
  if (!load_base_rsack (self, cancellable, error))
    return FALSE;
  /* XXX: initialize context earlier and lower this into the core instead */
  if (!finalize_overrides (self, cancellable, error))
    return FALSE;
  if (!finalize_overlays (self, cancellable, error))
    return FALSE;

  /* Now, it's possible all requested packages are in the new tree, so we have
   * another optimization here for that case. This is a bit tricky: assuming we
   * came here from an 'rpm-ostree install', this might mean that we redeploy
   * the exact same base layer, with the only difference being the origin file.
   * We could down the line experiment with optimizing this by just updating the
   * merge deployment's origin.
   * https://github.com/projectatomic/rpm-ostree/issues/753
   */
  if (!rpmostree_origin_may_require_local_assembly (self->computed_origin))
    {
      g_clear_pointer (&self->final_revision, g_free);
      /* No assembly? We're done then */
      return TRUE;
    }

  /* Actually do the prep work for local assembly */
  if (!prep_local_assembly (self, cancellable, error))
    return FALSE;

  *out_layering = self->layering_type;
  *out_changed = self->layering_changed;
  return TRUE;
}

gboolean
rpmostree_sysroot_upgrader_import_pkgs (RpmOstreeSysrootUpgrader *self, GCancellable *cancellable,
                                        GError **error)
{
  g_assert (self->layering_initialized);
  g_assert (!self->pkgs_imported);
  self->pkgs_imported = TRUE;

  /* any layering actually required? */
  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_NONE)
    return TRUE;

  if (self->layering_type == RPMOSTREE_SYSROOT_UPGRADER_LAYERING_RPMMD_REPOS)
    {
      if (!rpmostree_context_download (self->ctx, cancellable, error))
        return FALSE;
      if (!rpmostree_context_import (self->ctx, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * rpmostree_sysroot_upgrader_set_kargs:
 * @self: Self
 * @kernel_args: A list of strings representing kernel_arguments
 *
 * Set the active kargs to be used during deployment.
 */
void
rpmostree_sysroot_upgrader_set_kargs (RpmOstreeSysrootUpgrader *self, char **kernel_args)
{
  g_clear_pointer (&self->kargs_strv, g_strfreev);
  self->kargs_strv = g_strdupv (kernel_args);
}

static gboolean
write_history (RpmOstreeSysrootUpgrader *self, OstreeDeployment *new_deployment,
               GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) deployment_variant = NULL;
  if (!rpmostreed_deployment_generate_variant (self->sysroot, new_deployment, NULL, self->repo,
                                               FALSE, &deployment_variant, error))
    return FALSE;

  g_autofree char *deployment_dirpath
      = ostree_sysroot_get_deployment_dirpath (self->sysroot, new_deployment);
  struct stat stbuf;
  if (!glnx_fstatat (ostree_sysroot_get_fd (self->sysroot), deployment_dirpath, &stbuf, 0, error))
    return FALSE;

  g_autofree char *fn = g_strdup_printf ("%s/%ld", RPMOSTREE_HISTORY_DIR, stbuf.st_ctime);
  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, RPMOSTREE_HISTORY_DIR, 0775, cancellable, error))
    return FALSE;

  /* Write out GVariant to a file. One obvious question here is: why not keep this in the
   * journal itself since it supports binary data? We *could* do this, and it would simplify
   * querying and pruning, but IMO I find binary data in journal messages not appealing and
   * it breaks the expectation that journal messages should be somewhat easily
   * introspectable. We could also serialize it to JSON first, though we wouldn't be able to
   * re-use the printing code in `status.c` as is. Note also the GVariant can be large (e.g.
   * we include the full `rpmostree.rpmdb.pkglist` in there). */

  if (!glnx_file_replace_contents_at (
          AT_FDCWD, fn, static_cast<const guint8 *> (g_variant_get_data (deployment_variant)),
          g_variant_get_size (deployment_variant), static_cast<GLnxFileReplaceFlags> (0),
          cancellable, error))
    return FALSE;

  g_autofree char *version = NULL;
  {
    g_autoptr (GVariant) commit = NULL;
    if (!ostree_repo_load_commit (self->repo, ostree_deployment_get_csum (new_deployment), &commit,
                                  NULL, error))
      return FALSE;
    version = rpmostree_checksum_version (commit);
  }

  auto r = rpmostree_origin_get_refspec (self->computed_origin);
  sd_journal_send (
      "MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL (RPMOSTREE_NEW_DEPLOYMENT_MSG),
      "MESSAGE=Created new deployment /%s", deployment_dirpath, "DEPLOYMENT_PATH=/%s",
      deployment_dirpath, "DEPLOYMENT_TIMESTAMP=%" PRIu64, (uint64_t)stbuf.st_ctime,
      "DEPLOYMENT_DEVICE=%" PRIu64, (uint64_t)stbuf.st_dev, "DEPLOYMENT_INODE=%" PRIu64,
      (uint64_t)stbuf.st_ino, "DEPLOYMENT_CHECKSUM=%s", ostree_deployment_get_csum (new_deployment),
      "DEPLOYMENT_REFSPEC=%s", r.refspec.c_str (),
      /* we could use iovecs here and sd_journal_sendv to make these truly
       * conditional, but meh, empty field works fine too */
      "DEPLOYMENT_VERSION=%s", version ?: "", "COMMAND_LINE=%s", self->command_line ?: "",
      "AGENT=%s", self->agent ?: "", "AGENT_SD_UNIT=%s", self->sd_unit ?: "", NULL);

  return TRUE;
}

/**
 * rpmostree_sysroot_upgrader_deploy:
 * @self: Self
 * @initiating_command_line (nullable): command-line that initiated the deployment
 * @out_deployment: (out) (optional): return location for new deployment
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write the new deployment to disk, overlay any packages requested, perform a
 * configuration merge with /etc, and update the bootloader configuration.
 */
gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader *self,
                                   OstreeDeployment **out_deployment, GCancellable *cancellable,
                                   GError **error)
{
  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      /* we already printed the transaction in do_final_local_assembly() */
      return TRUE;
    }

  /* Invoke prep_layering() if not done already */
  if (!self->layering_initialized)
    {
      RpmOstreeSysrootUpgraderLayeringType layering_type;
      gboolean layering_changed = FALSE;
      if (!rpmostree_sysroot_upgrader_prep_layering (self, &layering_type, &layering_changed,
                                                     cancellable, error))
        return FALSE;
    }

  if (!self->pkgs_imported)
    {
      if (!rpmostree_sysroot_upgrader_import_pkgs (self, cancellable, error))
        return FALSE;
    }

  /* Generate the final ostree commit */
  if (!perform_local_assembly (self, cancellable, error))
    return FALSE;

  /* make sure we have a known target to deploy */
  const char *target_revision = self->final_revision ?: self->base_revision;
  g_assert (target_revision);

  /* Use staging only if we're booted into the target root. */
  const gboolean use_staging = (ostree_sysroot_get_booted_deployment (self->sysroot) != NULL);

  /* Fix for https://github.com/projectatomic/rpm-ostree/issues/1392,
   * when kargs_strv is empty, we port those directly from pending
   * deployment if there is one */
  if (!self->kargs_strv)
    {
      OstreeBootconfigParser *bootconfig
          = ostree_deployment_get_bootconfig (self->origin_merge_deployment);
      const char *options = ostree_bootconfig_parser_get (bootconfig, "options");
      self->kargs_strv = g_strsplit (options, " ", -1);
    }

  // Note that most of the code here operates on ->computed_origin except this.
  // We want to preserve all the original requests, for cases like inactive
  // layers/overrides.
  g_autoptr (GKeyFile) origin = rpmostree_origin_dup_keyfile (self->original_origin);
  g_autoptr (OstreeDeployment) new_deployment = NULL;

  g_autofree char *overlay_initrd_checksum = NULL;
  const char *overlay_v[] = { NULL, NULL };
  if (rpmostree_origin_has_initramfs_etc_files (self->computed_origin))
    {
      /* this should've been checked by rpmostree_sysroot_upgrader_prep_layering */
      g_assert (!rpmostreed_get_lock_layering (rpmostreed_daemon_get ()));
      auto etc_files = rpmostree_origin_get_initramfs_etc_files (self->computed_origin);
      CXX_TRY_VAR (fdv, rpmostreecxx::initramfs_overlay_generate (etc_files, *cancellable), error);
      glnx_fd_close int fd = fdv;
      if (!ostree_sysroot_stage_overlay_initrd (self->sysroot, fd, &overlay_initrd_checksum,
                                                cancellable, error))
        return glnx_prefix_error (error, "Staging initramfs overlay");

      overlay_v[0] = overlay_initrd_checksum;
    }

  OstreeSysrootDeployTreeOpts opts = {
    .override_kernel_argv = self->kargs_strv,
    .overlay_initrds = (char **)overlay_v,
  };

  if (use_staging)
    {
      /* touch file *before* we stage to avoid races */
      if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION)
        {
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD,
                                       dirname (strdupa (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED)),
                                       0755, cancellable, error))
            return FALSE;

          glnx_autofd int fd = open (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED,
                                     O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC, 0640);
          if (fd == -1)
            return glnx_throw_errno_prefix (error, "touch(%s)",
                                            _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED);
        }

      auto task = rpmostreecxx::progress_begin_task ("Staging deployment");
      if (!ostree_sysroot_stage_tree_with_options (self->sysroot, self->osname, target_revision,
                                                   origin, self->cfg_merge_deployment, &opts,
                                                   &new_deployment, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!ostree_sysroot_deploy_tree_with_options (self->sysroot, self->osname, target_revision,
                                                    origin, self->cfg_merge_deployment, &opts,
                                                    &new_deployment, cancellable, error))
        return FALSE;
    }

  if (!write_history (self, new_deployment, cancellable, error))
    return FALSE;

  /* Also do a sanitycheck even if there's no local mutation; it's basically free
   * and might save someone in the future.  The RPMOSTREE_SKIP_SANITYCHECK
   * environment variable is just used by test-basic.sh currently.
   *
   * Note that since the staging changes, this now operates without a
   * config-merged state.
   */
  if (!self->final_revision)
    {
      g_autofree char *deployment_path
          = ostree_sysroot_get_deployment_dirpath (self->sysroot, new_deployment);
      glnx_autofd int deployment_dfd = -1;
      if (!glnx_opendirat (ostree_sysroot_get_fd (self->sysroot), deployment_path, TRUE,
                           &deployment_dfd, error))
        return FALSE;

      if (!rpmostree_deployment_sanitycheck_true (deployment_dfd, cancellable, error))
        return FALSE;
    }
  else
    {
      /* Generate a temporary ref for the base revision in case we are
       * interrupted; the base layer refs generation isn't transactional.
       */
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                          self->base_revision, cancellable, error))
        return FALSE;
    }

  if (use_staging)
    {
      /* In the staging path, we just need to regenerate our baselayer refs and
       * do the prune.  The stage_tree() API above should have loaded our new deployment
       * into the set.
       */
      if (!rpmostree_syscore_cleanup (self->sysroot, self->repo, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!rpmostree_syscore_write_deployment (
              self->sysroot, new_deployment, self->cfg_merge_deployment, FALSE, cancellable, error))
        return FALSE;
    }

  if (out_deployment)
    *out_deployment = util::move_nullify (new_deployment);
  return TRUE;
}

GType
rpmostree_sysroot_upgrader_flags_get_type (void)
{
  static gsize static_g_define_type_id = 0;

  if (g_once_init_enter (&static_g_define_type_id))
    {
      static const GFlagsValue values[] = {
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED", "ignore-unconfigured" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER", "allow-older" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN, "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN",
          "dry-run" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGCACHE_ONLY", "pkgcache-only" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_SYNTHETIC_PULL", "synthetic-pull" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_LOCK_FINALIZATION", "lock-finalization" },
      };
      GType g_define_type_id = g_flags_register_static (
          g_intern_static_string ("RpmOstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&static_g_define_type_id, g_define_type_id);
    }

  return static_g_define_type_id;
}
