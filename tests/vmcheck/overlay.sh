#!/bin/bash
set -euo pipefail

# Execute this code path on the host
if test -z "${INSIDE_VM:-}"; then
    . ${commondir}/libvm.sh
    vm_setup

    if ! vm_ssh_wait 30; then
      echo "ERROR: A running VM is required for 'make vmcheck'."
      exit 1
    fi

    vm_rsync

    # ✀✀✀ BEGIN selinux-policy hack (part 1) for
    # https://github.com/fedora-selinux/selinux-policy-contrib/pull/45
    selhack=selinux-tmp-hack
    if ! vm_cmd sesearch -A -s init_t -t install_t -c dbus | grep -q allow; then
      echo "Activating selinux-tmp-hack"
      d=$(mktemp -d)
      cat > $d/$selhack.te << 'EOF'
policy_module(selinux-tmp-hack, 1.0.0)
gen_require(`
      type install_t;
')
init_dbus_chat(install_t)
EOF
      make -C $d -f /usr/share/selinux/devel/Makefile $selhack.pp
      vm_send /var/roothome/sync $d/$selhack.pp
      rm -rf $d
    fi
    # ✀✀✀ END selinux-policy hack ✀✀✀

    vm_cmd env RPMOSTREE_TEST_NO_OVERLAY="${RPMOSTREE_TEST_NO_OVERLAY:-}" INSIDE_VM=1 /var/roothome/sync/tests/vmcheck/overlay.sh
    vm_reboot
    exit 0
fi

set -x

# And then this code path in the VM

# get details from the current default deployment
rpm-ostree status --json > json.txt
json_field() {
  field=$1; shift;
  python -c "
import sys, json;
deployment = json.load(open('json.txt'))['deployments'][0]
print deployment.get('$field', '')
exit()"
}
commit=$(json_field checksum)
origin=$(json_field origin)
version=$(json_field version)
timestamp=$(json_field timestamp)
[ -n "$timestamp" ]
timestamp=$(date -d "@$timestamp" "+%b %d %Y")
rm -f json.txt

if [[ -z $commit ]] || ! ostree rev-parse $commit; then
  echo "Error while determining current commit" >&2
  exit 1
fi

cd /ostree/repo/tmp
rm vmcheck -rf
ostree checkout $commit vmcheck --fsync=0
rm vmcheck/etc -rf

# Now, overlay our built binaries & config files, unless
# explicitly requested not to (with the goal of testing the
# tree shipped as is with our existing tests).
if test -z "${RPMOSTREE_TEST_NO_OVERLAY}"; then
    INSTTREE=/var/roothome/sync/insttree
    rsync -rlv $INSTTREE/usr/ vmcheck/usr/
    rsync -rlv $INSTTREE/etc/ vmcheck/usr/etc/
else
    echo "Skipping overlay of built rpm-ostree"
fi

## ✀✀✀ BEGIN selinux-policy hack (part 2) for
## https://github.com/fedora-selinux/selinux-policy-contrib/pull/45
selhack=selinux-tmp-hack
pp=/var/roothome/sync/$selhack.pp
if [ -f $pp ]; then
  seld=usr/share/selinux/packages/$selhack
  mkdir -p vmcheck/$seld
  cp $pp vmcheck/$seld
  mkdir vmcheck/var/tmp # bwrap wrapper will mount tmpfs there
  /var/roothome/sync/scripts/bwrap-script-shell.sh /ostree/repo/tmp/vmcheck \
    semodule -v -n -i /$seld/$selhack.pp
fi
## ✀✀✀ END selinux-policy hack ✀✀✀

# ✀✀✀ BEGIN hack to get --keep-metadata
if ! ostree commit --help | grep -q -e --keep-metadata; then
  # this is fine, rsync doesn't modify in place
  mount -o rw,remount /usr
  # don't overwrite /etc/ to not mess up 3-way merge
  rsync -rlv --exclude '/etc/' vmcheck/usr/ /usr/
fi
# ✀✀✀ END hack to get --keep-metadata ✀✀✀

# if the commit already has pkglist metadata (i.e. the tree was composed with at
# least v2018.1), make sure it gets preserved, because it's useful for playing
# around (but note it's not a requirement for our tests)
commit_opts=
if ostree show $commit --raw | grep -q rpmostree.rpmdb.pkglist; then
  commit_opts="${commit_opts} --keep-metadata=rpmostree.rpmdb.pkglist"
fi

source_opt= # make this its own var since it contains spaces
if [ $origin != vmcheck ]; then
  source_title="${origin}"
  if [ -n "$version" ]; then
    source_title="${source_title} (${version}; $timestamp)"
  else
    source_title="${source_title} ($timestamp)"
  fi
  source_opt="--add-metadata-string=ostree.source-title=Dev overlay on ${source_title}"
  commit_opts="${commit_opts} --add-metadata-string=rpmostree.original-origin=${origin}"
else
  source_opt="--keep-metadata=ostree.source-title"
  commit_opts="${commit_opts} --keep-metadata=rpmostree.original-origin"
fi

ostree commit --parent=$commit -b vmcheck --consume --no-bindings \
       --link-checkout-speedup ${commit_opts} "${source_opt}" \
       --selinux-policy=vmcheck --tree=dir=vmcheck

ostree admin deploy vmcheck
