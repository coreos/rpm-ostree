# rpm-ostree Overview 

rpm-ostree is a hybrid image/package system.  It uses
[OSTree](https://ostree.readthedocs.io/en/latest/) as a base image
format, and supports RPM on both the client and server side using
[libhif](https://github.com/rpm-software-management/libhif).

For more information, see the online manual: [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/)

**Features:**

 - Atomic upgrades and rollback for host system updates
 - A server side tool to consume RPMs and commit them to an OSTree repository
 - A system daemon to consume OSTree commits as updates

Projects using rpm-ostree
--------------------------

[Project Atomic](http://www.projectatomic.io/) uses rpm-ostree to
provide a minimal host for Docker formatted Linux containers.
Replicating a base immutable OS, then using Docker for applications.

Manual
------

For more information, see the online manual: [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/)
