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

## Package layering

It is possible to add more packages onto the system that are not part of
the commit composed on the server. These additional "layered" packages
are persistent across upgrades, rebases, and deploys (contrast with the
ostree [unlocking](https://github.com/ostreedev/ostree/blob/master/man/ostree-admin-unlock.xml)
mechanism). This allows you to easily enhance the base set of packages
on only some machines, or only temporarily (rather than asking to have
it part of the server compose and affecting every machine). For example,
you may wish to permanently install some diagnostics tools on a test
machine.

```
# rpm-ostree pkg-add <pkg>
```

Will download the target package, its dependencies, and create a new
deployment with those packages installed.

```
# rpm-ostree pkg-remove <pkg>
```

Will create a new deployment with the target package removed.

Note that package layering is currently in preview mode and as such may
change interface or functionality before being declared stable.

## Filesystem layout

The only writable directories are `/etc` and `/var`.  In particular,
`/usr` has a read-only bind mount at all times.  Any data in `/var` is
never touched, and is shared across upgrades. 

At upgrade time, the process takes the *new default* `/etc`, and adds
your changes on top.  This means that upgrades will receive new
default files in `/etc`, which is quite a critical feature.

For more information, see
[OSTree: Adapting](https://ostree.readthedocs.io/en/latest/manual/adapting-existing/).

## Operating system changes

 * The RPM database is stored in `/usr/share/rpm`, and is immutable.
 * A package [nss-altfiles](https://github.com/aperezdc/nss-altfiles)
   is required, and the system password database is stored in
   `/usr/lib/passwd`.  Similar for the group database.  This might
   change in the future; see
   [this issue](https://github.com/projectatomic/rpm-ostree/issues/49).
