# rpm-ostree: A true hybrid image/package system

rpm-ostree is a hybrid image/package system.  It combines
[libostree](https://ostree.readthedocs.io/en/latest/) as a base image format,
and accepts RPM on both the client and server side, sharing code with the
[dnf](https://en.wikipedia.org/wiki/DNF_(software)) project; specifically
[libdnf](https://github.com/rpm-software-management/libdnf). and thus bringing
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

**Features:**

 - Transactional, background image-based (versioned/checksummed) upgrades
 - OS rollback without affecting user data (`/usr` but not `/etc`, `/var`) via libostree
 - Client-side package layering (and overrides)
 - Easily make your own: `rpm-ostree compose tree` and [CoreOS Assembler](https://github.com/coreos/coreos-assembler)
   as well as https://fedoraproject.org/wiki/Changes/OstreeNativeContainer

## Documentation

For more information, see the [project documentation](docs/index.md) or the
[project documentation website](https://coreos.github.io/rpm-ostree).

## License

rpm-ostree includes code licensed under GPLv2+, LGPLv2+, (Apache 2.0 OR MIT).
For more information, see [LICENSE](https://github.com/coreos/rpm-ostree/blob/main/LICENSE).
