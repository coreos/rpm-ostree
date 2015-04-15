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

## Treefile key documentation

 * `ref`: string, mandatory: Holds a string which will be the name of
   the branch for the content.

 * `gpg_key` string, optional: Key ID for GPG signing; the secret key
   must be in the home directory of the building user.  Defaults to
   none.

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from the system `/etc/yum.repos.d`.

 * `selinux`: boolean, optional: Defaults to `true`.  If `false`, then
   no SELinux labeling will be performed on the server side.

 * `boot_location`: string, optional: Historically, ostree put bootloader data
    in /boot.  However, this has a few flaws; it gets shadowed at boot time,
    and also makes dealing with Anaconda installation harder.  There are 3
    possible values:
    * "legacy": the default, data goes in /boot
    * "both": Kernel data in /boot and /usr/lib/ostree-boot
    * "new": Kernel data in /usr/lib/ostree-boot

 * `bootstrap_packages`: Array of strings, mandatory: The `glibc` and
   `nss-altfiles` packages (and ideally nothing else) must be in this
   set; rpm-ostree will modify the `/etc/nsswitch.conf` in the target
   root to ensure that `/usr/lib/passwd` is used.

 * `packages`: Array of strings, mandatory: Set of installed packages.
   Names prefixed with an `@` (e.g. `@core`) are taken to be the names
   of comps groups.

 * `units`: Array of strings, optional: Systemd units to enable by default

 * `default_target`: String, optional: Set the default systemd target

 * `include`: string, optional: Path to another treefile which will be
   used as an inheritance base.  The semantics for inheritance are:
   Non-array values in child values override parent values.  Array
   values are concatenated.  Filenames will be resolved relative to
   the including treefile.
