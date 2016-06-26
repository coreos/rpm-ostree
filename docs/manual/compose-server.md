## Background on managing an OSTree repository

Before you get started, it's recommended to read (at least) these two sections
of the OSTree manual:

 - [buildsystem-and-repos](https://ostree.readthedocs.io/en/latest/manual/buildsystem-and-repos/)
 - [repository-management](https://ostree.readthedocs.io/en/latest/manual/repository-management/)

## Generating OSTree commits from a CentOS base

First, you'll need a copy of `rpm-ostree` on your compose server.
It's included in the package collection for Fedora, and there are
[CentOS Core packages](http://buildlogs.centos.org/centos/7/atomic/x86_64/Packages/)
as well as [bleeding edge CentOS builds](https://ci.centos.org/job/atomic-rdgo-centos7/).

A good first thing to try would be using the
[CentOS Atomic Host](https://github.com/CentOS/sig-atomic-buildscripts/tree/downstream)
metadata to generate a custom host.

One time setup, where we clone the git repository, then make two
OSTree repos, one for doing builds, one for export via HTTP:

```
# mkdir /srv/centos-atomic
# cd /srv/centos-atomic
# git clone https://github.com/CentOS/sig-atomic-buildscripts -b downstream
# mkdir build-repo
# ostree --repo=build-repo init --mode=bare-user
# mkdir repo
# ostree --repo=repo init --mode=archive-z2
```

## Running `rpm-ostree compose tree`

This program takes as input a manifest file that describes the target
system, and commits the result to an OSTree repository.

The input format is a JSON "treefile".  See examples in
`doc/treefile-examples` as well as `doc/treefile.md`.

```
# rpm-ostree compose tree --repo=/srv/centos-atomic/build-repo sig-atomic-buildscripts/centos-atomic-host.json
```

This will download RPMs from the referenced repos, and commit the
result to the OSTree repository, using the ref named by `ref`.

Once we have that commit, let's export it:

```
# ostree --repo=repo pull-local build-repo centos-atomic-host/7/x86_64/standard
```

You can tell client systems to rebase to it by combining `ostree
remote add`, and `rpm-ostree rebase` on the client side.

## More information

  * [Build Your Own Atomic](https://github.com/jasonbrooks/byo-atomic)
  * [Build Your Own Atomic Image, Updated](http://www.projectatomic.io/blog/2014/08/build-your-own-atomic-centos-or-fedora/)
