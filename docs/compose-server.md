---
nav_order: 4
---

# Compose server
{: .no_toc }

1. TOC
{:toc}

## Using higher level build tooling

Originally `rpm-ostree compose tree` was intended to be a "high level" tool,
but that didn't work out very well in practice.  Today, you should consider
it as a low level tool.  For example, most people that want to generate
OSTree commits *also* want to generate bootable disk images, and rpm-ostree
has nothing to do with that.

One example higher level tool that takes care of both OSTree commit generation
and bootable disk images is [coreos-assembler](https://github.com/coreos/coreos-assembler);
it is strongly oriented towards "CoreOS-like" systems which include rpm-ostree
and Ignition.

The [osbuild](https://www.osbuild.org/) project has some support for
rpm-ostree based systems.  See [this blog entry](https://www.osbuild.org/news/2020-06-01-how-to-ostree-anaconda.html)
for example.

## Background on managing an OSTree repository

Before you get started, it's recommended to read (at least) these two sections
of the OSTree manual:

 - [buildsystem-and-repos](https://ostreedev.github.io/ostree/buildsystem-and-repos/)
 - [repository-management](https://ostreedev.github.io/ostree/repository-management/)

## Generating OSTree commits in a container

`rpm-ostree compose tree` runs well in an unprivileged (or "run as root")
podman container.  You can also use other container tools, they are just less
frequently tested.

You can also directly install `rpm-ostree` on a traditional `yum/rpm` based
virtual (or physical) machine - it won't affect your host.  However, containers
are encouraged.

## Choosing a base config

Currently, rpm-ostree is fairly coupled to the Fedora project.  We are open to supporting
other distributions however.

Example base rpm-ostree "manifest repositories" are:

 - [Silverblue](https://pagure.io/workstation-ostree-config)
 - [IoT](https://pagure.io/fedora-iot/ostree)
 - [Fedora CoreOS](https://github.com/coreos/fedora-coreos-config/)

## Running `rpm-ostree compose tree`

This program takes as input a manifest file that describes the target system,
and commits the result to an OSTree repository.

The input format is a YAML (or JSON) "treefile".

If you're doing this multiple times, it's strongly recommended to create a cache
directory:

```
# rpm-ostree compose tree --unified-core --cachedir=cache --repo=/srv/repo /path/to/manifest.yaml
```

This will download RPMs from the referenced repos, and commit the result to the
OSTree repository, using the ref named by `ref`.

Once we have that commit, let's export it:

```
# ostree --repo=repo pull-local build-repo exampleos/8/x86_64/stable
```

You can tell client systems to rebase to it by combining `ostree remote add`,
and `rpm-ostree rebase` on the client side.

## More information

 - https://www.osbuild.org/news/2020-06-01-how-to-ostree-anaconda.html
 - https://github.com/coreos/coreos-assembler

