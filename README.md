# rpm-ostree Overview

New! See the docs online at [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/ )

-----

rpm-ostree is a hybrid image/package system.  It uses
[OSTree](https://wiki.gnome.org/Projects/OSTree) as an image format,
and uses RPM as a component model.

The project aims to bring together a hybrid of image-like upgrade
features (reliable replication, atomicity), with package-like
flexibility (introspecting trees to find package sets, package
layering, partial live updates).

**Features:**

 - Atomic upgrades and rollback for host system updates
 - A server side tool to consume RPMs and commit them to an OSTree repository
 - A system daemon to consume ostree commits as updates

Projects using rpm-ostree
-------------------------

[Project Atomic](http://www.projectatomic.io/) uses rpm-ostree to
provide a minimal host for Docker formatted Linux containers.
Replicating a base immutable OS, then using Docker for applications.

Using rpm-ostree to build OS images/trees
-----------------------------------------

See [Compose Server](docs/manual/compose-server.md).

Building
--------

Releases are available as GPG signed git tags, and most recent
versions support extended validation using
[git-evtag](https://github.com/cgwalters/git-evtag).

However, in order to build from a git clone, you must update the
submodules.  If you're packaging and want a tarball, I recommend using
a "recursive git archive" script.  There are several available online;
[this code](https://git.gnome.org/browse/ostree/tree/packaging/Makefile.dist-packaging#n11)
in OSTree is an example.

Once you have a git clone or recursive archive, the next step is to
install the build dependencies.  At the moment, rpm-ostree has a hard
requirement on a bleeding edge version of
[libhif](https://github.com/rpm-software-management/libhif/).  It also
requires a few other libraries like
[librepo](https://github.com/rpm-software-management/librepo).

Once you have the dependencies, building is the same as every
autotools project:

```
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr --libdir=/usr/lib64 --sysconfdir=/etc
make
```

At this point you can run some of the unit tests with `make check`.
For more information on this, see `CONTRIBUTING.md`.

More documentation
------------------

New! See the docs online at [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/ )

Hacking
-------

See [Hacking](HACKING.md).

Contributing
------------

See [Contributing](docs/CONTRIBUTING.md).

