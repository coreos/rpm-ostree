## Using higher level build tooling

Originally `rpm-ostree compose tree` was intended to be a "high level" tool,
but that didn't work out very well in practice.  Today, you should consider
it as a low level tool.  For example, most people that want to generate
OSTree commits *also* want to generate bootable disk images, and rpm-ostree
has nothing to do with that.

One example higher level tool that takes care of both OSTree commit generation
and bootable disk images is [coreos-assembler](https://github.com/coreos/coreos-assembler).

## Background on managing an OSTree repository

Before you get started, it's recommended to read (at least) these two sections
of the OSTree manual:

 - [buildsystem-and-repos](https://ostree.readthedocs.io/en/latest/manual/buildsystem-and-repos/)
 - [repository-management](https://ostree.readthedocs.io/en/latest/manual/repository-management/)

## Generating OSTree commits from a CentOS base

First, you'll need a copy of `rpm-ostree`. The current recommendation is to use
a privileged container, but you can also install `rpm-ostree` directly to a
physical or virtual machine.

It's included in the package collection for Fedora, and there
are [CentOS Core packages](http://buildlogs.centos.org/centos/7/atomic/x86_64/Packages/) as
well as [bleeding edge CentOS builds](https://ci.centos.org/job/atomic-rdgo-centos7/).

You can create a privileged container with e.g. `podman` via: `podman run
--privileged registry.fedoraproject.org/fedora:27 ...`. However you create the
environment, run `yum -y install rpm-ostree`.

A good first thing to try would be using
the
[CentOS Atomic Host](https://github.com/CentOS/sig-atomic-buildscripts/tree/downstream) metadata
to generate a custom host.

One time setup, where we clone the git repository, then make two OSTree repos,
one for doing builds, one for export via HTTP:

```
# mkdir /srv/centos-atomic
# cd /srv/centos-atomic
# git clone https://github.com/CentOS/sig-atomic-buildscripts -b downstream
# mkdir build-repo
# ostree --repo=build-repo init --mode=bare-user
# mkdir repo
# ostree --repo=repo init --mode=archive
```

We'll also want to cache downloaded RPMs:

```
# mkdir cache
```

## Running `rpm-ostree compose tree`

This program takes as input a manifest file that describes the target system,
and commits the result to an OSTree repository.

The input format is a JSON "treefile". See examples in
`api-doc/treefile-examples`. More real-world examples include the manifest
for
[Fedora Atomic](https://pagure.io/fedora-atomic/blob/master/f/fedora-atomic-host.json) and
[CentOS Atomic](https://github.com/CentOS/sig-atomic-buildscripts/blob/downstream/centos-atomic-host.json).

If you're doing this multiple times, it's strongly recommended to create a cache
directory:

```
# rpm-ostree compose tree --unified-core --cachedir=cache --repo=/srv/centos-atomic/build-repo sig-atomic-buildscripts/centos-atomic-host.json
```

This will download RPMs from the referenced repos, and commit the result to the
OSTree repository, using the ref named by `ref`.

Once we have that commit, let's export it:

```
# ostree --repo=repo pull-local build-repo centos-atomic-host/7/x86_64/standard
```

You can tell client systems to rebase to it by combining `ostree remote add`,
and `rpm-ostree rebase` on the client side.

## More information

  * [run-treecompose script from FAHC](https://pagure.io/fedora-atomic-host-continuous/blob/2f1214c9ff35e55ec111db86be96e14d4b6040d6/f/centos-ci/run-treecompose)
  * [Build Your Own Atomic](https://github.com/jasonbrooks/byo-atomic)
  * [Build Your Own Atomic Image, Updated](http://www.projectatomic.io/blog/2014/08/build-your-own-atomic-centos-or-fedora/)
  * [Creating custom Atomic trees, images, and installers, part 1](http://developerblog.redhat.com/2015/01/08/creating-custom-atomic-trees-images-and-installers-part-1/)
  * [Creating custom Atomic trees, images, and installers, part 2](http://developerblog.redhat.com/2015/01/15/creating-custom-atomic-trees-images-and-installers-part-2/)
