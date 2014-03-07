rpm-ostree
==========

This tool takes a set of packages, and commits them to an
[OSTree](https://wiki.gnome.org/Projects/OSTree) repository.  At the
moment, it is intended for use on build servers.

Using rpm-ostree
----------------

There are two levels; the core "rpm-ostree" command takes a set of
packages and commits them to an OSTree repository.

The higher level rpm-ostree-autobuilder parses a "products.json" which
generates potentially many filesystem trees.  It also has code to
generate disk images and run smoketests.


Installing and setting up a repository
--------------------------------------

There are packages available in the rpm-ostree COPR; you can also
just "sudo make install" it.

Once you have that done, choose a build directory.  Here we'll use
/srv/rpm-ostree.

	# cd /srv/rpm-ostree
	# mkdir repo
	# ostree --repo=repo init --mode=archive-z2


Running rpm-ostree
------------------

The core "rpm-ostree" takes as input a "treefile".  There is a demo
one in `src/demo-treefile.json`.

	# rpm-ostree sometreefile.json

All this does is use yum to download RPMs from the referenced repos,
and commit the result to the OSTree repository, using the ref named by
`ref`.

You can export `/srv/rpm-ostree/repo` via any static webserver.

Running the autobuilder
-----------------------

The autobuilder instead takes as input a `products.json` which
generates multiple treefiles.  Try this:

	# ln -s /path/to/rpm-ostree.git/fedostree/products.json .
	# rpm-ostree-autobuilder

That will automatically poll every hour for changes in the RPMs
referenced by the `products.json` file, commit them to the
`/srv/rpm-ostree/repo`, and generate cached disk images in
`/srv/rpm-ostree/images`.
