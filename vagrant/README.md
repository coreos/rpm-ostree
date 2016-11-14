Developing using Docker + Vagrant
---------------------------------

The current tooling here is oriented towards
doing builds inside a CentOS 7 pet container,
with Vagrant on the host.

You should share the git working directory with the c7 container.
Assuming you have git repositories stored in `/srv`, something like:

```
docker run --name c7dev --privileged -v /srv:/srv --net=host -ti centos bash
```

You can start the Vagrant box.  To work around "fuse-sshfs" not
being built into the Vagrant box, do something like this:

```
vagrant up ; vagrant provision; vagrant halt; vagrant up
```

Now, once you do a build inside the c7dev container, like:

```
./autogen.sh CFLAGS='-ggdb -O0' --prefix=/usr --libdir=/usr/lib64 --enable-installed-tests --enable-gtk-doc
```

To sync over and install the built binaries:

```
make vmsync
```

You may also want to use `vmcheck`, like this:

```
make vmoverlay && make vmcheck
```
