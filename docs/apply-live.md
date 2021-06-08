---
parent: Architecture
nav_order: 3
---

# Architecture of apply-live
{: .no_toc }

1. TOC
{:toc}

## Copying into an "underlay"

As noted in the architecture doc, everything in rpm-ostree is oriented
around creating and managing hardlinked complete bootable filesystem trees.

In this flow then, `rpm-ostree install --apply-live strace` will first
create a new pending deployment, run sanity tests on it, prepare it to be booted, etc.

However, the first time `apply-live` is invoked, we create an `overlayfs`
mount over `/usr`.  It's mounted `ro` from the perspective of the rest
of the system, but rpm-ostree can write to it.

## Package and filesystem diffs

When `apply-live` is invoked, rpm-ostree computes the diff between
the source and target OSTree commit for `/usr`.  If this is the *first* `apply-live`,
the source commit is the booted commit.  For subsequent invocations,
it will be based on the current live commit.

We also compute a package-level diff; this is how `apply-live`
currently distinguishes between pure package additions versus upgrades.

## Copying data for /usr

Per the core OSTree model, almost everything we care about is in `/usr`.
So the first step is to apply the diff to the transient writable `overlayfs`.

One downside is that that this diff will take extra memory and disk space
proportional to its size.

## Updating /etc

The second aspect we need to take care of is `/etc`.  Normally, the libostree
core handles the `/etc` merge during shutdown as part of `ostree-finalize-staged.service`,
but we need to do it now in order to ensure that we get new config files
(or remove ones).

Note that the changes in `/etc` are persistent, live-applied changes there are
also hence not updated transactionally.  It is hence possible for configuration
files to "leak" from partially applied live updates.

## Updating /var

Normally, libostree core never touches `/var`.  Today rpm-ostree generates
`systemd-tmpfiles` snippets for RPM packages which contain directories in
`/var`.  In a regular update, these will hence be generated at boot
time by `systemd-tmpfiles-setup.service`.

But here, we need to do this live.  So rpm-ostree directly starts a
transient systemd unit running `systemd-tmpfiles`.

## Tracking live state

Because the `overlayfs` is transient (goes away on reboot), the `apply-live`
operation also writes its state into the transient `/run` directory, specifically
a stamp file is stored at `/run/ostree/deployment-state/$deployid/`.

Currently, there is also a persistent ostree ref `rpmostree/live-apply` for
the current live commit.  Eventually the goal is that libostree itself would
gain direct awareness of live apply, and we wouldn't write a persistent ref.


