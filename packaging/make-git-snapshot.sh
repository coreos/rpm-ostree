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

(cd ${tmpd}
 mkdir -p .cargo vendor
 (cd ${TOP} && cargo vendor ${tmpd}/vendor | sed -e "s,^directory *=.*,directory = './vendor',") > .cargo/config
 cp ${TOP}/Cargo.lock .
 # Filter out bundled libcurl and systemd; we always want the pkgconfig ones
 for crate_subdir in curl-sys/curl \
                     libz-sys/src/zlib \
                     systemd/libsystemd-sys/systemd \
                     libsystemd-sys/systemd; do
   rm -rf vendor/$crate_subdir
   python3 -c '
import json, sys
crate, subdir = sys.argv[1].split("/", 1)
checksum_file = ("vendor/%s/.cargo-checksum.json" % crate)
j = json.load(open(checksum_file))
j["files"] = {f:c for f, c in j["files"].items() if not f.startswith(subdir)}
open(checksum_file, "w").write(json.dumps(j))' $crate_subdir
 done
 tar --transform="s,^,${PKG_VER}/," -rf ${TARFILE_TMP} * .cargo/
 )

# And finally, vendor generated code.  See installdeps.sh
# and Makefile-rpm-ostree.am for more.
(cd ${srcdir}
 cp rpmostree-cxxrs{,-prebuilt}.h
 cp rpmostree-cxxrs{,-prebuilt}.cxx
 cp rust/cxx.h rust/cxx-prebuilt.h
 tar --transform "s,^,${PKG_VER}/," -rf ${TARFILE_TMP} rpmostree-cxxrs-prebuilt.h rpmostree-cxxrs-prebuilt.cxx rust/cxx-prebuilt.h)

mv ${TARFILE_TMP} ${TARFILE}
