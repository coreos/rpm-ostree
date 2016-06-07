Using vmcheck
=============

One time setup:

```
host$ vagrant up
host$ vagrant ssh
vm$ sudo rpm-ostree rebase centos-atomic-continuous:centos-atomic-host/7/x86_64/devel/continuous
vm$ systemctl reboot
```

Edit source code on the host system; to synchronize, use:

```
vagrant rsync
```

To build and install into the VM:

```

host$ vagrant ssh
vm$ cd ~/sync/tests/vmcheck
vm$ make build
vm$ make install
vm$ systemctl restart rpm-ostreed
```


