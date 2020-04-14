dn=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=../common/libtest.sh
. "${dn}/../common/libtest.sh"

export repo=$PWD/repo
export treefile=$PWD/config/manifest.json
treeref=$(jq -r .ref < "${treefile}"); export treeref

# ensures workdir sticks around so we can debug if needed
export RPMOSTREE_PRESERVE_TMPDIR=1

pyedit() {
    local f=$1; shift
    # this is a bit underhanded; we read it in as yaml, since it can read json
    # too, but serialize back as json (which is also valid yaml). that way we
    # can use all these functions transparently with yaml and json treefiles
    cat >pyedit.py <<EOF
import sys, json, yaml
tf=yaml.safe_load(sys.stdin)
${1}
json.dump(tf, sys.stdout)
EOF
    python3 ./pyedit.py < "${f}" > "${f}.new"
    rm -f ./pyedit.py
    mv "${f}"{.new,}
}

treefile_pyedit() {
    pyedit "${treefile}" "$@"
}

treefile_set() {
    treefile_pyedit "tf['""$1""'] = $2"
}

treefile_del() {
    treefile_pyedit "
try:
  del tf['$1']
except KeyError:
  pass"
}

treefile_set_ref() {
    treefile_set ref "$@"
    rpm-ostree compose tree --print-only "${treefile}" > tmp.json
    treeref=$(jq -r .ref < tmp.json); export treeref
    rm tmp.json
}

treefile_append() {
    treefile_pyedit "
if '$1' not in tf:
  tf['$1'] = $2
else:
  tf['$1'] += $2"
}

# for tests that need direct control on rpm-ostree
export compose_base_argv="\
    --unified-core \
    --repo=${repo} \
    --cachedir=${test_tmpdir}/cache"

# and create this now for tests which only use `compose_base_argv`
mkdir -p cache

runcompose() {
  # keep this function trivial and the final command runasroot to mostly steer
  # clear of huge footgun of set -e not working in function calls in if-stmts
  runasroot rpm-ostree compose tree ${compose_base_argv} \
    --write-composejson-to=compose.json "${treefile}" "$@"
}

# NB: One difference from cosa here is we don't use `sudo`. I think there's an
# issue with sudo under parallel not getting signals propagated from the
# controlling terminal? Anyway, net result is we can end up with a bunch of
# rpm-ostree processes leaking in the background still running. So for now, one
# has to run this testsuite as root, or use unprivileged. XXX: to investigate.

runasroot() {
    if has_compose_privileges; then
        "$@"
    else
        runvm "$@"
    fi
}

# This function below was taken and adapted from coreos-assembler. We
# should look into sharing this code more easily.

runvm() {
    if [ ! -f tmp/cache.qcow2 ]; then
        mkdir -p tmp
        qemu-img create -f qcow2 tmp/cache.qcow2 8G
        LIBGUESTFS_BACKEND=direct virt-format --filesystem=xfs -a tmp/cache.qcow2
    fi

    echo "export test_tmpdir=${test_tmpdir}" > tmp/env
    # automatically proxy RPMOSTREE env vars
    $(env | (grep ^RPMOSTREE || :) | xargs -r echo export) >> tmp/env
    echo "$@" > tmp/cmd.sh

    #shellcheck disable=SC2086
    qemu-kvm \
        -nodefaults -nographic -m 2048 -no-reboot -cpu host \
        -kernel "${fixtures}/supermin.build/kernel" \
        -initrd "${fixtures}/supermin.build/initrd" \
        -netdev user,id=eth0,hostname=supermin \
        -device virtio-net-pci,netdev=eth0 \
        -device virtio-scsi-pci,id=scsi0,bus=pci.0,addr=0x3 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
        -drive if=none,id=drive-scsi0-0-0-0,snapshot=on,file="${fixtures}/supermin.build/root" \
        -device scsi-hd,bus=scsi0.0,channel=0,scsi-id=0,lun=0,drive=drive-scsi0-0-0-0,id=scsi0-0-0-0,bootindex=1 \
        -drive if=none,id=drive-scsi0-0-0-1,discard=unmap,file=tmp/cache.qcow2 \
        -device scsi-hd,bus=scsi0.0,channel=0,scsi-id=0,lun=1,drive=drive-scsi0-0-0-1,id=scsi0-0-0-1 \
        -virtfs local,id=cache,path="${fixtures}",security_model=none,mount_tag=cache \
        -virtfs local,id=testdir,path="${test_tmpdir}",security_model=none,mount_tag=testdir \
        -serial stdio -append "root=/dev/sda console=ttyS0 selinux=1 enforcing=0 autorelabel=1"

    if [ ! -f tmp/cmd.sh.rc ]; then
        fatal "Couldn't find rc file, something went terribly wrong!"
    fi
    return "$(cat tmp/cmd.sh.rc)"
}
