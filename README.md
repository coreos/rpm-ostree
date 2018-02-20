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
 - Easily make your own: `rpm-ostree compose tree`

Projects using rpm-ostree
--------------------------

[Project Atomic](http://www.projectatomic.io/) is an umbrella project for
delivering upstream container technologies and combined with a minimized,
atomically upgradable host system to Fedora, Red Hat Enterprise Linux, and CentOS.

rpm-ostree is the underlying technology for host updates. The headlining project
is "Atomic Host", which is a server variant oriented towards running Linux
containers using e.g. Kubernetes. However, there is now also a Workstation
variant, showing the full generality of the rpm-ostree model.

Manual
------

For more information, see the online manual: [Read The Docs (rpm-ostree)](https://rpm-ostree.readthedocs.org/en/latest/)

Talks and media
-----

A number of Project Atomic talks are available; see for
example [this post](https://lists.projectatomic.io/projectatomic-archives/atomic-devel/2018-January/msg00057.html) which
has a bigger collection that also includes talks on containers.

rpm-ostree specific talks:

 * devconf.cz 2018: [Colin Walters: Hybrid image/package OS updates with rpm-ostree](https://www.youtube.com/watch?v=eWoFpOoA-tE) [slides](https://fedorapeople.org/~walters/2018.01-devconf/index.html)
 * devconf.cz 2018: [Peter Robinson: Using Fedora and OSTree for IoT](https://www.youtube.com/watch?v=mRqV38qT-wc)

