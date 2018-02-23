Treefile
--------

A "treefile" is a made up term for a JSON-formatted specification used
as input to `rpm-ostree compose tree` to bind "set of RPMs with
configuration" to "OSTree commit".

It's recommended to keep them in git, and set up a CI system like
Jenkins to operate on them as it changes.

It supports the following parameters:

 * `ref`: string, mandatory: Holds a string which will be the name of
   the branch for the content.

 * `gpg_key` string, optional: Key ID for GPG signing; the secret key
   must be in the home directory of the building user.  Defaults to
   none.

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from any files that end in `.repo`, in the same directory as
   the treefile.  `rpm-ostree compose tree` does not use the system
   `/etc/yum.repos.d`, because it's common to want to compose a target
   system distinct from the one the host sytem is running.

 * `selinux`: boolean, optional: Defaults to `true`.  If `false`, then
   no SELinux labeling will be performed on the server side.

 * `boot_location`: string, optional: Historically, ostree put bootloader data
    in /boot.  However, this has a few flaws; it gets shadowed at boot time,
    and also makes dealing with Anaconda installation harder.  There are 3
    possible values:
    * "both": the default, kernel data goes in /boot and /usr/lib/ostree-boot
    * "legacy": Now an alias for "both"; historically meant just "boot"
    * "new": kernel data goes in /usr/lib/ostree-boot and /usr/lib/modules

 * `etc-group-members`: Array of strings, optional: Unix groups in this
   list will be stored in `/etc/group` instead of `/usr/lib/group`.  Use
   this option for groups for which humans should be a member.

 * `install-langs`: Array of strings, optional.  This sets the RPM
   _install_langs macro.  Set this to e.g. `["en_US", "fr_FR"]`.

 * `mutate-os-release`: String, optional.  This causes rpm-ostree to
    change the `VERSION` and `PRETTY_NAME` fields to include the ostree
    version, and adds a specific `OSTREE_VERSION` key that can be easier
    for processes to query than looking via ostree.

 * `documentation`: boolean, optional. If this is set to false it sets the RPM
   transaction flag "nodocs" which makes yum/rpm not install files marked as
   documentation. The default is true.

 * `packages`: Array of strings, mandatory: Set of installed packages.
   comps groups are currently not supported due to walters having issues with libcomp:
   https://github.com/cgwalters/fedora-atomic-work/commit/36d18b490529fec91b74ca9b464adb73ef0ab462

 * `packages-$basearch`: Array of strings, optional: Set of installed packages, used
    only if $basearch matches the target architecture name.

 * `bootstrap_packages`: Array of strings, optional: Deprecated; you should
    now just include this set in the main `packages` array.

 * `units`: Array of strings, optional: Systemd units to enable by default

 * `default_target`: String, optional: Set the default systemd target

 * `initramfs-args`: Array of strings, optional.  Passed to the
    initramfs generation program (presently `dracut`).  An example use
    case for this with Dracut is `--filesystems xfs,ext4` to ensure
    specific filesystem drivers are included.  If not specified,
    `--no-hostonly` will be used.

 * `remove-files`: Array of files to delete from the generated tree.

 * `remove-from-packages`: Array, optional: Delete from specified packages
   files which match the provided array of regular expressions.
   This is safer than `remove-files` as it allows finer grained control
   with less risk of too-wide regular expressions.

   Each array element is an array, whose first member is a package name,
   and subsequent members are regular expressions (compatible with JavaScript).

   Example: `remove-from-packages: [["cpio", "/usr/share/.*"], ["dhclient", "/usr/lib/.*", "/usr/share/.*"]]`

   Note this does not alter the RPM database, so `rpm -V` will complain.

 * `preserve-passwd`: boolean, optional: Defaults to `true`.  If enabled,
   and `check-passwd` has a type other than file, copy the `/etc/passwd` (and
   `/usr/lib/passwd`) files from the previous commit if they exist. If
   check-passwd has the file type, then the data is preserved from that file to
   `/usr/lib/passwd`.
   This helps ensure consistent uid/gid allocations across builds.  However, it
   does mean that removed users will exist in the `passwd` database forever.

 * `check-passwd`: Object, optional: Checks to run against the new passwd file
   before accepting the tree. All the entries specified should exist (unless
   ignored) and have the same values or the compose will fail. There are four
   types: none (for no checking), previous (to check against the passwd file in
   the previous commit), file (to check against another passwd file), and data
   to specify the relevant passwd data in the json itself.
   Note that if you choose file, and preserve-passwd is true then the data will
   be copied from the referenced file and not the previous commit.

   Example: `check-passwd: { "type": "none" }`
   Example: `check-passwd: { "type": "previous" }`
   Example: `check-passwd: { "type": "file", "filename": "local-passwd" }`
   Example: `check-passwd: { "type": "data", "entries": { "bin": 1, "adm": [3, 4] } }`
   See also: `ignore-remove-users`

 * `check-groups`: Object, optional: Checks to run against the new group file
   before accepting the tree. All the entries specified should exist (unless
   ignored) and have the same values or the compose will fail. There are four
   types: none (for no checking), previous (to check against the group file in
   the previous commit), file (to check against another group file), and data
   to specify the relevant group data in the json itself.
   Note that if you choose file, and preserve-passwd is true then the data will
   be copied from the referenced file and not the previous commit.

   Example: `check-groups: { "type": "none" }`
   Example: `check-groups: { "type": "previous" }`
   Example: `check-groups: { "type": "file", "filename": "local-group" }`
   Example: `check-groups: { "type": "data", "entries": { "bin": 1, "adm": 4 } }`
   See also: `ignore-remove-groups`

 * `ignore-removed-users`: Array, optional: Users to ignore if they are missing
   in the new passwd file. If an entry of `*` is specified then any user can be
   removed without failing the compose.

   Example: `ignore-removed-users: ["avahi-autoipd", "tss"]`

 * `ignore-removed-groups`: Array, optional: Groups to ignore if they are missing
   in the new group file. If an entry of `*` is specified then any group can be
   removed without failing the compose.

   Example: `ignore-removed-groups: ["avahi"]`

 * `releasever`: String, optional: Used to set the librepo `$releasever` variable,
   commonly used in yum repo files.

   Example: `releasever: "26"`

 * `automatic_version_prefix`: String, optional: Set the prefix for versions
   on the commits. The idea is that if the previous commit on the branch to the
   doesn't match the prefix, or doesn't have a version, then the new commit will
   have the version as specified. If the prefix matches exactly, then we append
   ".1". Otherwise we parse the number after the prefix and increment it by one
   and then append that to the prefix.

   This means that on an empty branch with an automatic_version_prefix of "22"
   the first three commits would get the versions: "22", "22.1", "22.2"

   Example: `automatic_version_prefix: "22.0"`

 * `postprocess-script`: String, optional: Full filesystem path to a script
   that will be executed in the context of the target tree.  The script
   will be copied into the target into `/tmp`, and run as a container
   (a restricted chroot, with no network access).  After execution is
   complete, it will be deleted.

   It is *strongly recommended* to avoid using this except as a last resort.
   Having the system generated through RPMs allows administrators to understand
   the inputs to the system.  Any new files created through this mechanism will
   not have the versioning inherent in RPM.

   Only the script file will be copied in; thus if it has any dependencies,
   on data beyond what is in the target tree, you must embed them in the binary
   itself.

   An example use for this is working around bugs in the input RPMs that are
   hard to fix in stable releases.

   Note this does not alter the RPM database, so `rpm -V` will complain.

 * `include`: string, optional: Path to another treefile which will be
   used as an inheritance base.  The semantics for inheritance are:
   Non-array values in child values override parent values.  Array
   values are concatenated.  Filenames will be resolved relative to
   the including treefile.

 * `container`: boolean, optional: Defaults to `false`.  If `true`, then
   rpm-ostree will not do any special handling of kernel, initrd or the
   /boot directory. This is useful if the target for the tree is some kind
   of container which does not have its own kernel.

 * `add-files`: Array, optional: Copy external files to the rootfs.

   Each array element is an array, whose first member is the source
   file name, and the second element is the destination name.  The
   source file must be in the same directory as the treefile.

   Example: `"add-files": [["bar", "/bar"], ["foo", "/foo"]]`

 * `tmp-is-dir`: boolean, optional: Defaults to `false`.  By default,
   rpm-ostree creates symlink `/tmp` → `/sysroot/tmp`.
   It's more flexible to leave it as a directory (systemd will mount it),
   and further, we don't want to encourage `/sysroot` to be writable.
   For host system composes, we recommend turning this on; it's left off
   by default to ease the transition.

Experimental options
--------

All options listed here are subject to change or removal in a future
version of `rpm-ostree`.

 * `ex-rojig-spec`: string, optional:  If specified, will also cause
   a run of `rpm-ostree ex commit2rojig` on changes.  Also requires the
   `--ex-rojig-output-rpm` commandline option.
