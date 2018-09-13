dn=$(cd $(dirname $0) && pwd)
test_tmpdir=$(mktemp -d /var/tmp/rpm-ostree-compose-test.XXXXXX)
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
    python ./editjson.py && rm -f ./editjson.py
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
baseurl=${test_compose_datadir}/cache
enabled=1
gpgcheck=0
EOF
    export treefile=composedata/fedora-${name}.json
    pyeditjson "jd['ref'] += \"/${name}\"" < composedata/fedora-base.json > ${treefile}
    pysetjsonmember "repos" '["fedora-local"]' ${treefile}
    # FIXME extract from json
    export treeref=fedora/stable/x86_64/${name}
    if [ "${filetype}" = "yaml" ]; then
        python <<EOF
import json, yaml, sys
ifn="${treefile}"
ofn=ifn.replace('.json', '.yaml')
jd=json.load(open(ifn))
with open(ofn, "w") as f:
  f.write(yaml.dump(jd))
EOF
        export treefile=composedata/fedora-${name}.yaml
    fi
}

compose_base_argv="--repo ${repobuild}"
runcompose() {
    echo "$(date): starting compose"
    rpm-ostree compose tree ${compose_base_argv} ${treefile} "$@"
    ostree --repo=${repo} pull-local ${repobuild}
    echo "$(date): finished compose"
}

prepare_run_compose() {
    prepare_compose_test $1
    runcompose
}
