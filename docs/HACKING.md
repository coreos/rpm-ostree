---
nav_order: 6
---

# Hacking on rpm-ostree
{: .no_toc }

1. TOC
{:toc}

## Raw build instructions

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

```sh
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
make
```

At this point you can run some of the unit tests with `make check`.
For more information on this, see `CONTRIBUTING.md`.

## Doing builds in a container

First, we recommend building in a container (for example `docker`); you can use
other container tools obviously.  See `ci/build.sh` for build and test
dependencies.

## Testing

You can use `make check` in a container to run the unit tests.  However,
if you want to test the daemon in a useful way, you'll need virtualization.

rpm-ostree has some tests that use the [coreos-assembler/kola framework](https://github.com/coreos/coreos-assembler/blob/94602e26678fd1a8fa3bda37b3b1d980967be2d6/mantle/kola/README-kola-ext.md).

You will want to [build a custom image](https://github.com/coreos/coreos-assembler/blob/94602e26678fd1a8fa3bda37b3b1d980967be2d6/README-devel.md#using-overrides) and use `kola run -E /path/to/rpm-ostree.git ext.rpm-ostree.*'.

There's also a `make vmcheck` test suite that requires a `ssh-config` in the
source directory toplevel.  You can provision a VM however you want; libvirt
directly, vagrant, a remote OpenStack/EC2 instance, etc.  If you choose
vagrant for example, do something like this:

```sh
vagrant ssh-config > /path/to/src/rpm-ostree/ssh-config
```

The host is expected to be called `vmcheck` in the
`ssh-config`. You can specify multiple hosts and parallelize
the `make vmcheck` testsuite run through the `HOSTS`
variable. For example, if you have three nodes named
`vmcheck[123]`, you can use:

```sh
make vmcheck HOSTS='vmcheck1 vmcheck2 vmcheck3'
```

Once you have a `ssh-config` set up:

`make vmsync` will do an unlock, and sync the container build
into the VM.

`make vmoverlay` will do a non-live overlay, and reboot the VM.

Note that by default, these commands will retrieve the latest version of ostree
from the build environment and include those binaries when syncing to the VM.

Ideally, you should be installing `ostree` from streams like
[FAHC](https://pagure.io/fedora-atomic-host-continuous/) and
[CAHC](https://wiki.centos.org/SpecialInterestGroup/Atomic/Devel), which closely
track ostree's git master. This allows you to not have to worry about using
libostree APIs that are not yet released.

For more details on how tests are structured, see [tests/README.md](tests/README.md).

## Testing with a custom libdnf

rpm-ostree bundles libdnf since commit https://github.com/coreos/rpm-ostree/commit/125c482b1d16ce8376378f220fc2f93a5b157bc1
the rationale is:

 - libdnf broke ABI several times silently in the past
 - Today, dnf does not actually *use* libdnf much, which means
   for the most part any libdnf breakage is first taken by us
 - libdnf is trying to rewrite more in C++, which is unlikely to help
   API/ABI stability
 - dnf and rpm-ostree release on separate cycles (e.g. today rpm-ostree
   is used by OpenShift)

In general, until libdnf is defined 100% API/ABI stable, we will
continue to bundle it.

However, because it's a git submodule, it's easy to test updates
to it, and it also means we're not *forking* it.

So just do e.g.:
```
cd libdnf
git fetch origin
git reset --hard origin/master
cd ..
```

The various `make` targets will pick up the changes and recompile.

## Testing with a custom ostree

It is sometimes necessary to develop against a version of ostree which is not
even yet in git master. In such situations, one can simply do:

```sh
$ # from the rpm-ostree build dir
$ INSTTREE=$PWD/insttree
$ rm -rf $INSTTREE
$ # from the ostree build dir
$ make
$ make install DESTDIR=$INSTTREE
$ # from the rpm-ostree build dir
$ make
$ make install DESTDIR=$INSTTREE
```

At this point, simply set `SKIP_INSTALL=1` when running `vmsync` and `vmoverlay`
to reuse the installation tree and sync the installed binaries there:

```sh
$ make vmsync SKIP_INSTALL=1
$ make vmoverlay SKIP_INSTALL=1
```

Of course, you can use this pattern for not just ostree but whatever else you'd
like to install into the VM (e.g. bubblewrap, libsolv, etc...).
