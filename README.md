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

Once you have a git clone or recursive archive, building is the
same as almost every autotools project:

```
env NOCONFIGURE=1 ./autogen.sh
./configure --prefix=...
make
make install DESTDIR=/path/to/dest
```

More documentation
------------------

New! See the docs online at [Read The Docs (OSTree)](https://ostree.readthedocs.org/en/latest/ )

Some more information is available on the old wiki page:
https://wiki.gnome.org/Projects/OSTree

Contributing
------------

See [Contributing](CONTRIBUTING.md).

