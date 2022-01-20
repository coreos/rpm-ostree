---
parent: Experimental features
nav_order: 1
---

# Applying a new package using CoreOS layering

This tutorial shows how to layer custom content on top of a containerized Fedora CoreOS image.
As an example, it covers how to add the `cachefilesd` package through `Dockerfile` directives and how to further configure it on boot with Ignition.

This will allow users/admins to derive custom images based on Fedora CoreOS and containing packages not present in the base image.
This tutorial is based on a previously discussed [request](https://github.com/coreos/fedora-coreos-tracker/issues/1029).

### **Note: This is an experimental feature that is subject to change.**

To follow this guide you should have the prerequisites described in the [CoreOS documentation](https://docs.fedoraproject.org/en-US/fedora-coreos/tutorial-setup/).

Let's first deploy a FCOS environment and do an initial config with Ignition. We are aiming for a simple deployment which is [described here](https://docs.fedoraproject.org/en-US/fedora-coreos/tutorial-autologin/).

### Download Fedora CoreOS next stream

We depend on some newer rpm-ostree and skopeo functionality.
Let's use the next stream. Information about the streams can be found [here](https://docs.fedoraproject.org/en-US/fedora-coreos/update-streams/).

For the purpose of this tutorial we are using QEMU, but you can use a cloud environment or platform you prefer, the [Fedora CoreOS downloads page](https://getfedora.org/en/coreos/download) has all the available options.

```bash
# This is the 'next' image we will be using.
# Anything newer than this should work too.
$ RELEASE="35.20211215.1.0"

# Download the image.
$ curl -O https://builds.coreos.fedoraproject.org/prod/streams/next/builds/$RELEASE/x86_64/fedora-coreos-$RELEASE-qemu.x86_64.qcow2.xz

# Uncompress the qemu image.
$ unxz fedora-coreos-$RELEASE-qemu.x86_64.qcow2.xz

# Rename the file for easier handling.
$ mv fedora-coreos-$RELEASE-qemu.x86_64.qcow2.xz fedora-coreos.qcow2
```

### First Ignition config via Butane

Let's create a very simple Butane config that will perform the following actions:

- Add a systemd drop-in to override the default `serial-getty@ttyS0.service`.
    - The override will make the service automatically log the `core` user in to the serial console of the booted machine.
- Set the system hostname by dropping a file at `/etc/hostname`,
- Add a `/etc/cachefilesd.conf` file with our desired values.

For this, we create a Butane configuration file `autologin.bu` with the following content:

```yaml=
variant: fcos
version: 1.4.0
systemd:
  units:
    - name: serial-getty@ttyS0.service
      dropins:
      - name: autologin-core.conf
        contents: |
          [Service]
          # Override ExecStart in main unit
          ExecStart=
          # Add new Execstart with `-` prefix to ignore failure`
          ExecStart=-/usr/sbin/agetty --autologin core --noclear %I $TERM
storage:
  files:
    - path: /etc/hostname
      mode: 0644
      contents:
        inline: |
          tutorial
    - path: /etc/cachefilesd.conf
      mode: 0644
      contents:
        inline: |
          dir /var/cache/fscache
          tag mycache
          brun  8%
          bcull 6%
          bstop 2%
          frun  9%
          fcull 5%
          fstop 4%
          secctx system_u:system_r:cachefiles_kernel_t:s0
```

This configuration can then be converted into an Ignition config with Butane:

```bash
$ butane --pretty --strict autologin.bu --output autologin.ign
```

The resulting Ignition configuration produced by Butane as `autologin.ign` can be examined by running:
```
$ cat autologin.ign
```

### Booting Fedora CoreOS

Now that we have an Ignition config, we can boot a virtual machine with it. This tutorial uses the QEMU image with `libvirt`, but you should be able to use the same Ignition config on all the platforms supported by Fedora CoreOS.

We use `virt-install` to create a new Fedora CoreOS virtual machine with a specific config:

```bash
# Setup the correct SELinux label to allow access to the config
$ chcon --verbose --type svirt_home_t autologin.ign

# Start a Fedora CoreOS virtual machine
$ virt-install --name=fcos --vcpus=2 --ram=2048 --os-variant=fedora-coreos-stable \
    --import --network=bridge=virbr0 --graphics=none \
    --qemu-commandline="-fw_cfg name=opt/com.coreos/config,file=${PWD}/autologin.ign" \
    --disk=size=20,backing_store=${PWD}/fedora-coreos.qcow2
```

The `virt-install` command will start an instance named `fcos` from the `fedora-coreos.qcow2` image using the `autologin.ign` Ignition config. It will auto-attach the serial console of the machine so you will be able to see the image bootup messages.

Once the machine is booted up you should see a few prompts and then you should be automatically logged in and presented with a bash shell:

```
[  OK  ] Started rpm-ostree System Management Daemon.

Fedora CoreOS 32.20200715.3.0
Kernel 5.7.8-200.fc32.x86_64 on an x86_64 (ttyS0)

SSH host key: SHA256:XlbayjbgDKNoAAHQxsEL5Q7BdwLxxWSw4NXN9SALLmo (ED25519)
SSH host key: SHA256:3sx5jseteO4BvdOMWIi0J4koQL015mLonnD0UPTtnZk (ECDSA)
SSH host key: SHA256:K0fn5/TMJOoMs7Fu7RRkE7IBEf2t8OYCfVaVc+GJWGs (RSA)
ens2: 192.168.122.127 fe80::5054:ff:feb9:3d97
Ignition: user provided config was applied
No ssh authorized keys provided by Ignition or Afterburn
tutorial login: core (automatic login)

[core@tutorial ~]$
```

We can verify that our configuration has been applied: we were automatically logged in to the terminal, and the hostname on the prompt is `tutorial`.

As we can see our `/etc/cachefilesd.conf` file:

```
[core@tutorial ~]$ cat /etc/cachefilesd.conf
dir /var/cache/fscache
tag mycache
brun  8%
bcull 6%
bstop 2%
frun  9%
fcull 5%
fstop 4%
secctx system_u:system_r:cachefiles_kernel_t:s0
```

Let's also make sure there is no cachefiled package installed.
```
[core@tutorial ~]$ rpm -qi cachefilesd
package cachefilesd is not installed
```

### Create a derived image with additional content

We are assuming there is a need for debugging on your environment and you need strace and a custom binary. Finally we are going to add keylime and rebase your running FCOS deployment to include them.

This would be helpful when IT needs to provide the images or there is a need to add the same system image to multiple hosts. 
Instead of making the changes on each host which will have different versions of software or configs you can create an image which then you can rebase your systems on. We are using the [fcos-derivation-example](https://github.com/coreos/fcos-derivation-example) to start this process.

Let's clone our example code on your local system and enter the directory.

```bash
$ git clone git@github.com:coreos/fcos-derivation-example.git && cd fcos-derivation-example
```

Examine the Dockerfile and notice the custom binary and where we are installing strace.

```bash
$ vi Dockerfile
```

Let's add `cachefilesd` to the install command.

That would make the last line look like:
```dockerfile
RUN rpm-ostree install strace cachefilesd && rpm-ostree cleanup -m
```

Time to build the image. In this example we are using podman.
```
$ podman build -t localhost/my-custom-fcos .
```

### Inspect the content of the derived image
At this point we have an OCI image. You can test to see if it actually has the layered packages. 

Let's get into the container.
```
$ podman run -it localhost/my-custom-fcos /bin/bash
```

Run stace inside the container. You should see the following:
```bash
$ strace --version
strace -- version 5.15
Copyright (c) 1991-2021 The strace developers <https://strace.io>.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
Optional features enabled: stack-trace=libdw stack-demangle m32-mpers mx32-mpers secontext
```

Which tells us we have successfully installed `strace` on Fedora CoreOS.

Next let's see if cachefilesd is there.
```bash
$ cachefilesd --version     
```

The output should look like the following:
```
cachefilesd version 0.10.10
```

Now that we confirmed our packages are there, we can get out of the container:
```bash
$ exit
```

### Push the OCI image to a container registry

In order to use our image, we need to push it to an image repository that is accessible to our hosts.

In this example we will use `quay.io`. But this can be any compatible container image repository. Quay allows you to build your images directly on their platform which will save you the bandwidth of pushing the image, however we are not using this feature for the tutorial. You can read about that feature [here](https://docs.quay.io/guides/building.html).

You will need to create an account on [quay.io](quay.io).

Login to quay.io from your terminal:

```bash
$ podman login quay.io
```

Push the previously built image:

```bash
$ podman push localhost/my-custom-fcos quay.io/<YOURUSER>/my-custom-fcos
```

Note that by default, Quay marks the repository as private. Please follow the
[Quay documentation](https://docs.quay.io/guides/repo-view.html) to mark the repository as public.
### Rebase the OS to the derived image

For this step we go back to our Fedora CoreOS VM console.

We need to stop the Zincati service so that auto-updates won't interfere with our manual operations.

```
[core@tutorial ~]$ sudo systemctl stop zincati.service
```

Next, we call `rpm-ostree` to rebase our system using the image we just pushed to quay.

```bash
[core@tutorial ~]$ sudo rpm-ostree rebase --experimental \
    ostree-unverified-registry:quay.io/<YOURUSER>/my-custom-fcos
```

We can check if there is a new deployment is staged:

```bash
[core@tutorial ~]$ rpm-ostree status
```

We should see a new deployment staged like this:

```
State: idle
AutomaticUpdatesDriver: Zincati
  DriverState: inactive
Deployments:
  ostree-unverified-registry:quay.io/<YOURUSER>/my-custom-fcos
                    Digest: sha256:506f0924d117af1d16fe81264d531faad1a96af8c1590ba4782b0e8bf0020d1a
                 Timestamp: 2021-12-22T21:27:42Z
                      Diff: 2 added

```

Now that we confirmed that our new deployment is staged we can reboot.
```bash
[core@tutorial ~]$ sudo systemctl reboot
```

More information about this step can be read [here](https://coreos.github.io/rpm-ostree/container/#rebasing-a-client-system).


### Booted into new deployment with layered content

Now your host should have your custom binary and extra packages ready to use. Explore it by running `cachefilesd`, `strace` and playing with the configured services.

For example we can verify that cachefiled is installed:
```
[core@tutorial ~]$ rpm -qi cachefilesd
Name        : cachefilesd
Version     : 0.10.10
Release     : 12.fc35
Architecture: x86_64
Install Date: Thu Jan  6 01:57:06 2022
Group       : Unspecified
Size        : 75373
License     : GPLv2+
Signature   : RSA/SHA256, Sun Jul 25 04:33:13 2021, Key ID db4639719867c58f
Source RPM  : cachefilesd-0.10.10-12.fc35.src.rpm
Build Date  : Wed Jul 21 19:15:26 2021
Build Host  : buildvm-x86-22.iad2.fedoraproject.org
Packager    : Fedora Project
Vendor      : Fedora Project
URL         : http://people.redhat.com/~dhowells/fscache/
Bug URL     : https://bugz.fedoraproject.org/cachefilesd
Summary     : CacheFiles user-space management daemon
Description :
The cachefilesd daemon manages the caching files and directory that are that
are used by network file systems such a AFS and NFS to do persistent caching to
the local disk.

```


Let's also verify if our pre-configured `/etc/cachefilesd` has not changed.


```
[core@tutorial ~]$ cat /etc/cachefilesd.conf
```

We should see the same values we setup through ignition, installing the package should not change the config file.

### Taking down the Virtual Machine

Congratulations you now have a Fedora CoreOS host with your desired packages layered in. If you are done exploring the VM you now delete it. First escape out of the serial console by pressing `CTRL + ]` and then type:

```
$ virsh destroy fcos
$ virsh undefine --remove-all-storage fcos
```