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


Setting up the autobuilder
--------------------------

There are packages available in the rpm-ostree COPR; you can also
just "sudo make install" it.

Once you have that done, choose a build directory.  Here we'll use
/srv/rpm-ostree.

 # cd /srv/rpm-ostree
 # mkdir repo
 # ostree --repo=repo init --mode=archive-z2
 # ln -s /path/to/rpm-ostree.git/fedostree/products.json .
 # rpm-ostree-autobuilder autobuilder

That will automatically poll every hour for changes in the RPMs
referenced by the products.json file, commit them to the
/srv/rpm-ostree/repo, and generate cached disk images in
/srv/rpm-ostree/images.

You can export /srv/rpm-ostree/repo (and images/, and builds/) via any
static webserver.
