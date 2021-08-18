//! Implementation of the client-side of "rpm-ostree module".

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, bail, Result};
use gio::prelude::*;
use glib::Variant;
use ostree_ext::{gio, glib};
use structopt::StructOpt;

use crate::utils::print_treepkg_diff;

#[derive(Debug, StructOpt)]
#[structopt(name = "rpm-ostree module", no_version)]
#[structopt(rename_all = "kebab-case")]
enum Opt {
    /// Enable a module
    Enable(InstallOpts),
    /// Disable a module
    Disable(InstallOpts),
    /// Install a module
    Install(InstallOpts),
    /// Uninstall a module
    Uninstall(InstallOpts),
}

#[derive(Debug, StructOpt)]
struct InstallOpts {
    #[structopt(parse(from_str))]
    modules: Vec<String>,
    #[structopt(long)]
    reboot: bool,
    #[structopt(long)]
    lock_finalization: bool,
    #[structopt(long)]
    dry_run: bool,
}

const OPT_KEY_ENABLE_MODULES: &str = "enable-modules";
const OPT_KEY_DISABLE_MODULES: &str = "disable-modules";
const OPT_KEY_INSTALL_MODULES: &str = "install-modules";
const OPT_KEY_UNINSTALL_MODULES: &str = "uninstall-modules";

pub(crate) fn modularity_entrypoint(args: &Vec<String>) -> Result<()> {
    match Opt::from_iter(args.iter()) {
        Opt::Enable(ref opts) => enable(opts),
        Opt::Disable(ref opts) => disable(opts),
        Opt::Install(ref opts) => install(opts),
        Opt::Uninstall(ref opts) => uninstall(opts),
    }
}

// XXX: Should split out a lot of the below into a more generic Rust wrapper around
// UpdateDeployment() like we have on the C side.

fn get_modifiers_variant(key: &str, modules: &[String]) -> Result<glib::Variant> {
    let r = glib::VariantDict::new(None);
    r.insert_value(key, &modules.to_variant());
    Ok(r.end())
}

fn get_options_variant(opts: &InstallOpts) -> Result<glib::Variant> {
    let r = glib::VariantDict::new(None);
    r.insert("no-pull-base", &true);
    r.insert("reboot", &opts.reboot);
    r.insert("lock-finalization", &opts.lock_finalization);
    r.insert("dry-run", &opts.dry_run);
    Ok(r.end())
}

fn enable(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_ENABLE_MODULES, opts)
}

fn disable(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_DISABLE_MODULES, opts)
}

fn install(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_INSTALL_MODULES, opts)
}

fn uninstall(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_UNINSTALL_MODULES, opts)
}

fn modules_impl(key: &str, opts: &InstallOpts) -> Result<()> {
    if opts.modules.is_empty() {
        bail!("At least one module must be specified");
    }

    let client = &mut crate::client::ClientConnection::new()?;
    let previous_deployment = client
        .get_os_proxy()
        .cached_property("DefaultDeployment")
        .ok_or_else(|| anyhow!("Failed to find default-deployment property"))?;
    let modifiers = get_modifiers_variant(key, &opts.modules)?;
    let options = get_options_variant(opts)?;
    let params = Variant::from_tuple(&[modifiers, options]);
    let reply = &client.get_os_proxy().call_sync(
        "UpdateDeployment",
        Some(&params),
        gio::DBusCallFlags::NONE,
        -1,
        gio::NONE_CANCELLABLE,
    )?;
    let reply = reply
        .get::<(String,)>()
        .ok_or_else(|| anyhow!("Invalid reply"))?;
    client.transaction_connect_progress_sync(reply.0.as_str())?;
    if opts.dry_run {
        println!("Exiting because of '--dry-run' option");
    } else if !opts.reboot {
        let new_deployment = client
            .get_os_proxy()
            .cached_property("DefaultDeployment")
            .ok_or_else(|| anyhow!("Failed to find default-deployment property"))?;
        if previous_deployment != new_deployment {
            print_treepkg_diff("/");
        }
    }
    Ok(())
}
