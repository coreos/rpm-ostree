Using vmcheck
=============

One time setup:

```
host$ vagrant up
host$ vagrant ssh
vm$ sudo rpm-ostree rebase centos-atomic-continuous:centos-atomic-host/7/x86_64/devel/continuous
vm$ sudo systemctl reboot
```

Though rebasing on CAHC is not strictly required, it will
allow your code to make use of the most recent builds of
projects to which `rpm-ostree` is tightly coupled, such as
`ostree` and `libhif`.

If you need to test your code with custom `ostree` or
`libhif` builds, you have no choice for now other than
making your own tree (and yum repo for use by the build
container). We're hoping to improve this workflow soon.

To synchronize source code on the host system, use:

```
vagrant rsync
```

To build and install into the VM:

```
host$ vagrant ssh
vm$ cd ~/sync/tests/vmcheck
vm$ make build
vm$ make install
```

At this point, a new deployment containing the new binaries
is ready to be used. All that's left is to reboot:

```
vm$ sudo systemctl reboot
```
