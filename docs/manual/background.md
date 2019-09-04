## Package systems versus image systems

Broadly speaking, software update systems for operating systems tend
to fall cleanly into one of two camps: package-based or image-based.

### Package system benefits and drawbacks

 * + Highly dynamic, fast access to wide array of software
 * + State management in `/etc` and `/var` is well understood 
 * + Can swap between major/minor system states (`apt-get upgrade` is similar to `apt-get dist-upgrade`)
 * + Generally supports any filesystem or partition layout
 * - As package set grows, testing becomes combinatorially more expensive
 * - Live system mutation, no rollbacks

### Image benefits and drawbacks

 * + Ensures all users are running a known state
 * + Rollback supported
 * + Easier to verify system integrity
 * - Many image systems have a read-only `/etc`, and writable partitions elsewhere
 * - Must reboot for updates
 * - Usually operate at block level, so require fixed partition layout and filesystem
 * - Many use a "dual root" mode which wastes space and is inflexible
 * - Often paired with a separate application mechanism, but misses out on things that aren't apps
 * - Administrators still need to know content inside

## How RPM-OSTree provides a middle ground

rpm-ostree in its default mode feels more like image replication, but
the underlying architecture allows a lot of package-like flexibility.

In this default mode, packages are composed on a server, and clients
can replicate that state reliably.  For example, if one adds a package
on the compose server, clients get it.  If one removes a package, it's
also removed when clients upgrade.

One simple mental model for rpm-ostree is: imagine taking a set of
packages on the server side, install them to a chroot, then doing `git commit`
on the result.  And imagine clients just `git pull -r` from
that.  What OSTree adds to this picture is support for file uid/gid,
extended attributes, handling of bootloader configuration, and merges
of `/etc`.

To emphasize, replication is at a filesystem level - that means things 
like SELinux labels and uid/gid mappings are assigned on
the server side.

On the other hand, rpm-ostree works on top of any Unix filesystem.  It
will not interfere with any filesystem or block-level snapshots or
backups such as LVM or BTRFS.
