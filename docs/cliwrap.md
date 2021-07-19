---
parent: Experimental features
nav_order: 1
---

# Wrapping other CLI entrypoints

A simple way to describe the goal of rpm-ostree is to convert the default model for operating system updates to be "image based".

However, many distributions such as Fedora (and derivatives) ship a variety of tools which need adaptation for this.  In some
cases, changing those upstream projects to be "ostree aware" makes sense, and is straightforward.  In others, it is easier
to wrap/reimplement the functionality of those tools.

rpm-ostree today contains code to wrap/reimplement the latter, which includes `dnf/yum`, `grubby`, and `rpm` for example.

## Enabling and disabling cliwrap on a client system

```
$ rpm-ostree deploy --ex-cliwrap=true
```

You may also want to follow this with an `rpm-ostree ex apply-live` to apply the change live.

To disable: `rpm-ostree deploy --ex-cliwrap=false`

## Enabling cliwrap at build time

This is just `cliwrap: true` in the treefile.

## Wrapped commands

### rpm

Currently, this is mostly oriented towards executing the real underlying `rpm` binary,
but may e.g. drop privileges for commands that may not be safe.  For example:

```
[root@cosa-devsh ~]# rpm -e toolbox
rpm-ostree: Note: This system is image (rpm-ostree) based.
rpm-ostree: Dropping privileges as `rpm` was executed with not "known safe" arguments.
rpm-ostree: You may invoke the real `rpm` binary in `/usr/libexec/rpm-ostree/wrapped/rpm`.
rpm-ostree: Continuing execution in 5 seconds.

error: can't create transaction lock on /usr/share/rpm/.rpm.lock (Read-only file system)
[root@cosa-devsh ~]# 
```

In the future, the `rpm-ostree` cliwrap for `rpm` may help translate some commands.

### dracut 

This one just prints a helpful redirection:

```
[root@cosa-devsh ~]# dracut
This system is rpm-ostree based; initramfs handling is
integrated with the underlying ostree transaction mechanism.
Use `rpm-ostree initramfs` to control client-side initramfs generation.
```

### grubby

Similar to dracut, prints a redirection:

```
[root@cosa-devsh ~]# grubby
This system is rpm-ostree based; grubby is not used.
Use `rpm-ostree kargs` instead.
```
### yum/dnf

The implementation of this is tracked in [this Github issue](https://github.com/coreos/rpm-ostree/issues/2883).

But for example, typing `dnf update` will be translated to `rpm-ostree update`.  Other commands
such as `dnf install foo` will print a helpful error like this:

```
[root@cosa-devsh ~]# dnf install foo
Note: This system is image (rpm-ostree) based.
Before installing packages to the host root filesystem, consider other options:
 - `toolbox`: For command-line development and debugging tools in a privileged container
 - `podman`: General purpose containers
 - `docker`: General purpose containers
 - `rpm-ostree install`: Install RPM packages layered on the host root filesystem.
   Consider these "operating system extensions".
   Add `--apply-live` to immediately start using the layered packages.
error: not implemented
```
