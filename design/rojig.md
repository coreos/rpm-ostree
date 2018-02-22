Introducing rpm-ostree rojig
--------

In the rpm-ostree project, we're blending an image system (libostree)
with a package system (libdnf).  The goal is to gain the
advantages of both.  However, the dual nature also brings overhead;
this proposal aims to reduce some of that by adding a new "rojig"
model to rpm-ostree that makes more operations use the libdnf side.

To do this, we're reviving an old idea: The [http://atterer.org/jigdo/](Jigdo)
approach to reassembling large "images" by downloading component packages.  (We're
not using the code, just the idea).

In this approach, we're still maintaining the "image" model of libostree. When
one deploys an OSTree commit, it will reliably be bit-for-bit identical. It will
have a checksum and a version number. There will be *no* dependency resolution
on the client by default, etc.

The change is that we always use libdnf to download RPM packages as they exist
today, storing any additional data inside a new "ostree-image" RPM. In this
proposal, rather than using ostree branches, the system tracks an "ostree-image"
RPM that behaves a bit like a "metapackage".

Why?
----

The "dual" nature of the system appears in many ways; users and administrators
effectively need to understand and manage both systems.

An example is when one needs to mirror content. While libostree does support
mirroring, and projects like Pulp make use of it, support is not as widespread
as mirroring for RPM. And mirroring is absolutely critical for many
organizations that don't want to depend on Internet availability.

Related to this is the mapping of libostree "branches" and rpm-md repos. In
Fedora we offer multiple branches for Atomic Host, such as
`fedora/27/x86_64/atomic-host` as well as
`fedora/27/x86_64/testing/atomic-host`, where the latter is equivalent to `yum
--enablerepo=updates-testing update`. In many ways, I believe the way we're
exposing as OSTree branches is actually nicer - it's very clear when you're on
the testing branch.

However, it's also very *different* from the yum/dnf model. Once package
layering is involved (and for a lot of small scale use cases it will be,
particularly for desktop systems), the libostree side is something that many
users and administrators have to learn *in addition* to their previous "mental model"
of how the libdnf/yum/rpm side works with `/etc/yum.repos.d` etc.

Finally, for network efficiency; on the wire, libostree has two formats, and the
intention is that most updates hit the network-efficient static delta path, but
there are various cases where this doesn't happen, such as if one is skipping a
few updates, or today when rebasing between branches. In practice, as soon as
one involves libdnf, the repodata is already large enough that it's not worth
trying to optimize fetching content over simply redownloading changed RPMs.

(Aside: people doing custom systems tend to like the network efficiency of "pure
 ostree" where one doesn't pay the "repodata cost" and we will continue to
 support that.)

How?
----

We've already stated that a primary design goal is to preserve the "image"
functionality by default. Further, let's assume that we have an OSTree commit,
and we want to point it at a set of RPMs to use as the rojig source. The source
OSTree commit can have modified, added to, or removed data from the RPM set, and
we will support that. Examples of additional data are the initramfs and RPM
database.

We're hence treating the RPM set as just data blobs; again, no dependency
resolution, `%post` scripts or the like will be executed on the client. Or again
to state this more strongly, installation will still result in an OSTree commit
with checksum that is bit-for-bit identical.

A simple approach is to scan over the set of files in the RPMs, then the set
of files in the OSTree commit, and add RPMs which contain files in the OSTree
commit to our "rojig set".

However, a major complication is SELinux labeling. It turns out that in a lot of
cases, doing SELinux labeling is expensive; there are thousands of regular
expressions involved. However, RPM packages themselves don't contain labels;
instead the labeling is stored in the `selinux-policy-targeted` package, and
further complicating things is that there are also other packages that add
labeling configuration such as `container-selinux`. In other words there's a
circular dependency: packages have labels, but labels are contained in packages.
We go to great lengths to handle this in rpm-ostree for package layering, and we
need to do the same for rojig.

We can address this by having our OIRPM contain a mapping of (package, file
path) to a set of extended attributes (including the key `security.selinux`
one).

At this point, if we add in the new objects such as the metadata objects from
the OSTree commit and all new content objects that aren't part of a package,
we'll have our OIRPM. (There is
some [further complexity](https://pagure.io/fedora-atomic/issue/94) around
handling the initramfs and SELinux labeling that we'll omit for now).
