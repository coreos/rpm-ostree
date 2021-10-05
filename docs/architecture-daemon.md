---
parent: Architecture
nav_order: 2
---

# Daemon model
{: .no_toc }

1. TOC
{:toc}

## Basic architecture recap

There are two main contexts in which rpm-ostree is used: on compose servers to
generate OSTree commits from RPMs, and on client systems, which consume these
OSTree commits to provide transactional upgrades.

The [core architecture](architecture-core.md) document describes in details the
process by which RPMs are converted into OSTrees on both the compose server and
client systems in "hybrid mode".

## The rpm-ostree daemon

As described in the [administration](administrator-handbook.md) page, rpm-ostree is
the CLI program used on rpm-ostree-based systems to e.g. upgrade, rebase,
install packages, etc...

These operations follow a
client/[daemon](https://en.wikipedia.org/wiki/Daemon_(computing)) model where
the client talks to the daemon over
[D-Bus](https://www.freedesktop.org/wiki/Software/dbus/). The
reason for this architecture is primarily:
1. to ensure serialization/locking of system mutations
2. to make it easier for other clients to manage rpm-ostree-based systems (like
   [Cockpit](https://cockpit-project.org/))
3. to allow unprivileged users to make system mutations via polkit

The D-Bus API is defined here:
https://github.com/coreos/rpm-ostree/blob/main/src/daemon/org.projectatomic.rpmostree1.xml

The rpm-ostree daemon runs as a systemd service which owns the
`org.projectatomic.rpmostree1` name on the system D-Bus:

https://github.com/coreos/rpm-ostree/blob/main/src/daemon/rpm-ostreed.service.in

When a client wants to talk to the daemon, the D-Bus daemon starts up the
systemd service if it's not already running:

https://github.com/coreos/rpm-ostree/blob/main/src/daemon/org.projectatomic.rpmostree1.service.in

### Interacting with the daemon

When a user types `rpm-ostree upgrade`, at a high-level what happens is that it
reaches out to the daemon (starting it if it's not yet started) and calls the
`Upgrade()` D-Bus method. You can see its method signature in the XML definition
file linked above.

On the client side, this happens in `rpmostree-builtin-upgrade.cxx` where it
calls `rpmostree_os_call_upgrade_sync` function. This is an auto-generated
function (via `gdbus-codegen`) which in turns uses glib's D-Bus bindings to do
the actual D-Bus call.

On the daemon side, this call is handled in `os_handle_upgrade` (in
`rpmostreed-os.cxx`) which creates a new `DeployTransaction` object (via
`rpmostreed_transaction_new_deploy`). Transactions are how we ensure that only a
single mutation happens at a time, and allow tracking the state of the daemon's
processing of the request.

Transaction objects implement their own separate D-Bus API over an abstract
socket. The daemon returns to the client the address of the transaction's socket
and the client then connects to it as a D-Bus peer connection. (Notice how all
the methods in the XML return a `transaction_address`).

Once the client is connected to the transaction, it calls its `Start()` D-Bus
method, which invokes the `_execute()` function. In the case of
`DeployTransaction`, this is `deploy_transaction_execute()` in
`rpmostreed-transaction-types.cxx`.

The client then receives signals from the transaction to monitor the progress of
the operation (for example, everytime a message must be printed, a `Message`
signal is emitted).

The same analogous process occurs for other operations. For example, `rpm-ostree
kargs` creates a `KernelArgTransaction` object, which has its execution code in
`kernel_arg_transaction_execute`.

Once a transaction has finished, it emits the `Finished` signal, which tells the
client to stop waiting for more updates and disconnect.

### Polkit

The daemon integrates with
[polkit](https://www.freedesktop.org/software/polkit/docs/latest/polkit.8.html),
which is a framework for authorizing actions. For example, a local administrator
running as its unprivileged uid should be able to call `rpm-ostree upgrade`.

How this works is that the daemon converts D-Bus method calls like `Upgrade()`
into polkit actions in `os_authorize_method`. It then consults polkit to
determine whether the client should be permitted to execute the requested
actions.

We ship a base policy file which provide actions which should be allowed:

https://github.com/coreos/rpm-ostree/blob/main/src/daemon/org.projectatomic.rpmostree1.policy

Some distros may enhance this policy by shipping rules which dynamically
calculate authorization based on e.g. group membership. For example, in
Fedora:

https://src.fedoraproject.org/rpms/fedora-release/blob/rawhide/f/org.projectatomic.rpmostree1.rules

### Modifying the D-Bus API

Once a D-Bus API is stable, its signature cannot be modified because it would
break other D-Bus clients. This is why many methods have a catch-all `options`
parameter which allows for extending the API without changing its signature.

When adding new APIs, it's OK to have separate arguments for each parameter
considered essential to the operation, while keeping "auxiliary" parameters in
an `options` parameter (such as whether to reboot or not).
