rpm-ostree
==========

This program serves a dual role; its "tree compose" command is
intended for use on build servers, to take RPM packages and commit
them to an [OSTree](https://wiki.gnome.org/Projects/OSTree)
repository.  On the client side, it acts as a consumer of the
`libostree` shared library, integrating upgrades with RPM.

Major changes since 2014.8
--------------------------

The previous major release of this program contained within it an
"autobuilder" codebase which had significant functionality beyond just
composing trees, such as creating VM disk images and running
smoketests.

Since that time, the other functionality has moved to:
https://github.com/cgwalters/rpm-ostree-toolbox

This program now only commits trees to a repository, using "treefiles"
which are very simple JSON input data.

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

Running rpm-ostree
------------------

The core "rpm-ostree tree compose" builtin as input a "treefile".  See
examples in `doc/treefile-examples`, as well as `doc/treefile.md`.

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
