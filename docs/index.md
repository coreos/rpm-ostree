# What is nts? (formerly rpm-ostree)

`nts` is a hybrid image/package system, designed for the Fedora/CentOS
ecosystem. It was formerly known as `rpm-ostree`. `nts` builds on top of two
shared libraries, [libOSTree](https://ostree.readthedocs.io/en/latest/) for
image-like atomic
upgrades, [libdnf](https://github.com/rpm-software-management/libdnf) for
package management.

NOTE: In much of the rest of this documentation, we will refer to `rpm-ostree`;
it's expected that the naming transition will occur slowly over time.

# Getting it

If you want to try the system as a user, we recommend
[Project Atomic](http://www.projectatomic.io/). If you are
interested in assembling your own systems, see
[compose server](manual/compose-server.md).

# Why would I want to use it?

Unlike traditional package managers, `nts` defaults to using
an atomic upgrade model from a pre-assembled tree.  Client systems
replicate this from a server, and *no package management* is
performed on the client side.  For example, we do not download the
yum/dnf repodata, and no dependency resolution is performed.

However, one can use `nts install` to dynamically layer on additional
packages on the client side.  This ends up with a hybrid blend
between traditional image systems, and package management.

Of course, for Project Atomic, the focus of Atomic Host is on
installing applications in containers, but we believe package
layering is an essential bridge.

# Why not implement these changes in an existing package manager?

The [OSTree related projects](https://ostree.readthedocs.io/en/latest/manual/related-projects/)
section covers this to a degree.  As soon as one starts taking
"snapshots" or keeping track of multiple roots, it uncovers many
issues.  For example, which content specifically is rolled forward or
backwards?  If the package manager isn't deeply aware of a snapshot
tool, it's easy to lose coherency.

### Filesystem layout

A concrete example is that rpm-ostree moves the RPM database
to `/usr/share/rpm`, since we want one per root `/usr`.  In contrast,
the [snapper](http://snapper.io/) tool goes to some effort to
include `/var/lib/rpm` in snapshots, but
avoid rolling forward/back log files in `/var/log`.

OSTree requires clear rules around the semantics
of directories like `/usr` and `/var` across upgrades, and
while this requires changing some software, we believe the
result is significantly more reliable than having two separate
systems like yum and snapper glued together, or apt-get and BTRFS,
etc.

### User experience 

Furthermore, beyond just the mechanics of things like the filesystem
layout, the implemented upgrade model affects the entire user
experience.

For example, the base system OSTree commits that one replicates from a
remote server can be assigned version numbers.  They are
released as coherent wholes, tested together.  If one is simply
performing snapshots on the client side, every client machine
can have different versions of components.

Related to this is that rpm-ostree clearly distinguishes which
packages you have layered, and it's easy to remove them, going back to
a pristine, known state.  Many package managers just implement a "bag
of packages" model with no clear bases or layering.  As the OS evolves
over time, "package drift" occurs where you might have old, unused
packages lying around.

# But still evolutionary

On the other hand, rpm-ostree in other ways is very evolutionary.
There have been many, many different package managers invented -
why not adopt or build on one of those?

The answer here is that it takes a long time for tooling to be built
on top of a package format - things like mirroring servers.  Another
example is source format representations - there are many, many
tools that know how to build source RPMs.

From the perspective of distribution which has all of that ecosystem
built up, rpm-ostree does introduce a new binary format (ostree), but
otherwise includes an RPM database, and also operates on packages.  It
is not a new source format either.

