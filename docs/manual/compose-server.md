## Installing and setting up a repository

Once you have that done, choose a build directory.  Here we'll use
/srv/rpm-ostree.

	# cd /srv/rpm-ostree
	# mkdir repo
	# ostree --repo=repo init --mode=archive-z2

## Running `rpm-ostree compose tree`

This program takes as input a manifest file that describes the target
system, and commits the result to an OSTree repository.

See also: https://github.com/projectatomic/rpm-ostree-toolbox

The input format is a JSON "treefile".  See examples in
`doc/treefile-examples` as well as `doc/treefile.md`.

	# rpm-ostree compose tree --repo=/srv/rpm-ostree/repo --proxy=http://127.0.0.1:8123 sometreefile.json

All this does is use yum to download RPMs from the referenced repos,
and commit the result to the OSTree repository, using the ref named by
`ref`.  Note that we've specified a local caching proxy (`polipo` in
this case) - otherwise we will download the packages for each
treecompose.

You can export `/srv/rpm-ostree/repo` via any static webserver.

The use of `--proxy` is not mandatory but strongly recommended - with
this option you can avoid continually redownloading the packages every
compose.  I personally use
[Polipo](http://www.pps.univ-paris-diderot.fr/~jch/software/polipo/),
but you can of course any HTTP proxy you wish.

