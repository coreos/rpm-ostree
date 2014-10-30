rpm-ostree
==========

RPM-OSTree (also nicknamed `/usr/bin/atomic`) is a mechanism to
assemble RPMs on a server side into an
[OSTree](https://wiki.gnome.org/Projects/OSTree) repository.  Then
clients can update from that repository in a reliable image-like
fashion, via `atomic upgrade`.

Currently, rpm operates on a read-only mode on installed systems; it
is not possible to add or remove anything on the client.  In return,
client systems are reliably synchronized with the server-provided
tree.  For example, if a package is removed in the server-composed
set, when clients update, it also drops out of their tree.

This model works well in scenarios where one wants reliable state
replication of master to many client machines.

Installing and setting up a repository
--------------------------------------

First, unfortunately you must *disable* SELinux on the build host in
order to *support* SELinux on the built system.  See:
https://bugzilla.redhat.com/show_bug.cgi?id=1060423

Once you have that done, choose a build directory.  Here we'll use
/srv/rpm-ostree.

	# cd /srv/rpm-ostree
	# mkdir repo
	# ostree --repo=repo init --mode=archive-z2

Running `rpm-ostree compose tree`
---------------------------------

This program takes as input a manifest file that describes the target
system, and commits the result to an OSTree repository.

See also: https://github.com/projectatomic/rpm-ostree-toolbox

The input format is a JSON "treefile".  See examples in
`doc/treefile-examples`, as well as `doc/treefile.md`.

	# rpm-ostree compose tree --repo=/srv/rpm-ostree/repo --proxy=http://127.0.0.1:8123 sometreefile.json

All this does is use yum to download RPMs from the referenced repos,
and commit the result to the OSTree repository, using the ref named by
`ref`.  Note that we've specified a local caching proxy (`polipo` in
this case) - otherwise we you will download the packages for each
treecompose.

You can export `/srv/rpm-ostree/repo` via any static webserver.

The use of `--proxy` is not mandatory but strongly recommended - with
this option you can avoid continually redownloading the packages every
compose.  I personally use
[Polipo](http://www.pps.univ-paris-diderot.fr/~jch/software/polipo/),
but you can of course any HTTP proxy you wish.
