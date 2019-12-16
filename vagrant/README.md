Developing using Podman + Vagrant
---------------------------------

**Note: These instructions are in flux and will be oriented
toward a [coreos-assembler](https://github.com/coreos/coreos-assembler)
based workflow, which targets Fedora CoreOS (FCOS). You may find built FCOS
images at: http://artifacts.ci.centos.org/fedora-coreos/prod/builds/latest/.**

The current tooling here is oriented towards
doing builds inside a Fedora 29 pet container,
with Vagrant on the host.

You should share the git working directory with the f29 container.
Assuming you have git repositories stored in `/srv`, something like:

```
podman run --name f29dev --privileged -v /srv:/srv --net=host -ti registry.fedoraproject.org/fedora:29 bash
```

You can start the Vagrant box.  To work around "fuse-sshfs" not
being built into the Vagrant box, do something like this:

```
vagrant up; vagrant provision; vagrant halt; vagrant up
```

Note to run `vagrant` as non-root, you'll need to either 
[add your user to the `libvirt` group, or configure polkit](https://github.com/projectatomic/rpm-ostree/issues/49#issuecomment-478091562).

If working under the `/srv` directory, you should first do
`sudo chown -R <your-user> /srv/path/to/rpm-ostree/` on your host so
that rootless `vagrant` can write there.

Before building, you may need to install the build dependencies in the
f29dev container. A quick way to do this is:

```
ci/installdeps.sh
```

Now, run autoreconf inside the f29dev container:

```
./autogen.sh CFLAGS='-ggdb -O0' --prefix=/usr --libdir=/usr/lib64 --enable-installed-tests --enable-gtk-doc
```

To sync over and install the built binaries to the Vagrant VM:

```
make vmsync
```

See [HACKING.md](../HACKING.md).
