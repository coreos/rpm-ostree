## Administering an rpm-ostree based system

At the moment, there are two primary commands to be familiar with on
an rpm-ostree based system.  Remember that `atomic` is an alias for
`rpm-ostree`.  The author tends to use the former on client systems,
and the latter on compose servers.

   # atomic upgrade

Will perform a system upgrade, creating a *new* chroot, and set it as
the default for the next boot.  You should use `systemctl reboot`
shortly afterwards.

   # atomic rollback

By default, the `atomic upgrade` will keep at most two bootable
"deployments", though the underlying technology supports more.

## Filesystem layout

The only writable directories are `/etc` and `/var`.  In particular,
`/usr` has a read-only bind mount at all times.  Any data in `/var` is
never touched, and is shared across upgrades. 

At upgrade time, the process takes the *new default* `/etc`, and adds
your changes on top.  This means that upgrades will receive new
default files in `/etc`, which is quite a critical feature.
