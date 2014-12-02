Treefile
--------

 * `ref`: string, mandatory: Holds a string which will be the name of
   the branch for the content.

 * `gpg_key` string, optional: Key ID for GPG signing; the secret key
   must be in the home directory of the building user.  Defaults to
   none.

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from `.repo` files in the same directory as the treefile.

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

 * `etc-group-members`: Array of strings, optional: Unix groups in this
   list will be stored in `/etc/group` instead of `/usr/lib/group`.  Use
   this option for groups for which humans should be a member.

 * `install-langs`: Array of strings, optional.  This sets the RPM
   _install_langs macro.  Set this to e.g. `["en_US", "fr_FR"]`.

 * `packages`: Array of strings, mandatory: Set of installed packages.
   Names prefixed with an `@` (e.g. `@core`) are taken to be the names
   of comps groups.

 * `units`: Array of strings, optional: Systemd units to enable by default

 * `default_target`: String, optional: Set the default systemd target

 * `initramfs-args`: Array of strings, optional.  Passed to the
    initramfs generation program (presently `dracut`).  An example use
    case for this with Dracut is `--filesystems xfs,ext4` to ensure
    specific filesystem drivers are included.

 * `remove-files`: Delete these files from the generated tree

 * `remove-from-packages`: Array, optional: Delete from specified packages
   files which match the provided array of regular expressions.
   This is safer than `remove-files` as it allows finer grained control
   with less risk of too-wide regular expressions.

   Each array element is an array, whose first member is a package name,
   and subsequent members are regular expressions (compatible with JavaScript).

   Example: `remove-from-packages: [["cpio", "/usr/share/.*"], ["dhclient", "/usr/lib/.*", "/usr/share/.*"]]`

   Note this does not alter the RPM database, so `rpm -V` will complain.

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
