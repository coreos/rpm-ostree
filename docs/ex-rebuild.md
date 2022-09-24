---
parent: Experimental features
nav_order: 1
---

# Declarative system changes

For historical reasons, the build side of rpm-ostree is fully declarative; it
accepts [treefiles](treefiles.md) as input.  The client side is imperative, with a CLI and
DBus API.  

The goal of this feature is to unify things by supporting declarative
client-side changes.  The current implementation just exposes treefiles.  Instead of
typing e.g. `rpm-ostree install foo`, you can add treefiles into
`/etc/rpm-ostree/origin.d`, and invoke `rpm-ostree ex rebuild` to
*declaratively* reconcile the system to that state.

For more background on this, see https://github.com/coreos/rpm-ostree/issues/2326

## Example: Installing and removing packages as an atomic unit

```
$ mkdir -p /etc/rpm-ostree/origin.d
$ cat > /etc/rpm-ostree/origin.d/mycustom.yaml <<EOF
packages:
  - vim
  - NetworkManager-wifi
override-remove:
  - moby-engine
EOF
$ rpm-ostree ex rebuild
```

Previously, this required a CLI invocation like
```
$ rpm-ostree override remove moby-engine --install vim --install NetworkManager-wifi
```

which is comparatively awkward and not very discoverable.


## Note: Origin files are consumed after use

In the current design, these origin files are *deleted* after
they are successfully used.  The idea behind this is to avoid
adding leftover files into the generated "image" state.

This semantic is still being debated and may change in the future.



