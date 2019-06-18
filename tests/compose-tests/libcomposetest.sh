dn=$(cd $(dirname $0) && pwd)
test_tmpdir=$(mktemp -d ${RPMOSTREE_TMPDIR_LOCATION:-/var/tmp}/rpm-ostree-compose-test.XXXXXX)
touch ${test_tmpdir}/.test
trap _cleanup_tmpdir EXIT
cd ${test_tmpdir}
. ${dn}/../common/libtest.sh

export repo=$(pwd)/repo
export repobuild=$(pwd)/repo-build

pyeditjson() {
    cat >editjson.py <<EOF
import sys,json
jd=json.load(sys.stdin)
${1}
json.dump(jd,sys.stdout)
EOF
    python3 ./editjson.py && rm -f ./editjson.py
}

pysetjsonmember() {
    pyeditjson "jd['"$1"'] = $2" < ${treefile} > ${treefile}.new && mv ${treefile}{.new,}
}

pyappendjsonmember() {
    pyeditjson "jd['"$1"'] += $2" < ${treefile} > ${treefile}.new && mv ${treefile}{.new,}
}

prepare_compose_test() {
    name=$1
    shift
    filetype=${1:-json}
    ostree --repo=${repo} init --mode=archive
    echo 'fsync=false' >> ${repo}/config
    ostree --repo=${repobuild} init --mode=bare-user
    echo 'fsync=false' >> ${repobuild}/config
    mkdir -p ${test_compose_datadir}/cache
    cp -r ${dn}/../composedata .
    # We use the local RPM package cache
    rm -f composedata/*.repo
    cat > composedata/fedora-local.repo <<EOF
[fedora-local]
baseurl=${test_compose_datadir}/fedora-local
enabled=1
gpgcheck=0
EOF
    export treefile=composedata/fedora-${name}.json
    export treeref=fedora/stable/x86_64/${name}
    pyeditjson 'jd["ref"] = "'${treeref}'"' < composedata/fedora-base.json > ${treefile}
    pysetjsonmember "repos" '["fedora-local"]' ${treefile}
    # FIXME extract from json
    if [ "${filetype}" = "yaml" ]; then
        python3 <<EOF
import json, yaml, sys
ifn="${treefile}"
ofn=ifn.replace('.json', '.yaml')
jd=json.load(open(ifn))
with open(ofn, "w") as f:
  yaml.safe_dump(jd, f)
EOF
        export treefile=composedata/fedora-${name}.yaml
    fi
    # The workdir will be cleaned up (or not) with the overall test dir
    if ! [ -d cache ]; then
        mkdir cache
        ostree --repo=cache/pkgcache-repo init --mode=bare-user
        echo 'fsync=false' >> cache/pkgcache-repo/config
        # Clone the pkgcache
        ostree --repo=cache/pkgcache-repo pull-local ${test_compose_datadir}/pkgcache-repo
        # And copy everything else to avoid locking issues
        cp -a --reflink=auto ${test_compose_datadir}/cache cache/cache
    fi
}

composejson=$(pwd)/compose.json
compose_base_argv="--cachedir=./cache --cache-only --repo ${repobuild} --write-composejson-to ${composejson}"
runcompose() {
    echo "$(date): starting compose"
    env RPMOSTREE_PRESERVE_TMPDIR=1 rpm-ostree compose tree ${compose_base_argv} ${treefile} "$@"
    commit=$(jq -r '.["ostree-commit"]' < "${composejson}")
    ostree --repo=${repo} pull-local ${repobuild} "${treeref:-${commit}}"
    echo "$(date): finished compose"
}

prepare_run_compose() {
    prepare_compose_test $1
    runcompose
}
