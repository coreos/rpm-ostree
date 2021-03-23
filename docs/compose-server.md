---
nav_order: 4
---

# Compose server
{: .no_toc }

1. TOC
{:toc}

## Managing RPM based OSTree commits

With `rpm-ostree compose` you get a tool to compose your own ostree commits based on a
[treefile](https://coreos.github.io/rpm-ostree/treefile/) configuration, a couple of RPMs,
some post-processing and possibly some custom modifications directly in the resulting tree.

The tool allows to either build a tree commit in one go with a single command: `rpm-ostree compose tree`.
Or to split that process up into smaller chunks with the usage of `rpm-ostree compose install`,
followed by `rpm-ostree compose postprocess` and finally `rpm-ostree compose commit`. While
the former approach is pretty complete and allows most use-cases the latter is useful if you need
some more customization on the resulting filesystem. More customization than the sandboxed
post-process functionality of the treefile allows.

In most scenarios you'll want to consider using a more "high level" tool, than `rpm-ostree compose`.

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

## Granular tree compose with `install|postprocess|commit`

In order to get even more control we split `rpm-ostree compose tree` into
`rpm-ostree compose install`, followed by `rpm-ostree compose postprocess`
and finally `rpm-ostree compose commit`.

Similar to `rpm-ostree compose tree` we'll use a "treefile". We'll also specify a target directory
serving as our work-in-progress rootfs:

```
# rpm-ostree compose install --unified-core --cachedir=cache --repo=/srv/repo /path/to/manifest.yaml /var/sysroot
```

This will download RPMs from the referenced repos and execute any specified post-process scripts.

We now can alter anything found under `/var/sysroot/rootfs`.

Next we can run more postprocessing:

```
# rpm-ostree compose postprocess postprocess /var/sysroot/rootfs /path/to/manifest.yaml
```

When we are finished with our manual changes we can now create the commit:

```
# rpm-ostree compose tree --repo=/srv/repo /path/to/manifest.yaml /var/sysroot
```

Once we have that commit, let's export it:

```
# ostree --repo=repo pull-local build-repo exampleos/8/x86_64/stable
```

You can tell client systems to rebase to it by combining `ostree remote add`,
and `rpm-ostree rebase` on the client side.

## Generating OSTree commits in a container

`rpm-ostree compose tree` runs well in an unprivileged (or "run as root")
podman container.  You can also use other container tools, they are just less
frequently tested.

You can also directly install `rpm-ostree` on a traditional `yum/rpm` based
virtual (or physical) machine - it won't affect your host.  However, containers
are encouraged.

## More information

 - https://www.osbuild.org/news/2020-06-01-how-to-ostree-anaconda.html
 - https://github.com/coreos/coreos-assembler

