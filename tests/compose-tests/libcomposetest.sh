dn=$(cd $(dirname $0) && pwd)
. ${dn}/../common/libtest.sh
test_tmpdir=$(mktemp -d /var/tmp/rpm-ostree-compose-test.XXXXXX)
touch ${test_tmpdir}/.test
trap _cleanup_tmpdir EXIT
cd ${test_tmpdir}

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

prepare_compose_test() {
    name=$1
    shift
    cp -r ${dn}/../composedata .
    export treefile=composedata/fedora-${name}.json
    pyeditjson "jd['ref'] += \"/${name}\"" < composedata/fedora-base.json > ${treefile}
    # FIXME extract from json
    export treeref=fedora/25/x86_64/${name}
}

runcompose() {
    rpm-ostree compose --repo=${repobuild} tree --cache-only --cachedir=${test_compose_datadir}/cache ${treefile}
    ostree --repo=${repo} pull-local ${repobuild}
}

prepare_run_compose() {
    prepare_compose_test $1
    runcompose
}
