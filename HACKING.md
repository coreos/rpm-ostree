Raw build instructions
----------------------

First, releases are available as GPG signed git tags, and most recent
versions support extended validation using
[git-evtag](https://github.com/cgwalters/git-evtag).

You'll need to get the submodules too: `git submodule update --init`

rpm-ostree has a hard requirement on a bleeding edge version of
[libhif](https://github.com/rpm-software-management/libhif/) - we now
consume this as a git submodule automatically.

We also require a few other libraries like
[librepo](https://github.com/rpm-software-management/librepo).

So the build process now looks like any other autotools program:

```
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
make
```

At this point you can run some of the unit tests with `make check`.
For more information on this, see `CONTRIBUTING.md`.

Using the Vagrant box
=====================

The easiest way to get started hacking on `rpm-ostree` is to
use the vagrant machine. This is also the set up used for
our integration tests.

One-time setup
==============

Starting the vagrant machine is as easy as:

```
host$ vagrant up
host$ vagrant ssh
```

The VM for now uses the official
[centos/atomic-host](https://atlas.hashicorp.com/centos/boxes/atomic-host)
box. However, because `rpm-ostree` is tightly coupled with
other projects such as `ostree` and `libhif`, the code in
`HEAD` may require a more recent version than available in
the latest official box.

For this reason, you may want to rebase the VM onto the
[CentOS Atomic Continuous](https://ci.centos.org/job/atomic-rdgo-centos7/)
stream, which will contain the latest `HEAD` versions of
these dependencies (normally within the hour). To rebase,
simply do:

```
vm$ sudo rpm-ostree rebase centos-atomic-continuous:centos-atomic-host/7/x86_64/devel/continuous
vm$ sudo systemctl reboot
```

If you need to test your code with custom `ostree` or
`libhif` builds, you have no choice for now other than
making your own tree (and yum repo for use by the build
container). We're hoping to improve this workflow soon.

Hacking
=======

The `make vmoverlay` command will automatically sync the
current files to the VM, unlock the current deployment,
build `rpm-ostree`, and install it.

If you need to test on a locked deployment with the updated
`rpm-ostree` baked into the tree, you can use the `make
vmbuild` command, which will install `rpm-ostree` into a new
deployment and reboot the VM to use it.

For convenience, the `make vmshell` command does the same
as `make vmbuild` but additionally places you in a shell,
ready to test your changes.

Note that by default, all the commands above try to re-use
the same configuration files to save speed. If you want to
force a cleanup, you can use `VMCLEAN=1`.

Testing
=======

The `make vmcheck` command performs the same task as `make
vmbuild`, but additionally starts the integration testsuite.
