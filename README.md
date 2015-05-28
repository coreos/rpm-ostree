# rpm-ostree
A system to compose RPMs on a server side into an
[OSTree](https://wiki.gnome.org/Projects/OSTree)
repository, and a client side tool to perform updates.

The project aims to bring together a hybrid of image-like upgrade
features (reliable replication, atomicity), with package-like
flexibility (seeing package sets inside trees, layering, partial live
updates).

## rpm-ostree is in beta!
While many of the underlying technologies here are stable,
if you are considering using this in your organization, you
should perform a careful evaluation of the whole stack.  Software
updates are obviously critical, and touch on many areas of concern.

### Contents
* [Background and rationale](doc/background.md)
* [Setting up and managing a compose server](doc/compose-server.md)
* [Administering an rpm-ostree system](doc/administrator-handbook.md)
