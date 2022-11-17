#!/bin/bash
set -euo pipefail

# freeze on a specific commit for tests for reproducibility and since it should
# always work to target older treefiles
FEDORA_COREOS_CONFIG_COMMIT=ce65013fcb9f10bfee1c7c1c27477c6c6ce676b3

dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd "$dn/.." && pwd)
commondir=$(cd "$dn/common" && pwd)
export topsrcdir commondir

# shellcheck source=common/libtest-core.sh
. "${commondir}/libtest.sh"

read -r -a tests <<< "$(filter_tests "${topsrcdir}/tests/compose")"
if [ ${#tests[*]} -eq 0 ]; then
  echo "No tests selected; mistyped filter?"
  exit 0
fi

JOBS=${JOBS:-$(ncpus)}

outputdir="${topsrcdir}/compose-logs"
fixtures="$(pwd)/compose-cache"

# re-use the same FCOS config and RPMs if it already exists
if [ ! -d compose-cache ]; then
  mkdir -p compose-cache

  # first, download all the RPMs into a directory
  echo "Caching test fixtures in compose-cache/"

  # Really want to use cosa fetch for this and just share the pkgcache repo.
  # Though for now we still need to support non-unified mode. Once we don't, we
  # can clean this up.
  pushd compose-cache
  git clone https://github.com/coreos/fedora-coreos-config config

  pushd config
  git checkout "${FEDORA_COREOS_CONFIG_COMMIT}"
  # we flatten the treefile to make it easier to manipulate in tests (we have
  # lots of tests that check for include logic already)
  rpm-ostree compose tree --print-only manifest.yaml > manifest.json
  rm manifest.yaml
  mv manifests/{passwd,group} .
  rm -rf manifests/
  # Also make sure to download glibc-all-langpacks which is no longer in FCOS by
  # default; we'll want it to test `install-langs`. This also means that we have
  # to add updates-archive to the repo list.
  # Also neuter OSTree layers; we don't re-implement cosa's auto-layering sugar
  curl -LO https://src.fedoraproject.org/rpms/fedora-repos/raw/f37/f/fedora-updates-archive.repo
  python3 -c '
import sys, json
y = json.load(sys.stdin)
y["repos"] += ["updates-archive"]
y["packages"] += ["glibc-all-langpacks"]
y["ostree-layers"] = []
json.dump(y, sys.stdout)' < manifest.json > manifest.json.new
  mv manifest.json{.new,}
  popd # config

  mkdir cachedir
  # we just need a repo so we can download stuff (but see note above about
  # sharing pkgcache repo in the future)
  ostree init --repo=repo --mode=archive
  rpm-ostree compose tree --unified-core --download-only-rpms --repo=repo \
    config/manifest.json --cachedir cachedir \
    --ex-lockfile config/manifest-lock.x86_64.json \
    --ex-lockfile config/manifest-lock.overrides.yaml
  rm -rf repo
  (cd cachedir && createrepo_c .)
  echo -e "[cache]\nbaseurl=$(pwd)/cachedir\ngpgcheck=0" > config/cache.repo

  pushd config
  python3 -c '
import sys, json
y = json.load(sys.stdin)
y["repos"] = ["cache"]
y["postprocess"] = []
y.pop("lockfile-repos", None)
json.dump(y, sys.stdout)' < manifest.json > manifest.json.new
  mv manifest.json{.new,}
  git add .
  git -c user.email="composetest@localhost.com" -c user.name="composetest" \
    commit -am 'modifications for tests'
  popd # config

  popd # compose-cache
fi

if ! has_compose_privileges; then
  pushd compose-cache

  # Unlike cosa, we don't need as much flexibility since we don't e.g. build
  # images. So just create the supermin appliance and root now so each test
  # doesn't have to build it.
  mkdir -p supermin.{prepare,build}
  # we just import the strict minimum here that rpm-ostree needs
  rpms="rpm-ostree bash rpm-build coreutils selinux-policy-targeted dhcp-client util-linux"
  # shellcheck disable=SC2086
  supermin --prepare --use-installed -o supermin.prepare $rpms
  # the reason we do a heredoc here is so that the var substition takes
  # place immediately instead of having to proxy them through to the VM
  cat > init <<EOF
#!/bin/bash
set -xeuo pipefail
export PATH=/usr/sbin:$PATH

mount -t proc /proc /proc
mount -t sysfs /sys /sys
mount -t devtmpfs devtmpfs /dev

LANG=C /sbin/load_policy -i

# load kernel module for 9pnet_virtio for 9pfs mount
/sbin/modprobe 9pnet_virtio

# need fuse module for rofiles-fuse/bwrap during post scripts run
/sbin/modprobe fuse

# set up networking
/usr/sbin/dhclient eth0

# set the umask so that anyone in the group can rwx
umask 002

# mount once somewhere predictable to source env vars
mount -t 9p -o rw,trans=virtio,version=9p2000.L testdir /mnt
source /mnt/tmp/env
umount /mnt

# we only need two dirs
mkdir -p "${fixtures}" "\${test_tmpdir}"
mount -t 9p -o ro,trans=virtio,version=9p2000.L cache "${fixtures}"
mount -t 9p -o rw,trans=virtio,version=9p2000.L testdir "\${test_tmpdir}"
mount /dev/sdb1 "\${test_tmpdir}/cache"
cd "\${test_tmpdir}"

# hack for non-unified mode
rm -rf cache/workdir && mkdir cache/workdir

rc=0
sh -x tmp/cmd.sh || rc=\$?
echo \$rc > tmp/cmd.sh.rc
if [ -b /dev/sdb1 ]; then
    /sbin/fstrim -v cache
fi
/sbin/reboot -f
EOF
  chmod a+x init
  tar -czf supermin.prepare/init.tar.gz --remove-files init
  supermin --build "${fixtures}/supermin.prepare" --size 5G -f ext2 -o supermin.build

  popd # compose-cache
fi

echo "Running ${#tests[*]} tests ${JOBS} at a time"

echo "Test results outputting to ${outputdir}/"

echo -n "${tests[*]}" | parallel -d' ' -j "${JOBS}" --line-buffer \
  "${topsrcdir}/tests/compose/runtest.sh" "${outputdir}" "${fixtures}"
