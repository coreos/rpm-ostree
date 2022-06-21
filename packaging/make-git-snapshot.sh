#!/usr/bin/env bash
set -xeuo pipefail

srcdir=$1
shift
PKG_VER=$1
shift
GITREV=$1
shift

TARFILE=${PKG_VER}.tar
TARFILE_TMP=$(pwd)/${TARFILE}.tmp

test -n "${srcdir}"
test -n "${PKG_VER}"
test -n "${GITREV}"

TOP=$(git rev-parse --show-toplevel)

echo "Archiving ${PKG_VER} at ${GITREV} to ${TARFILE_TMP}"
(cd ${TOP}; git archive --format=tar --prefix=${PKG_VER}/ ${GITREV}) > ${TARFILE_TMP}
ls -al ${TARFILE_TMP}
(cd ${TOP}; git submodule status) | while read line; do
    rev=$(echo ${line} | cut -f 1 -d ' '); path=$(echo ${line} | cut -f 2 -d ' ')
    echo "Archiving ${path} at ${rev}"
    (cd ${srcdir}/${path}; git archive --format=tar --prefix=${PKG_VER}/${path}/ ${rev}) > submodule.tar
    tar -A -f ${TARFILE_TMP} submodule.tar
    rm submodule.tar
done
disttmp=.dist-tmp
tmpd=${TOP}/$disttmp
trap cleanup EXIT
function cleanup () {
    if test -f ${tmpd}/.tmp; then
        rm "${tmpd}" -rf
    fi
}
# Run it now
cleanup
mkdir ${tmpd} && touch ${tmpd}/.tmp

vendor_cmd="cargo vendor-filterer"
target_vendor_cmd=$srcdir/target/cargo-vendor-filterer/bin/cargo-vendor-filterer
if test -x "${target_vendor_cmd}"; then
    vendor_cmd=${target_vendor_cmd}
fi

(cd ${tmpd}
 mkdir -p .cargo
 (cd ${TOP} && ${vendor_cmd} ${tmpd}/vendor | sed -e "s,^directory *=.*,directory = './vendor',") > .cargo/config
 cp ${TOP}/Cargo.lock .
 tar --owner=0 --group=0 --transform="s,^,${PKG_VER}/," -rf ${TARFILE_TMP} * .cargo/
 )

# And finally, vendor generated code.  See installdeps.sh
# and Makefile-rpm-ostree.am for more.
(cd ${srcdir}
 cp rpmostree-cxxrs{,-prebuilt}.h
 cp rpmostree-cxxrs{,-prebuilt}.cxx
 cp rust/cxx.h rust/cxx-prebuilt.h
 tar --owner=0 --group=0 --transform "s,^,${PKG_VER}/," -rf ${TARFILE_TMP} rpmostree-cxxrs-prebuilt.h rpmostree-cxxrs-prebuilt.cxx rust/cxx-prebuilt.h)

mv ${TARFILE_TMP} ${TARFILE}
