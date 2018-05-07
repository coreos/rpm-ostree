# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

# Have we already been sourced?
if test -n "${LIBTEST_SH:-}"; then
  # would be good to know when it happens
  echo "INFO: Skipping subsequent sourcing of libtest.sh"
  return
fi
LIBTEST_SH=1

self="$(realpath $0)"
if test -z "${SRCDIR:-}"; then
    SRCDIR=${topsrcdir}/tests
fi
. ${SRCDIR}/common/libtest-core.sh

UPDATEINFO=${SRCDIR}/utils/updateinfo

for bin in jq; do
    if ! command -v $bin >/dev/null; then
        (echo ${bin} is required to execute tests 1>&2; exit 1)
    fi
done

_cleanup_tmpdir () {
    if test -z "${TEST_SKIP_CLEANUP:-}"; then
	if test -f ${test_tmpdir}/.test; then
           rm ${test_tmpdir} -rf
	fi
    else
	echo "Skipping cleanup of ${test_tmpdir}"
    fi
}

# Create a tmpdir if we're running as a local test (i.e. through `make check`)
# or as a `vmcheck` test, which also needs some scratch space on the host.
if ( test -n "${UNINSTALLEDTESTS:-}" || test -n "${VMTESTS:-}" ) && ! test -f $PWD/.test; then
   test_tmpdir=$(mktemp -d /tmp/test-$(basename ${topsrcdir}).XXXXXX)
   touch ${test_tmpdir}/.test
   trap _cleanup_tmpdir EXIT
   cd ${test_tmpdir}
fi
if test -n "${UNINSTALLEDTESTS:-}"; then
    export PATH=${builddir}:${PATH}
fi

test_tmpdir=$(pwd)
echo "Using tmpdir ${test_tmpdir}"

export G_DEBUG=fatal-warnings

# Don't flag deployments as immutable so that test harnesses can
# easily clean up.
export OSTREE_SYSROOT_DEBUG=mutable-deployments

# See comment in ot-builtin-commit.c and https://github.com/ostreedev/ostree/issues/758
# Also keep this in sync with the bits in libostreetest.c
case $(stat -f --printf '%T' /) in
    overlayfs)
        echo "overlayfs found; enabling OSTREE_NO_XATTRS"
        ostree --version
        export OSTREE_SYSROOT_DEBUG="${OSTREE_SYSROOT_DEBUG},no-xattrs"
        export OSTREE_NO_XATTRS=1 ;;
    *) echo "Not using overlayfs" ;;
esac

export TEST_GPG_KEYID="472CDAFA"

# GPG when creating signatures demands a writable
# homedir in order to create lockfiles.  Work around
# this by copying locally.
echo "Copying gpghome to ${test_tmpdir}"
cp -a "${SRCDIR}/gpghome" ${test_tmpdir}
chmod -R u+w "${test_tmpdir}"
export TEST_GPG_KEYHOME=${test_tmpdir}/gpghome
export OSTREE_GPG_HOME=${test_tmpdir}/gpghome/trusted

if test -n "${OT_TESTS_DEBUG:-}"; then
    set -x
fi

if test -n "${OT_TESTS_VALGRIND:-}"; then
    CMD_PREFIX="env G_SLICE=always-malloc valgrind -q --leak-check=full --num-callers=30 --suppressions=${SRCDIR}/ostree-valgrind.supp"
fi

# A wrapper which also possibly disables xattrs for CI testing
ostree_repo_init() {
    repo=$1
    shift
    ${CMD_PREFIX} ostree --repo=${repo} init "$@"
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> ${repo}/config
    fi
}

setup_test_repository () {
    mode=$1
    shift

    oldpwd=`pwd`

    cd ${test_tmpdir}
    if test -n "${mode}"; then
        ostree_repo_init repo --mode=${mode}
    else
        ostree_repo_init repo
    fi
    ot_repo="--repo=$(pwd)/repo"
    export OSTREE="${CMD_PREFIX} ostree ${ot_repo}"

    cd ${test_tmpdir}/files
    $OSTREE commit -b test2 -s "Test Commit 1" -m "Commit body first"

    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    mkdir baz/deeper
    echo hi > baz/deeper/ohyeah
    ln -s nonexistent baz/alink
    mkdir baz/another/
    echo x > baz/another/y

    cd ${test_tmpdir}/files
    $OSTREE commit -b test2 -s "Test Commit 2" -m "Commit body second"
    $OSTREE fsck -q

    cd $oldpwd
}

run_temp_webserver() {
    env PYTHONUNBUFFERED=1 setsid python -m SimpleHTTPServer 0 >${test_tmpdir}/httpd-output &
    for x in $(seq 50); do
        if test -e ${test_tmpdir}/httpd-output; then
            sed -e 's,Serving HTTP on 0.0.0.0 port \([0-9]*\) \.\.\.,\1,' < ${test_tmpdir}/httpd-output > ${test_tmpdir}/httpd-port
            if ! cmp ${test_tmpdir}/httpd-output ${test_tmpdir}/httpd-port 1>/dev/null; then
                break
            fi
        fi
        sleep 0.1
    done
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
}

setup_fake_remote_repo1() {
    mode=$1
    args=$2
    shift
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    ostree_repo_init gnomerepo --mode=$mode
    mkdir gnomerepo-files
    cd gnomerepo-files 
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit --add-metadata-string version=3.0 -b main -s "A remote commit" -m "Some Commit body"
    mkdir baz/deeper
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit --add-metadata-string version=3.1 -b main -s "Add deeper"
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit --add-metadata-string version=3.2 -b main -s "The rest"
    cd ..
    rm -rf gnomerepo-files
    
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir}/ostree-srv ostree
    run_temp_webserver
    cd ${oldpwd} 

    export OSTREE="ostree --repo=repo"
}

setup_os_boot_syslinux() {
    # Stub syslinux configuration
    mkdir -p sysroot/boot/loader.0
    ln -s loader.0 sysroot/boot/loader
    touch sysroot/boot/loader/syslinux.cfg
    # And a compatibility symlink
    mkdir -p sysroot/boot/syslinux
    ln -s ../loader/syslinux.cfg sysroot/boot/syslinux/syslinux.cfg
}

setup_os_boot_uboot() {
    # Stub U-Boot configuration
    mkdir -p sysroot/boot/loader.0
    ln -s loader.0 sysroot/boot/loader
    touch sysroot/boot/loader/uEnv.txt
    # And a compatibility symlink
    ln -s loader/uEnv.txt sysroot/boot/uEnv.txt
}

setup_os_repository () {
    mode=$1
    bootmode=$2
    shift

    oldpwd=`pwd`

    cd ${test_tmpdir}
    mkdir testos-repo
    if test -n "$mode"; then
	      ostree_repo_init testos-repo --mode=${mode}
    else
	      ostree_repo_init testos-repo
    fi

    cd ${test_tmpdir}
    mkdir osdata
    cd osdata
    mkdir -p boot usr/bin usr/lib/modules/3.6.0 usr/share usr/etc
    echo "a kernel" > boot/vmlinuz-3.6.0
    echo "an initramfs" > boot/initramfs-3.6.0
    bootcsum=$(cat boot/vmlinuz-3.6.0 boot/initramfs-3.6.0 | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    mv boot/vmlinuz-3.6.0 boot/vmlinuz-3.6.0-${bootcsum}
    mv boot/initramfs-3.6.0 boot/initramfs-3.6.0-${bootcsum}

    echo "an executable" > usr/bin/sh
    echo "some shared data" > usr/share/langs.txt
    echo "a library" > usr/lib/libfoo.so.0
    ln -s usr/bin bin
cat > usr/etc/os-release <<EOF
NAME=TestOS
VERSION=42
ID=testos
VERSION_ID=42
PRETTY_NAME="TestOS 42"
EOF
    echo "a config file" > usr/etc/aconfigfile
    mkdir -p usr/etc/NetworkManager
    echo "a default daemon file" > usr/etc/NetworkManager/nm.conf
    mkdir -p usr/etc/testdirectory
    echo "a default daemon file" > usr/etc/testdirectory/test

    ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmaster/x86_64-runtime -s "Build"

    # Ensure these commits have distinct second timestamps
    sleep 2
    echo "a new executable" > usr/bin/sh
    ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.10 -b testos/buildmaster/x86_64-runtime -s "Build"

    cd ${test_tmpdir}
    cp -a osdata osdata-devel
    cd osdata-devel
    mkdir -p usr/include
    echo "a development header" > usr/include/foo.h
    ostree --repo=${test_tmpdir}/testos-repo commit --add-metadata-string version=1.0.9 -b testos/buildmaster/x86_64-devel -s "Build"

    ostree --repo=${test_tmpdir}/testos-repo fsck -q

    cd ${test_tmpdir}
    # sysroot dir already made by setup-session.sh
    ostree admin --sysroot=sysroot init-fs sysroot
    if test -n "${OSTREE_NO_XATTRS:-}"; then
        echo -e 'disable-xattrs=true\n' >> sysroot/ostree/repo/config
    fi
    ostree admin --sysroot=sysroot os-init testos

    case $bootmode in
        "syslinux")
	    setup_os_boot_syslinux
            ;;
        "uboot")
	    setup_os_boot_uboot
            ;;
    esac

    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    ln -s ${test_tmpdir} ostree
    run_temp_webserver
    cd ${oldpwd} 
}

os_repository_new_commit ()
{
    boot_checksum_iteration=$1
    content_iteration=$2
    echo "BOOT ITERATION: $boot_checksum_iteration"
    if test -z "$boot_checksum_iteration"; then
	boot_checksum_iteration=0
    fi
    if test -z "$content_iteration"; then
	content_iteration=0
    fi
    cd ${test_tmpdir}/osdata
    rm boot/*
    echo "new: a kernel ${boot_checksum_iteration}" > boot/vmlinuz-3.6.0
    echo "new: an initramfs ${boot_checksum_iteration}" > boot/initramfs-3.6.0
    bootcsum=$(cat boot/vmlinuz-3.6.0 boot/initramfs-3.6.0 | sha256sum | cut -f 1 -d ' ')
    export bootcsum
    mv boot/vmlinuz-3.6.0 boot/vmlinuz-3.6.0-${bootcsum}
    mv boot/initramfs-3.6.0 boot/initramfs-3.6.0-${bootcsum}

    echo "a new default config file" > usr/etc/a-new-default-config-file
    mkdir -p usr/etc/new-default-dir
    echo "a new default dir and file" > usr/etc/new-default-dir/moo

    echo "content iteration ${content_iteration}" > usr/bin/content-iteration

    version=$(date "+%Y%m%d.${content_iteration}")
    echo "version: $version"

    ostree --repo=${test_tmpdir}/testos-repo commit  --add-metadata-string "version=${version}" -b testos/buildmaster/x86_64-runtime -s "Build"
    cd ${test_tmpdir}
}

check_root_test ()
{
    if test "$(id -u)" != "0"; then
       skip "This test requires uid 0"
    fi
    if ! capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin'; then
        skip "No CAP_SYS_ADMIN in bounding set"
    fi
}

ensure_dbus ()
{
    if test -z "$RPMOSTREE_USE_SESSION_BUS"; then
        exec "$topsrcdir/tests/utils/setup-session.sh" "$self"
    fi
}

# https://github.com/ostreedev/ostree/commit/47b4dd1b38e422254afa67756873957c25aeab6d
# Unfortunately, introspection uses dlopen(), which doesn't quite
# work when the DSO is compiled with ASAN but the outer executable
# isn't.
skip_one_with_asan () {
    if test -n "${BUILDOPT_ASAN:-}"; then
        echo "ok # SKIP - built with ASAN"
        return 0
    else
        return 1
    fi
}

assert_status_file_jq() {
    status_file=$1; shift
    for expression in "$@"; do
        if ! jq -e "${expression}" >/dev/null < $status_file; then
            jq . < $status_file | sed -e 's/^/# /' >&2
            echo 1>&2 "${expression} failed to match in $status_file"
            exit 1
        fi
    done
}

assert_status_jq() {
    rpm-ostree status --json > status.json
    assert_status_file_jq status.json "$@"
}

get_obj_path() {
  repo=$1; shift
  csum=$1; shift
  objtype=$1; shift
  echo "${repo}/objects/${csum:0:2}/${csum:2}.${objtype}"
}

uinfo_cmd() {
    $UPDATEINFO --repo "${test_tmpdir}/yumrepo" "$@"
}

# builds a new RPM and adds it to the testdir's repo
# $1 - name
# $2+ - optional, treated as directive/value pairs
build_rpm() {
    local name=$1; shift
    # Unset, not zero https://github.com/projectatomic/rpm-ostree/issues/349
    local epoch=""
    local version=1.0
    local release=1
    local arch=x86_64

    mkdir -p $test_tmpdir/yumrepo/{specs,packages}
    local spec=$test_tmpdir/yumrepo/specs/$name.spec

    # write out the header
    cat > $spec << EOF
Name: $name
Summary: %{name}
License: GPLv2+
EOF

    local build= install= files= pretrans= pre= post= posttrans= post_args=
    local verifyscript= uinfo=
    local transfiletriggerin= transfiletriggerin_patterns=
    local transfiletriggerin2= transfiletriggerin2_patterns=
    local transfiletriggerun= transfiletriggerun_patterns=
    while [ $# -ne 0 ]; do
        local section=$1; shift
        local arg=$1; shift
        case $section in
        requires)
            echo "Requires: $arg" >> $spec;;
        provides)
            echo "Provides: $arg" >> $spec;;
        conflicts)
            echo "Conflicts: $arg" >> $spec;;
        post_args)
            post_args="$arg";;
        version|release|epoch|arch|build|install|files|pretrans|pre|post|posttrans|verifyscript|uinfo)
            declare $section="$arg";;
        transfiletriggerin)
            transfiletriggerin_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerin2)
            transfiletriggerin2_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerun)
            transfiletriggerun_patterns="$arg";
            declare $section="$1"; shift;;
        *)
            assert_not_reached "unhandled section $section";;
        esac
    done

    cat >> $spec << EOF
Version: $version
Release: $release
${epoch:+Epoch: $epoch}
BuildArch: $arch

%description
%{summary}

# by default, we create a /usr/bin/$name script which just outputs $name
%build
echo -e "#!/bin/sh\necho $name-$version-$release.$arch" > $name
chmod a+x $name
$build

${pretrans:+%pretrans}
$pretrans

${pre:+%pre}
$pre

${post:+%post} ${post_args}
$post

${posttrans:+%posttrans}
$posttrans

${transfiletriggerin:+%transfiletriggerin -- ${transfiletriggerin_patterns}}
$transfiletriggerin

${transfiletriggerin2:+%transfiletriggerin -- ${transfiletriggerin2_patterns}}
$transfiletriggerin2

${transfiletriggerun:+%transfiletriggerun -- ${transfiletriggerun_patterns}}
$transfiletriggerun

${verifyscript:+%verifyscript}
$verifyscript

%install
mkdir -p %{buildroot}/usr/bin
install $name %{buildroot}/usr/bin
$install

%clean
rm -rf %{buildroot}

%files
/usr/bin/$name
$files
EOF

    # because it'd be overkill to set up mock for this, let's just fool
    # rpmbuild using setarch
    local buildarch=$arch
    if [ "$arch" == "noarch" ]; then
        buildarch=$(uname -m)
    fi

    (cd $test_tmpdir/yumrepo/specs &&
     setarch $buildarch rpmbuild --target $arch -ba $name.spec \
        --define "_topdir $PWD" \
        --define "_sourcedir $PWD" \
        --define "_specdir $PWD" \
        --define "_builddir $PWD/.build" \
        --define "_srcrpmdir $PWD" \
        --define "_rpmdir $test_tmpdir/yumrepo/packages" \
        --define "_buildrootdir $PWD")
    # use --keep-all-metadata to retain previous updateinfo
    (cd $test_tmpdir/yumrepo &&
     createrepo_c --no-database --update --keep-all-metadata .)
    # convenience function to avoid follow-up add-pkg
    if [ -n "$uinfo" ]; then
        uinfo_cmd add-pkg $uinfo $name 0 $version $release $arch
    fi
    if test '!' -f $test_tmpdir/yumrepo.repo; then
        cat > $test_tmpdir/yumrepo.repo.tmp << EOF
[test-repo]
name=test-repo
baseurl=file:///$PWD/yumrepo
EOF
        mv $test_tmpdir/yumrepo.repo{.tmp,}
    fi
}

# build an SELinux package ready to be installed -- really, we just support file
# context entries for now, though it's enough to test policy changes
# $1 - package name
# $2+ - pairs of file path regex and context types
build_selinux_rpm() {
    local name=$1; shift

    local module_dir=$test_tmpdir/policies/$name
    mkdir -p $module_dir
    local module_te=$module_dir/$name.te
    local module_fc=$module_dir/$name.fc

    # also declare a type associated with the app; any non-trivial SELinux
    # package will have some type enforcement rules that will require policy
    # recompilation
    cat > $module_te <<EOF
policy_module(${name}, 1.0.0)
type ${name}_t;
EOF

    echo -n "" > $module_fc

    while [ $# -ne 0 ]; do
        local fc_regex=$1; shift
        local fc_type=$1; shift
        local fc_label="gen_context(system_u:object_r:$fc_type,s0)"
        echo "$fc_regex -- $fc_label" >> $module_fc
    done

    make -C $module_dir -f /usr/share/selinux/devel/Makefile $name.pp

    # We point the spec file directly at our pp. This is a bit underhanded, but
    # it's cleaner than copying it in and using e.g. Source0 or something.
    local pp=$(realpath $module_dir/$name.pp)
    local install_dir=/usr/share/selinux/packages
    build_rpm $name install "mkdir -p %{buildroot}${install_dir}
                             install ${pp} %{buildroot}${install_dir}" \
                    post "semodule -n -i ${install_dir}/${name}.pp" \
                    files "${install_dir}/${name}.pp"
}
