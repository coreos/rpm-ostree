<!-- markdownlint-disable-file MD013 MD033 -->
# Building rpm-ostree in autosd image from scratch

The Automotive Stream Distribution (AutoSD) is an upstream repository for Red Hat In-Vehicle OS, much like CentOS Stream is to RHEL. AutoSD is based on CentOS Stream with a few divergences. If your base system is not a CentOS, you can use a container to generate a compatible CentOS Stream 9 RPM as showed below, otherwise just skip the step and use your raw system.

## Building rpm-ostree rpm on CentOS Stream9

Create a **ContainerFile** with the content below.

```bash
FROM centos:stream9

RUN dnf update -y
RUN dnf install -y 'dnf-command(config-manager)' epel-release
RUN dnf config-manager --set-enabled crb
RUN dnf config-manager --add-repo https://buildlogs.centos.org/9-stream/automotive/aarch64/packages-main/
RUN dnf config-manager --add-repo https://buildlogs.centos.org/9-stream/autosd/aarch64/packages-main/

# Install rpm-build and create empty dir structure.
RUN dnf install -y rpm-build
RUN mkdir -p $HOME/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

# Install deps required for recent rpm-ostree
RUN cd $HOME/rpmbuild/SPECS/ && curl -OL https://gitlab.com/redhat/centos-stream/rpms/rpm-ostree/-/raw/c9s/rpm-ostree.spec

RUN cd $HOME/rpmbuild/SPECS && dnf builddep -y rpm-ostree.spec
RUN cargo install cargo-vendor-filterer --version ^0.5

# Lets use the latest from community (see next curl rustup step).
# Also avoids "cargo metadata: error: failed to run rustc to learn about target-specific information"
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
```

## Building the image

```bash
podman build -t rpmostreedevel -f ContainerFile
```

## Running the image

The strategy now is run the container sharing the current **HOME/rpmbuild** dir, so as soon the rpm is created in the container it will be located in the host machine in the running user dir.

```bash
sudo podman run --rm --security-opt label=disable -v /home/$USER/rpmbuild/:/home/$USER/rpmbuild/ -ti rpmostreedevel /bin/bash

[root@d1ec4ed543ec /]#
```

## Building a forked rpm-ostree

Now time to clone your [rpm-ostree](https://github.com/coreos/rpm-ostree) fork and build it.

Cloning the repo and switch to your work branch

```bash
cd /tmp
git clone https://github.com/dougsland/rpm-ostree.git && cd rpm-ostree
git checkout remotes/origin/initoverlayfs -b initoverlayfs
```

Adding required submodules for the build

```bash
git submodule update --init
```

Generating source code

```bash
cd packaging
make -f Makefile.dist-packaging
cp rpm-ostree-e2dbedd3.tar.xz $HOME/rpmbuild/SOURCES/

# Getting version and updating the spec file
rpmostree_tarxz=$(ls rpm-ostree-*.tar.xz| sed 's/rpm-ostree-\(.*\)\.tar\.xz/\1/')
sed -i "/^Version:/s/:.*/: $rpmostree_tarxz/" $HOME/rpmbuild/SPECS/rpm-ostree.spec

# Remove any specific Patch for Fedora koji build
# See:
# https://gitlab.com/redhat/centos-stream/rpms/rpm-ostree/-/blob/8e861dccda62f170461da2b90cc2f549886094aa/0001-cliwrap-rpm-mark-eval-E-as-safe.patch#L14
# https://gitlab.com/redhat/centos-stream/rpms/rpm-ostree/-/blob/8e861dccda62f170461da2b90cc2f549886094aa/0001-cliwrap-rpm-mark-eval-E-as-safe.patch
sed -i '/Patch[0-9]*:/d' $HOME/rpmbuild/SPECS/rpm-ostree.spec
```

Keep in mind, the version of rpm-ostree must be higher than
the current version shipped in the autosd image. In the time this document was created the source file generated in the rpm-ostree is based on hash commit which is not enough most of the times. Below a quick hack.

```bash
pushd $HOME/rpmbuild/SOURCES/
    # untar the source file:
    tar xvf rpm-ostree-e2dbedd3.tar.xz

    # rename the dir
    mv rpm-ostree-e2dbedd3 rpm-ostree-2025.11

    # tar again
    tar -cJf rpm-ostree-2025.11.tar.xz rpm-ostree-2025.11
popd
```

Change the spec file to use this new version, replace in the
spec file the Version field to Version: **2025.11**

```bash
vi /root/rpmbuild/SPECS/rpm-ostree.spec
```

Finally generate the RPM

```bash
rpmbuild -ba ~/rpmbuild/SPECS/rpm-ostree.spec
```

You are done, exit from the container

```bash
exit
```

## Building autosd with the generated file

Clone the autosd repo

```bash
git clone https://gitlab.com/CentOS/automotive/sample-images && cd sample-images
```

Create a RPM repo in rpmbuild/RPMS, now it includes the fresh rpm-ostree

```bash
pushd ~/rpmbuild/RPMS
    createrepo .
popd
```

Add the new RPM repo just created into the sample-images project

```bash
vi osbuild-manifests/distro/cs9.ipp.yml
  - id: extra
   baseurl: file:///home/curtine/rpmbuild/RPMS/
```

Full diff Example:

```bash
diff --git a/osbuild-manifests/distro/cs9.ipp.yml b/osbuild-manifests/distro/cs9.ipp.yml
index c2e3abc..1564c9d 100644
--- a/osbuild-manifests/distro/cs9.ipp.yml
+++ b/osbuild-manifests/distro/cs9.ipp.yml
@@ -15,6 +15,8 @@ mpp-vars:
     baseurl: https://mirror.stream.centos.org/SIGs/9-stream/autosd/$arch/packages-main/
   - id: next
     baseurl: https://download.copr.fedorainfracloud.org/results/@centos-automotive-sig/next/epel-9-$arch/
+  - id: extra
+    baseurl: file:///root/rpmbuild/RPMS/
   distro_devel_repos:
   - id: crb
     baseurl: $distro_baseurl/CRB/$arch/os/
```

Generate the autosd image using initoverlayfs + rpm-ostree

```bash
pushd osbuild-manifests
     sudo make cs9-qemu-initoverlayfs-ostree.x86_64.qcow2
popd
```
