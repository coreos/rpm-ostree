Using yum-ostree
================

This tool takes a set of packages, and commits them to an OSTree
repository.  At the moment, it is intended for use on build servers.
For example, this invocation:

	# yum-ostree --repo=repo --enablerepo=fedora --os=fedora --os-version=20 create my-server-packages @minimal kernel ostree fedora-release lvm2 e2fsprogs btrfs-progs passwd httpd

Will create a ref named "fedora/20/my-server-packages", containing a
complete bootable root filesystem of those packages.  As you can see
from the example above, the package set is quite minimal, we're just
adding on "httpd".

Now, if you export the repo via any plain HTTP server, clients using
OSTree can then replicate this tree, and boot it, tracking updates you
make over time.

Pulling and booting from a client machine
=========================================

First, install the ostree package, of course; make sure you have
ostree 2013.7 or newer.

	# yum install ostree

Now, this bit of one time initialization will both
create `/ostree` for you, as well as `/ostree/deploy/fedora`.

	# ostree admin os-init fedora

This step tells OSTree how to find the repository you built on
the server.  You only need to do this once.

	# ostree remote add myserver https://mycorp.example.com/repo

Now, since we did not GPG sign our repo above, we need to disable GPG
verification.  Add `gpg-verify=false` in the `[remote]` section.

	# nano /ostree/repo/config

This step downloads that ref into `/ostree/repo`:

	# ostree pull myserver fedora/20/my-server-packages

This step extracts the root filesystem, and updates the bootloader
configuration:

	# ostree admin deploy --os=fedora fedora/20/my-server-packages

We need to do some initial setup before we actually boot the system.
Copy in the storage configuration:

	# cp /etc/fstab /ostree/deploy/fedora/current

And set a root password:

	# chroot /ostree/deploy/fedora/current passwd

And there is one final (manual) step: You must copy your system's
kernel arguments from `/boot/grub2/grub.cfg` and add them to
`/boot/loader/entries/ostree-fedora-0.conf`, on the `options`
line. This step may be automated further in the future.

Booting the system
==================

Remember, at this point there is no impact on your installed system
except for additional disk space in the `/boot/loader` and `/ostree`
directories.

Reboot, and get a GRUB prompt.  At the prompt, press `c`.  Now, enter:

	insmod blscfg
	bls_import

Then press `Esc`.  You should have an additional boot menu entry,
named `ostree:fedora:0`.  Nagivate to it and press `Enter`.


Inside the system
=================

To upgrade, run as root:

# ostree admin upgrade

Note that in our demo so far, we did not install `yum` (or even
`rpm`).  Getting these to work fully is the next phase of the
`yum-ostree` development.
