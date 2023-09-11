---
parent: Experimental features
nav_order: 1
---

# bootc

rpm-ostree vendors the [containers/bootc project](https://github.com/containers/bootc) as
an experimental feature; this avoids needing a separate package.

## Enabling bootc

As part of a container build:

```
$ ln -sr /usr/bin/rpm-ostree /usr/bin/bootc
```

In the future we are likely to pursue deeper integration as bootc gains more features.

## Compatibility

It's very much an intentional design feature that `bootc` is compatible with
a container-deployed rpm-ostree system.  You can seamlessly invoke e.g.
`bootc upgrade` as well as `rpm-ostree upgrade` for example.

However, as soon as any "machine local" changes are set up, such as layered
packages or local initramfs regeneration, `bootc upgrade` will no longer work.
