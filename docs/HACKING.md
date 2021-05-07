---
nav_order: 7
---

# Hacking on rpm-ostree
{: .no_toc }

1. TOC
{:toc}

## Building and testing in a container

The majority of developers on rpm-ostree build and test it
from a [toolbox container](https://github.com/containers/toolbox), separate from the host system.
The instructions below may *also* work when run in a traditional
login on a virtual machine, but are less frequently tested.

### Installing dependencies (cxx)

Today rpm-ostree uses [cxx.rs](https://cxx.rs/) - the CLI tools for
that aren't packaged in e.g. Fedora; we ship the pre-generated
source in the releases.  But to build from git you need to install the
tools.

```
$ ./ci/installdeps.sh
```

You will also need to rerun this after the dependency changes in our
`Cargo.lock`.  Eventually we will fix this.

### Installing dependencies: packages

Otherwise, you can use e.g. `sudo dnf builddep rpm-ostree` to get
the rest of the build dependencies.

### Baseline build

rpm-ostree uses autotools to build both our C/C++ side as well
as to invoke `cargo` to build the Rust code.  After you've
[cloned the repository](https://docs.github.com/en/github/creating-cloning-and-archiving-repositories/cloning-a-repository):

```
$ git submodule update --init
$ ./autogen.sh --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
$ make
```

### Unit tests

```
$ make check
```

### Virtualized integration testing

The unit tests today don't cover much; rpm-ostree is very oriented to run as a privileged systemd unit managing a host system.

rpm-ostree has some tests that use the [coreos-assembler/kola framework](https://coreos.github.io/coreos-assembler/kola/external-tests/).

You will want to [build a custom image](https://coreos.github.io/coreos-assembler/working/#using-overrides), then run `make install` in the `${topsrcdir}/tests/kolainst/` directory, and finally `kola run --qemu-image path/to/custom-rpm-ostree-qemu.qcow2 'ext.rpm-ostree.*'`. See the [kola external tests documentation](https://coreos.github.io/coreos-assembler/kola/external-tests/#using-kola-run-with-externally-defined-tests) for more information and also how to filter tests.

There's also a `vmcheck` test suite. This model always operates on an immutable base image. It takes that image and dynamically launches a separate VM for each test using `kola spawn`. For example, using the [CoreOS Assembler](https://coreos.github.io/coreos-assembler/building-fcos/), you can build a FCOS image that contains the version of rpm-ostree that you would like to test.

One approach for (somewhat) fast iteration is `cosa build-fast`, then run e.g. `./tests/vmcheck.sh`.

To filter tests, use the `TESTS=` environment variable. For example, to run only `tests/vmcheck/test-misc-2.sh`, you can do:

```sh
TESTS='misc-2' ./tests/vmcheck.sh
```

For development, there is also a `make vmsync` which copies the built rpm-ostree
into an unlocked VM. To use this, you must have an `ssh-config` file with a host
defined in it called `vmcheck`. You can provision the VM however you want;
libvirt directly, vagrant, a remote OpenStack/EC2 instance, etc.  If you choose
vagrant for example, do something like this:

```sh
vagrant ssh-config > /path/to/src/rpm-ostree/ssh-config
```

Note that by default, these commands will retrieve the latest version of ostree
from the build environment and include those binaries when syncing to the VM.
So make sure to have the latest ostree installed or built. This allows you to
not have to worry about using libostree APIs that are not yet released.

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
git reset --hard origin/main
cd ..
```

The various `make` targets will pick up the changes and recompile.

## Testing with a custom ostree

It is sometimes necessary to develop against a version of ostree which is not
even yet in git main. In such situations, one can simply do:

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
