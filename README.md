# rpm-ostree: A true hybrid image/package system

rpm-ostree combines [libostree](https://ostree.readthedocs.io/en/latest/) (an image system),
with [libdnf](https://github.com/rpm-software-management/libdnf) (a package system), bringing
many of the benefits of both together.

```
                         +-----------------------------------------+
                         |                                         |
                         |       rpm-ostree (daemon + CLI)         |
                  +------>                                         <---------+
                  |      |     status, upgrade, rollback,          |         |
                  |      |     pkg layering, initramfs --enable    |         |
                  |      |                                         |         |
                  |      +-----------------------------------------+         |
                  |                                                          |
                  |                                                          |
                  |                                                          |
+-----------------|-------------------------+        +-----------------------|-----------------+
|                                           |        |                                         |
|         libostree (image system)          |        |            libdnf (pkg system)          |
|                                           |        |                                         |
|   C API, hardlink fs trees, system repo,  |        |    ties together libsolv (SAT solver)   |
|   commits, atomic bootloader swap         |        |    with librepo (RPM repo downloads)    |
|                                           |        |                                         |
+-------------------------------------------+        +-----------------------------------------+
```

For more information, see the online manual: [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/)

**Features:**

 - Transactional, background image-based (versioned/checksummed) upgrades
 - OS rollback without affecting user data (`/usr` but not `/etc`, `/var`) via libostree
 - Client-side package layering (and overrides)
 - Easily make your own: `rpm-ostree compose tree` and [CoreOS Assembler](https://github.com/coreos/coreos-assembler)

Projects using rpm-ostree
--------------------------

The OSTree project is independent of distributions and agnostic to how content
is delivered and managed; it's used today by e.g. Debian, Fedora, and OpenEmbedded
derived systems among others.  There are some examples in the [OSTree github](https://github.com/ostreedev/ostree).

In contrast, rpm-ostree is intended to be tightly integrated with the Fedora
ecosystem.  Today it is the underlying update mechanism of [Fedora CoreOS](https://getfedora.org/coreos/)
as well as its derivative RHEL CoreOS.  It is also used by [Fedora IoT](https://iot.fedoraproject.org/)
and [Fedora Silverblue](https://silverblue.fedoraproject.org/). 

Originally, it was productized as part of [Project Atomic](http://www.projectatomic.io/).

Why?
---

Package systems such as apt and yum are highly prevalent in Linux-based operating systems.  The core premise of rpm-ostree is that image-based updates should be the default.  This provides a high degree of predictability and resiliency.  However, where rpm-ostree is fairly unique in the ecosystem is supporting client-side package layering and overrides; deeply integrating RPM as an (optional) layer on top of OSTree.

A good way to think of package layering is recasting RPMs as "operating system extensions", similar to how browser extensions work (although before those were sandboxed).  One can use package layering for components not easily containerized, such as PAM modules, custom shells, etc.

Further, one can easily use `rpm-ostree override replace` to override the kernel or userspace components with the very same RPMs shipped to traditional systems.  The Fedora project for example continues to only have one kernel build.

Layering and overrides are still built on top of the default OSTree engine - installing and updating client-side packages constructs a new filesystem root, it does not by default affect your booted root.  This preserves the "image" nature of the system.

Manual
------

For more information, see the online manual: [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/)

Talks and media
-----

A number of Project Atomic talks are available; see for
example [this post](https://lists.projectatomic.io/projectatomic-archives/atomic-devel/2018-January/msg00057.html) which
has a bigger collection that also includes talks on containers.

rpm-ostree specific talks:

 * devconf.cz 2018: [Colin Walters: Hybrid image/package OS updates with rpm-ostree](https://www.youtube.com/watch?v=4A_xl5dC210) [slides](https://fedorapeople.org/~walters/2018.01-devconf/index.html)
 * devconf.cz 2018: [Peter Robinson: Using Fedora and OSTree for IoT](https://www.youtube.com/watch?v=mRqV38qT-wc)

