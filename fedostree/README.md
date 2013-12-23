"fedostree"
===========

This project uses [rpm-ostree](https://github.com/cgwalters/rpm-ostree)
to commit multiple comps groups 

Pulling and booting from a client machine
-----------------------------------------

First, install the ostree package, of course; make sure you have
ostree 2013.7 or newer.

	yum install ostree

Now, this bit of one time initialization will both
create `/ostree` for you, as well as `/ostree/deploy/fedostree`.

	ostree admin os-init fedostree

This step tells OSTree how to find the repository you built on
the server.  You only need to do this once.

	ostree remote add fedostree http://http://209.132.184.226/repo

Now, since we did not GPG sign our repo above, we need to disable GPG
verification.  Add `gpg-verify=false` in the `[remote]` section.

	nano /ostree/repo/config

At this point, we have only initialized configuration.  Let's start
by downloading the "minimal" install (just @core):

	ostree pull fedostree fedora/20/minimal

This step extracts the root filesystem, and updates the bootloader
configuration:

	ostree admin deploy --os=fedostree fedora/20/minimal

We need to do some initial setup before we actually boot the system.
Copy in the storage configuration:

	cp /etc/fstab /ostree/deploy/fedostree/current

And set a root password:

	chroot /ostree/deploy/fedostree/current passwd

And there is one final (manual) step: You must copy your system's
kernel arguments from `/boot/grub2/grub.cfg` and add them to
`/boot/loader/entries/ostree-fedora-0.conf`, on the `options`
line. This step may be automated further in the future.

IMPORTANT NOTE: You must use selinux=0 for now.

Booting the system
------------------

Remember, at this point there is no impact on your installed system
except for additional disk space in the `/boot/loader` and `/ostree`
directories.

Reboot, and get a GRUB prompt.  At the prompt, press `c`.  Now, enter:

	insmod blscfg
	bls_import

Then press `Esc`.  You should have an additional boot menu entry,
named `ostree:fedora:0`.  Nagivate to it and press `Enter`.


Inside the system
-----------------

To upgrade, run as root:

	ostree admin upgrade

Note that in our demo so far, we did not install `yum` (or even
`rpm`).  Getting these to work fully is the next phase of the
`yum-ostree` development.

Switching trees
---------------

Remember, with OSTree, it's possible to atomically transition between
different complete bootable filesystem trees.  Let's now try the
"standard-docker-io" tree:

	ostree pull fedostree fedora/20/standard-docker-io

If you look at the [http://209.132.184.226/fedora-ostree-ci](script),
you can see this tree contains `@core`, `@standard`, and finally
`docker-io`.

Like above, let's now deploy it:

	ostree admin deploy --os=fedostree fedora/20/standard-docker-io
	systemctl reboot


