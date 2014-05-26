rpm-ostree
==========

This tool takes a set of packages, and commits them to an
[OSTree](https://wiki.gnome.org/Projects/OSTree) repository.  At the
moment, it is intended for use on build servers.

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

Second, you must install `nss-altfiles` on the host system, and
edit your /etc/nsswitch.conf to include `altfiles`, like this:

	passwd: files altfiles 
	group:  files altfiles

You may or may not be using SSSD (and thus the `sss` option); if you
are then it should look like:

	passwd: files altfiles sss
	group:  files altfiles sss

For more information, see:
http://lists.rpm.org/pipermail/rpm-maint/2014-January/003652.html

There are packages available in the rpm-ostree COPR:
http://copr-fe.cloud.fedoraproject.org/coprs/walters/rpm-ostree/

At the moment, all of the tooling except for the patched
`shadow-utils` is in Fedora rawhide.

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
