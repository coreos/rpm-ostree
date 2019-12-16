#!/bin/bash
set -euo pipefail

dn=$(cd "$(dirname "$0")" && pwd)
topsrcdir=$(cd "$dn/.." && pwd)
commondir=$(cd "$dn/common" && pwd)
export topsrcdir commondir

${dn}/vmcheck/overlay.sh

# https://github.com/coreos/coreos-assembler/pull/632
ncpus() {
  if ! grep -q kubepods /proc/1/cgroup; then
    # this might be a developer laptop; leave one cpu free to be nice
    echo $(($(nproc) - 1))
    return 0
  fi

  quota=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)
  period=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)
  if [[ ${quota} != -1 ]] && [[ ${period} -gt 0 ]]; then
    echo $(("${quota}" / "${period}"))
  fi

  # just fallback to 1
  echo 1
}

# Just match 1:1 the number of processing units available. Ideally, we'd also
# cap based on memory available to us, but that's notoriously difficult to do
# for containers (see:
# https://fabiokung.com/2014/03/13/memory-inside-linux-containers/). We make an
# assumption here that we have at least 1G of RAM we can use per CPU available
# to us.
nhosts=${NHOSTS:-$(ncpus)}

nselected=0
ntotal=0
tests=()
for tf in $(find "${topsrcdir}/tests/vmcheck/" -name 'test-*.sh' | sort); do
  ntotal=$((ntotal + 1))

  tfbn=$(basename "$tf" .sh)
  tfbn=" ${tfbn#test-} "
  if [ -n "${TESTS+ }" ]; then
    if [[ " $TESTS " != *$tfbn* ]]; then
      continue
    fi
  fi

  nselected=$((nselected + 1))
  tests+=(${tfbn})
done

echo "Running ${nselected} out of ${ntotal} tests ${nhosts} at a time"

outputdir="${topsrcdir}/vmcheck-logs"
echo "Test results outputting to ${outputdir}/"
if [ "${#tests[*]}" -gt 0 ]; then
  echo -n "${tests[*]}" | parallel -d' ' -j "${nhosts}" --line-buffer \
    "${topsrcdir}/tests/vmcheck/runtest.sh" "${outputdir}"
fi
