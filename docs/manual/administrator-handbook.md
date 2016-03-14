## Administering an rpm-ostree based system

At the moment, there are four primary commands to be familiar with on
an `rpm-ostree` based system.  Also remember that in a Project Atomic
system, the `atomic host` command (from the
[Atomic command](https://github.com/projectatomic/atomic/)) is an
alias for `rpm-ostree`.  The author tends to use the former on client
systems, and the latter on compose servers.

```
   # atomic host status
```
Will show you your deployments, in the order in which they will appear
in the bootloader.  The `*` shows the currently booted deployment.

```
   # atomic host upgrade
```
Will perform a system upgrade, creating a *new* chroot, and set it as
the default for the next boot.  You should use `systemctl reboot`
shortly afterwards.

```
   # atomic host rollback
```
By default, the `atomic upgrade` will keep at most two bootable
"deployments", though the underlying technology supports more.

```
# atomic host deploy <version>
```
This command makes use of the server-side history feature of OSTree.
It will search the history of the current branch for a commit with the
specified version, and deploy it.  This can be used in scripts to
ensure consistent updates.  For example, if the upstream OS vendor
provides an update online, you might not want to deploy it until
you've tested it.  This helps ensure that when you upgrade, you are
getting exactly what you asked for.

## Filesystem layout

The only writable directories are `/etc` and `/var`.  In particular,
`/usr` has a read-only bind mount at all times.  Any data in `/var` is
never touched, and is shared across upgrades. 

At upgrade time, the process takes the *new default* `/etc`, and adds
your changes on top.  This means that upgrades will receive new
default files in `/etc`, which is quite a critical feature.

## Operating system changes

 * The RPM database is stored in `/usr/share/rpm`, and is immutable.
 * A package [nss-altfiles](https://github.com/aperezdc/nss-altfiles) is required,
   and the system password database is stored in `/usr/lib/passwd`.  Similar
   for the group database.
