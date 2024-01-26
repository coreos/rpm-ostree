---
parent: Composing images
nav_order: 2
---

# Treefile reference

A "treefile" is a made up term for a JSON-formatted specification used
as input to `rpm-ostree compose tree` to bind "set of RPMs with
configuration" to "OSTree commit".

It's recommended to keep them in git, and set up a CI system like
Jenkins to operate on them as it changes.

It supports the following parameters:

 * `ref`: string, mandatory: Holds a string which will be the name of
   the branch for the content. This field supports variable substitution.

   Example: `ref: "cool-os/${releasever}/${stream}"`

 * `metadata`: Mapping of strings to values, optional.  This can be used
   for other tools to insert arbitrary metadata into the treefile which
   they parse later, for example via `rpm-ostree compose tree --print-metadata-json`.

 * `gpg-key` (or `gpg_key`): string, optional: Key ID for GPG signing; the
   secret key must be in the home directory of the building user.  Defaults to
   none.

 * `repos`: array of strings, mandatory: Names of yum repositories to
   use, from any files that end in `.repo`, in the same directory as
   the treefile.  `rpm-ostree compose tree` does not use the system
   `/etc/yum.repos.d`, because it's common to want to compose a target
   system distinct from the one the host sytem is running.

 * `selinux`: boolean, optional: Defaults to `true`.  If `false`, then
   no SELinux labeling will be performed on the server side.

 * `ima`: boolean, optional: Defaults to `false`.  Propagate any
   IMA signatures in input RPMs into the final OSTree commit.

 * `boot-location` (or `boot_location`): string, optional:
    There are 2 possible values:
    * "new": A misnomer, this value is no longer "new".  Kernel data
      goes in `/usr/lib/ostree-boot` in addition to `/usr/lib/modules`.
      This is the default; use it if you have a need to care about
      upgrading from very old versions of libostree.
    * "modules": Kernel data goes just in `/usr/lib/modules`.  Use
      this for new systems, and systems that don't need to be upgraded
      from very old libostree versions.

 * `etc-group-members`: Array of strings, optional: Unix groups in this
   list will be stored in `/etc/group` instead of `/usr/lib/group`.  Use
   this option for groups for which humans should be a member.

 * `install-langs`: Array of strings, optional.  This sets the RPM
   _install_langs macro.  Set this to e.g. `["en_US", "fr_FR"]`.

 * `mutate-os-release`: String, optional.  This causes rpm-ostree to
    change the `VERSION` and `PRETTY_NAME` fields to include the ostree
    version, and adds a specific `OSTREE_VERSION` key that can be easier
    for processes to query than looking via ostree. The actual value of
    this key represents the baked string that gets substituted out for
    the final OSTree version. This field supports variable substitution.

    Example: `mutate-os-release: "${releasever}"`

 * `documentation`: boolean, optional. If this is set to false it sets the RPM
   transaction flag "nodocs" which makes yum/rpm not install files marked as
   documentation. The default is true.

 * `packages`: Array of strings, mandatory: Each entry in this list should name a package;
   however, virtual provides are also supported.
   For convenience when writing YAML/JSON, entries in this list will also be split by
   whitespace.  Finally, another syntax special case is that surrounding the entry
   with quotes `'` will suppress this whitespace split, and that in turn makes it possible to use version query
   syntax such as `'podman >= 4.1'`.

 * `packages-$basearch`: Array of strings, optional: Set of installed packages, used
    only if $basearch matches the target architecture name.

 * `exclude-packages`: Array of strings, optional: Each entry in this list is a package name
   which will be filtered out.  If a package listed in the manifest ("manifest package") indirectly hard depends
   on one of these packages, it will be a fatal error.  If a manifest package recommends one
   of these packages, the recommended package will simply be omitted.  It is also a fatal
   error to include a package both as a manifest package and in the exclusion list.

   An example use case for this is for Fedora CoreOS, which will exclude the `python` and `python3`
   packages to ensure that nothing included in the OS starts depending on it in the future.

 * `repo-packages`: Array of objects, optional: Set of packages to install from
   specific repos. Each object in the array supports the following keys:
   * `packages`: Array of strings, required: List of packages to install.
   * `repo`: String, required: Name of the repo from which to fetch packages.

 * `ostree-layers`: Array of strings, optional: After all packages are unpacked,
    check out these OSTree refs, which must already be in the destination repository.
    Any conflicts with packages will be an error.

 * `ostree-override-layers`: Array of strings, optional: Like above, but any
    files present in packages and prior layers will be silently overriden.
    This is useful for development builds to replace parts of the base tree.

 * `container-cmd`: array of strings, optional: This maps to the `CMD` Dockerfile
   instruction, and is currently only meaningful when encapsulating/exporting
   an ostree commit as a Docker/OCI container.

 * `bootstrap_packages`: Array of strings, optional: Deprecated; you should
    now just include this set in the main `packages` array.

 * `recommends`: boolean, optional: Install `Recommends`, defaults to `true`.

 * `units`: Array of strings, optional: Systemd units to enable by default

 * `default-target` (or `default_target`): String, optional: Set the default
    systemd target.

 * `initramfs-args`: Array of strings, optional.  Passed to the
    initramfs generation program (presently `dracut`).  An example use
    case for this with Dracut is `--filesystems xfs,ext4` to ensure
    specific filesystem drivers are included.  If not specified,
    `--no-hostonly` will be used.
    Deprecated; you should place files in `/etc/dracut.conf.d` instead. This
    option is ignored when regenerating the initramfs in the container flow.

 * `rpmdb`: String, optional: The RPM database backend.  Can be one of
    `target` (the default) or `host`.  Legacy values 
    `bdb`, `ndb`, and `sqlite` are treated as `target`.
    This option is a historical mistake; ultimately the only thing that really works is to write
    the rpmdb in the `target` format - the format that the `librpm`
    library in the target filesystem tree understands.  However, this is
    a relatively new default, so the value `host` is provided as a fallback

 * `rpmdb-normalize`: boolean, optional. Defaults to `false`.  If enabled,
    this will perform various manipulations of the RPM database to, as much
    as possible, guarantee a deterministic result for the on-disk RPM
    database. Requires the `SOURCE_DATE_EPOCH` environment variable to be set
    to the UNIX epoch time to be used as the build timestamp. Currently only
    fully supports the `bdb` backend. Somewhat experimental.

 * `selinux-label-version`: integer, optional:  When set to `1`, this will
    turn on an ostree flag which labels files in `/usr/etc` as if they were in
    `/etc`.  This is important to aid in having a "transient" `/etc`.

 * `cliwrap`: boolean, optional.  Defaults to `false`.  If enabled,
    rpm-ostree will replace binaries such as `/usr/bin/rpm` with
    wrappers that intercept unsafe operations, or adjust functionality.

    The default is `false` out of conservatism; you likely want to enable this.

 * `cliwrap-binaries`: array of strings, optional.  An explicit list of binaries
    to enable `cliwrap`.  The current allowed set contains just one value: `kernel-install`.

 * `readonly-executables`: boolean, optional.  Defaults to `false` (for backcompat).
    If enabled, rpm-ostree will remove the write bit from all executables.

    The default is `false` out of conservatism; you likely want to enable this.

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

 * `releasever`: String or integer, optional: Used to set the librepo
   `$releasever` variable, commonly used in yum repo files.

   Example: `releasever: "26"`
   Example: `releasever: 35`

   When defined, it is automatically also added to the `variable` map described
   below. Thus, it can be used for substitutions and conditional includes.

 * `automatic-version-prefix` (or `automatic_version_prefix`): String, optional:
   Set the prefix for versions on the commits. The idea is that if the previous
   commit on the branch to the doesn't match the prefix, or doesn't have a
   version, then the new commit will have the version as specified. If the
   prefix matches exactly, then we append ".1". Otherwise we parse the number
   after the prefix and increment it by one and then append that to the prefix.

   A current date/time may also be passed through `automatic-version-prefix`,
   by including a date tag in the prefix as such: `<date:format>`, where
   `format` is a string with date formats such as `%Y` (year), `%m` (month), etc.
   The full list of supported formats is [found in the GLib API](https://developer.gnome.org/glib/stable/glib-GDateTime.html#g-date-time-format).
   Including a date/time format will automatically append a `.0` to
   the version, if not present in the prefix, which resets to `.0` if
   the date (or prefix) changes.

   This means that on an empty branch with an `automatic-version-prefix`
   of `"22"` the first three commits would get the versions: "22", "22.1",
   "22.2". Some example progressions are shown:

   | `automatic-version-prefix` | version progression                        |
   | -------------------------- | ------------------------------------------ |
   | `22`                       | 22, 22.1, 22.2, ...                        |
   | `22.1`                     | 22.1.1, 22.1.2, 22.1.3, ...                |
   | `22.<date:%Y>`             | 22.2019.0, 22.2019.1, 22.2020.0, ...       |
   | `22.<date:%Y>.1`           | 22.2019.1.0, 22.2019.1.1, 22.2020.1.0, ... |

   This field supports variable substitution.

   Example: `automatic-version-prefix: "22.0"`
   Example: `automatic-version-prefix: "${releasever}.<date:%Y%m%d>.dev"`

 * `automatic-version-suffix`: String, optional: This must be a single ASCII
   character.  The default value is `.`.  Used by `automatic-version-prefix`.
   For example, if you set this to `-` then `22` will become `22-1`, `22-2` etc.

 * `add-commit-metadata`: Map<String, Object>, optional: Metadata to inject as
   part of composed commits. Keys inserted here can still be overridden at the
   command line with `--add-metadata-string` or `--add-metadata-from-json`. All
   objects of type string support variable substitution.

   Example:

   ```yaml
   add-commit-metadata:
     cool-os.is-production: false
     cool-os.git-snapshot: "${git_snapshot}"
   ```

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

   If you want to depend on network access, or tools not in the target host,
   you can use the split-up `rpm-ostree compose install`
   and `rpm-ostree compose postprocess/commit` commands.

 * `postprocess`: array of string, optional: This is an *inline* script
   variant of `postprocess-script` that is also an array, so it works
   correctly with inheritance.  If both `postprocess-script` and `postprocess`
   are provided, then `postprocess-script` will be executed after all
   other `postprocess`.

 * `include`: string or array of string, optional: Path(s) to treefiles which will be
   used as an inheritance base.  The semantics for inheritance are:
   Non-array values in child values override parent values.  Array
   values are concatenated.  Filenames will be resolved relative to
   the including treefile.  Since rpm-ostree 2019.5, this value may
   also be an array of strings.  Including the same file multiple times
   is an error.

 * `arch-include`: object (`Map<String,include>`), optional: Each member of this
   object should be the name of a base architecture (`$basearch`), and the `include` value
   functions the same as the `include` key above - it can be either
   a single string, or an array of strings - and it has the same semantics.
   Entries which match `arch-include` are processed after `include`.

   Example (in YAML):

   ```yaml
   arch-include:
     x86_64: bootloader-x86_64.yaml
     s390x:
       - bootloader-s390x.yaml
       - tweaks-s390x.yaml
    ```

 * `conditional-include`: array of objects, optional: This is like `include`,
    but supports conditions based on variables defined in `variables`. The
    syntax is:

    ```yaml
    conditional-include:
      - if: <var> <op> <value>
        include: <include>
    ```

    `<var>` is a variable name previously defined via `variables`. `<op>` must
    be one of `==`, `!=`, `<`, `<=`, `>`, `>=`. `<value>` has the same sematics
    as variable values: it can be a boolean, number, or string (in quotes).
    `<include>` functions the same as the `include` key above - it can be either
    a string or an array of strings.

    Multiple conditions may be provided:

    ```yaml
    conditional-include:
      - if:
        - <var> <op> <value>
        - <var> <op> <value>
        - <var> <op> <value>
        include: <include>
    ```

    In that case, *all* conditions must be met for the inclusion to happen.

    Example:

    ```yaml
   variables:
     devpackages: true
     stream: "development"
   releasever: 35
   conditional-include:
     - if: devpackages == true
       include: dev-packages.yaml
   conditional-include:
     - if: stream != "development"
       include: delete-dev-files.yaml
   conditional-include:
     - if: releasever >= 35
       include: f35-selinux-workaround.yaml
    ```

 * `container`: boolean, optional: Defaults to `false`.  If `true`, then
   rpm-ostree will not do any special handling of kernel, initrd or the
   /boot directory. This is useful if the target for the tree is some kind
   of container which does not have its own kernel.  This also implies
   several other options, such as `tmp-is-dir: true` and `selinux: false`.

 * `add-files`: Array, optional: Copy external files to the rootfs.

   Each array element is an array, whose first member is the source
   file name, and the second element is the destination name.  The
   source file must be in the same directory as the treefile.

   Example: `"add-files": [["bar", "/usr/share/bar"], ["foo", "/lib/foo"]]`

   Note that in the OSTree model, not all directories are managed by OSTree. In
   short, only files in `/usr` (or UsrMove symlinks into `/usr`) and `/etc` are
   supported. For more details, see the OSTree manual:
   https://ostreedev.github.io/ostree/deployment/

 * `tmp-is-dir`: boolean, optional: Defaults to `false`.  By default,
   rpm-ostree creates symlink `/tmp` → `sysroot/tmp`.  When set to `true`,
   `/tmp` will be a regular directory, which allows the `systemd` unit
   `tmp.mount` to mount it as `tmpfs`. It's more flexible to leave it
   as a directory, and further, we don't want to encourage `/sysroot`
   to be writable. For host system composes, we recommend turning this on;
   it's left off by default to ease the transition.

 * `machineid-compat`: boolean, optional: Defaults to `true`.  By default,
   rpm-ostree creates `/usr/etc/machine-id` as an empty file for historical
   reasons.  Set this to `false` to ensure it's not present at all.  This
   will cause systemd to execute `ConditionFirstBoot=`, which implies
   running `systemctl preset-all` for example.  This requires booting the system
   with `rw` so that systemd can properly populate `/etc/machine-id` and execute
   the presets at switchroot.  When this is enabled, the `units`
   directive will no longer function.  Instead, create a
   `/usr/lib/systemd/system-presets/XX-example.preset` file as part of a package
   or in the postprocess script.

 * `platform-module`: string, optional.  For the very rare case where you need
   to either provide or override the platform module.  When using RPM modules
   libdnf will attempt to derive the appropriate platform module by inspecting
   various virtual provide entries on the available packages.  If this fails it
   will fall back to parsing `/etc/os-release` or `/usr/lib/os-release` in that
   order.  If the first mechanism fails and we are running a compose, we will not
   have the necessary files to allow the fall back to work.  If you find yourself
   in this situation you can provide the platform module name yourself using this
   option. You can also use this to override the platform module if needed. This
   field supports variable substitution.

 * `variables`: object (`Map<String, value>`), optional: Define new variables
   which could then be substituted into the value of various fields and used in
   conditional includes described above. Supported value types are booleans,
   numbers, and strings.

   The `releasever` variable is reserved and automatically populated to the same
   value as the `releasever` key. The `basearch` variable is reserved and
   automatically populated to the base architecture of the compose.

   Example:

   ```yaml
   variables:
     devpackages: true
     stream: "development"
   releasever: 35
   ref: "cool-os/${releasever}/${stream}"
   conditional-include:
     - if: devpackages == true
       include: dev-packages.yaml
   ```

 * `repo_metadata`: String, optional: Can be one of `inline` (the default),
   `detached` or  `disabled`. If set to `inline`, parts of the RPM repository
   metadata are added to the OSTree commit metadata under the
   `rpmostree.rpmmd-repos` key. Setting this to `detached` also adds the
   information but puts it in the detached metadata of the commit, preventing
   it from affecting the commit checksum. Setting it to `disabled` will
   prevent the metadata from being added at all.

## Experimental options

All options listed here are subject to change or removal in a future
version of `rpm-ostree`.

 * `lockfile-repos`: array of strings, optional: Semantically similar to
   `repo`, but these repos will only be used to fetch packages locked
   via lockfiles. This is useful when locked packages are kept
   separately from the primary repos and one wants to ensure that
   rpm-ostree will otherwise not select unlocked packages from them.
 * `modules`: Object, optional: Describes RPM modules to enable or install. Two
   keys are supported:
   * `enable`: Array of strings, required: Set of RPM module specs to enable
     (the same formats as dnf are supported, e.g. `NAME[:STREAM]`).
     One can then cherry-pick specific packages from the enabled modules via
     `packages`.
   * `install`: Array of strings, required: Set of RPM module specs to install
     (the same formats as dnf are supported, e.g. `NAME[:STREAM][/PROFILE]`).
 * `repovars`: object (`Map<String, String>`), optional: yum repository variable
   names to use when substituting variables in yum repo files. The `releasever`
   variable name is invalid. Use the `releasever` key instead. The `basearch`
   name is invalid; it is filled in automatically.
 * `opt-usrlocal-overlays`: boolean, optional: Defaults to `false`.  By
   default, `/opt` and `/usr/local` are symlinks to subdirectories in `/
   var`. This prevents the ability to compose with packages that install in
   those directories. If enabled, RPMs with `/opt` and `/usr/local` content
   are allowed; client-side, both paths are writable overlay directories on.
   Requires libostree v2023.9+.
