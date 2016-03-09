## Administering an rpm-ostree based system

At the moment, there are three primary commands to be familiar with on
an rpm-ostree based system.  Remember that `atomic` is an alias for
`rpm-ostree`.  The author tends to use the former on client systems,
and the latter on compose servers.
```
   # atomic status
```
Will show you your deployments, in the order in which they will appear
in the bootloader.  The `*` shows the currently booted deployment.
```
   # atomic upgrade
```
Will perform a system upgrade, creating a *new* chroot, and set it as
the default for the next boot.  You should use `systemctl reboot`
shortly afterwards.
```
   # atomic rollback
```
By default, the `atomic upgrade` will keep at most two bootable
"deployments", though the underlying technology supports more.

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
