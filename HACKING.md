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

On Fedora, you can install those with the command `dnf builddep rpm-ostree`.

So the build process now looks like any other autotools program:

```
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
make
```

At this point you can run some of the unit tests with `make check`.
For more information on this, see `CONTRIBUTING.md`.

Doing builds in a container
===================================

First, we recommend building in a container (for example `docker`); you can use
other container tools obviously.  See `ci/build.sh` for build and test
dependencies.

Testing
=========

You can use `make check` in a container to run the unit tests.  However,
if you want to test the daemon in a useful way, you'll need virtualization.

There's a `make vmcheck` test suite that requires a `ssh-config` in the
source directory toplevel.  You can provision a VM however you want; libvirt
directly, vagrant, a remote OpenStack/EC2 instance, etc.  If you choose
vagrant for example, do something like this:

```
vagrant ssh-config > /path/to/src/rpm-ostree/ssh-config
```

Once you have a `ssh-config` set up:

`make vmsync` will do an unlock, and sync the container build
into the VM.

`make vmoverlay` will do a non-live overlay, and reboot the VM.

For convenience, the `make vmshell` command does the same
as `make vmsync` but additionally places you in a shell,
ready to test your changes.

Note that by default, all the commands above try to re-use
the same configuration files to save speed. If you want to
force a cleanup, you can use `VMCLEAN=1`.
