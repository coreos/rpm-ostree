---
nav_order: 6
---

# Extensions

Extensions are additional packages which client machines can
install using package layering. While rpm-ostree itself is
indifferent on the subject, most rpm-ostree-based distros
encourage a containerized workflow for better separation of
host and application layers. But sometimes, containerization
is not ideal for some software, and yet it may not be
desirable to bake them into the OSTree commit by default.

Package layering normally fetches such extensions from
remote repos. However in some architectures there may be a
better way to transfer them, or one may simply want tighter
control over them and stronger binding between OSTree commit
and extension versions (e.g. for reproducibility, guaranteed
depsolve, QE, releng, etc..).

`rpm-ostree compose extensions` takes an `extensions.yaml`
file describing OS extensions (packages) and a base OSTree
commit. After performing a depsolve, it downloads the
extension packages and places them in an output directory.

## extensions.yaml

The format of the `extensions.yaml` file is as follow:

```yaml
# The top-level object is a dict. The only supported key
# right now is `extensions`, which is a dict of extension
# names to extension objects.
extensions:
  # This can be whatever name you'd like. The name itself
  # isn't used by rpm-ostree.
  sooper-dooper-tracers:
    # Optional; defaults to `os-extension`. An OS extension
    # is an extension intended to be `rpm-ostree install`ed.
    kind: os-extension
    # List of packages for this extension
    packages:
        - strace
        - ltrace
    # Optional list of architectures on which this extension
    # is valid. These are RPM basearches. If omitted,
    # defaults to all architectures.
    architectures:
        - x86_64
        - aarch64
  kernel-dev:
    # A development extension lists packages useful for
    # developing for the target OSTree, but won't be layered
    # on top. A common example is kernel modules. No
    # depsolving happens, packages listed are downloaded.
    kind: development
    packages:
      - kernel-devel
      - kernel-headers
    # Optional name of a base package used to constrain the
    # EVR of all the packages in this extension.
    match-base-evr: kernel
```
