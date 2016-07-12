# rpm-ostree Overview

New! See the docs online at [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/ )

-----

rpm-ostree is a hybrid image/package system.  It uses
[OSTree](https://ostree.readthedocs.io/en/latest/) as an image format,
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

Hacking
-------

See [Hacking](HACKING.md).

Contributing
------------

See [Contributing](CONTRIBUTING.md).

