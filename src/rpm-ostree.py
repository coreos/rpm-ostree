#!/usr/bin/env python
#
# Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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

import os
import re
import sys
import optparse
import time
import shutil
import subprocess

from gi.repository import GLib
from gi.repository import Gio
from gi.repository import OSTree

os_release_data = {}
opts = None
args = None

def ensuredir(path):
    if not os.path.isdir(path):
        os.makedirs(path)

def rmrf(path):
    shutil.rmtree(path, ignore_errors=True)

def feed_checksum(checksum, stream):
    b = stream.read(8192)
    while b != '':
        checksum.update(b)
        b = stream.read(8192)

def _find_current_origin_refspec():
    dpath = '/ostree/deploy/%s/deploy' % (os_release_data['ID'], )
    for name in os.listdir(dpath):
        if name.endswith('.origin'):
            for line in open(os.path.join(dpath, name)):
                if line.startswith('refspec='):
                    return line[len('refspec='):]
    return None

def _initfs(targetroot):
    os.makedirs(targetroot)
    for d in ['dev', 'proc', 'run', 'sys', 'var']:
        os.mkdir(os.path.join(targetroot, d))

    # Special ostree mount
    os.mkdir(os.path.join(targetroot, 'sysroot'))

    # Some FHS targets; these all live in /var
    for (target, name) in [('var/opt', 'opt'),
                           ('var/srv', 'srv'),
                           ('var/mnt', 'mnt'),
                           ('var/roothome', 'root'),
                           ('var/home', 'home'),
                           ('run/media', 'media'),
                           ('sysroot/ostree', 'ostree'),
                           ('sysroot/tmp', 'tmp')]:
        os.symlink(target, os.path.join(targetroot, name))


def _clone_current_root_to_yumroot(yumroot):
    _initfs(yumroot)
    for d in ['boot', 'usr', 'etc', 'var/lib/rpm', 'var/cache/yum']:
        destdir = os.path.join(yumroot, d)
        rmrf(destdir)
        ensuredir(os.path.dirname(destdir))
        subprocess.check_call(['cp', '--reflink=auto', '-a',
                               '/' + d,
                               destdir])

def replace_nsswitch(target_usretc):
    nsswitch_conf = os.path.join(target_usretc, 'nsswitch.conf')
    f = open(nsswitch_conf)
    newf = open(nsswitch_conf + '.tmp', 'w')
    passwd_re = re.compile(r'^passwd:\s+files(.*)$')
    group_re = re.compile(r'^group:\s+files(.*)$')
    for line in f:
        match = passwd_re.match(line)
        if match and line.find('altfiles') == -1:
            newf.write('passwd: files altfiles' + match.group(1) + '\n')
            continue
        match = group_re.match(line)
        if match and line.find('altfiles') == -1:
            newf.write('group: files altfiles' + match.group(1) + '\n')
            continue
        newf.write(line)
    f.close()
    newf.close()
    os.rename(nsswitch_conf + '.tmp', nsswitch_conf)

def do_kernel_prep(yumroot, logs_lookaside):
    bootdir = os.path.join(yumroot, 'boot')
    kernel_path = None
    for name in os.listdir(bootdir):
        if name.startswith('vmlinuz-'):
            kernel_path = os.path.join(bootdir, name)
            break
        elif name.startswith('initramfs-'):
            # If somehow the %post generated an initramfs, blow it
            # away - we take over that role.
            initramfs_path = os.path.join(bootdir, name)
            print "Removing RPM-generated " + initramfs_path
            rmrf(initramfs_path)

    if kernel_path is None:
        raise ValueError("Failed to find vmlinuz- in " + yum_boot)

    kname = os.path.basename(kernel_path)
    kver = kname[kname.find('-') + 1:]
    print "Kernel version is " + kver
           
    # OSTree will take care of this
    loaderdir = os.path.join(bootdir, 'loader')
    rmrf(loaderdir)

    args = ['chroot', yumroot, 'depmod', kver]
    print "Running: %s" % (subprocess.list2cmdline(args), )
    subprocess.check_call(args)

    # Copy of code from gnome-continuous; yes, we hardcode
    # the machine id for now, because distributing pre-generated
    # initramfs images with dracut/systemd at the moment
    # effectively requires this.
    # http://lists.freedesktop.org/archives/systemd-devel/2013-July/011770.html
    print "Hardcoding machine-id"
    f = open(os.path.join(yumroot, 'etc', 'machine-id'), 'w')
    f.write('45bb3b96146aa94f299b9eb43646eb35\n')
    f.close()

    args = ['chroot', yumroot,
            'dracut', '-v', '--tmpdir=/tmp',
            '-f', '/tmp/initramfs.img', kver];
    print "Running: %s" % (subprocess.list2cmdline(args), )
    subprocess.check_call(args)
    
    initramfs_path = os.path.join(yumroot, 'tmp', 'initramfs.img')
    if not os.path.exists(initramfs_path):
        raise ValueError("Failed to find " + initramfs_path)

    os.rename(initramfs_path, os.path.join(bootdir, 'initramfs-yumostree.img'))
    varlog_dracut_path = os.path.join(yumroot, 'var', 'log', 'dracut.log')
    if os.path.exists(varlog_dracut_path):
        os.rename(varlog_dracut_path, os.path.join(logs_lookaside, 'dracut.log'))
    
def _create_rootfs_from_yumroot_content(targetroot, yumroot):
    """Prepare a root filesystem, taking mainly the contents of /usr from yumroot"""

    _initfs(targetroot)

    # We take /usr from the yum content
    os.rename(os.path.join(yumroot, 'usr'), os.path.join(targetroot, 'usr'))
    # Plus the RPM database goes in usr/share/rpm
    legacyrpm_path = os.path.join(yumroot, 'var/lib/rpm')
    newrpm_path = os.path.join(targetroot, 'usr/share/rpm')
    if not os.path.isdir(newrpm_path):
        os.rename(legacyrpm_path, newrpm_path)

    # Except /usr/local -> ../var/usrlocal
    target_usrlocal = os.path.join(targetroot, 'usr/local')
    if not os.path.islink(target_usrlocal):
        rmrf(target_usrlocal)
        os.symlink('../var/usrlocal', target_usrlocal)
    target_usretc = os.path.join(targetroot, 'usr/etc')
    rmrf(target_usretc)
    os.rename(os.path.join(yumroot, 'etc'), target_usretc)

    # Move boot, but rename the kernel/initramfs to have a checksum
    targetboot = os.path.join(targetroot, 'boot')
    os.rename(os.path.join(yumroot, 'boot'), targetboot)
    kernel = None
    initramfs = None
    for name in os.listdir(targetboot):
        if name.startswith('vmlinuz-'):
            kernel = os.path.join(targetboot, name)
        elif name.startswith('initramfs-'):
            initramfs = os.path.join(targetboot, name)

    assert (kernel is not None and initramfs is not None)
    
    checksum = GLib.Checksum.new(GLib.ChecksumType.SHA256)
    f = open(kernel)
    feed_checksum(checksum, f)
    f.close()
    f = open(initramfs)
    feed_checksum(checksum, f)
    f.close()

    bootcsum = checksum.get_string()
    
    os.rename(kernel, kernel + '-' + bootcsum)
    os.rename(initramfs, initramfs + '-' + bootcsum)

    # Also carry along toplevel compat links
    for name in ['lib', 'lib64', 'bin', 'sbin']:
        src = os.path.join(yumroot, name)
        if os.path.islink(src):
            os.rename(src, os.path.join(targetroot, name))

    target_tmpfilesd = os.path.join(targetroot, 'usr/lib/tmpfiles.d')
    ensuredir(target_tmpfilesd)
    shutil.copy(os.path.join(PKGLIBDIR, 'tmpfiles-gnome-ostree.conf'), target_tmpfilesd)

def yuminstall(yumroot, packages):
    yumargs = ['yum', '-y', '--releasever=%s' % (opts.os_version, ), '--nogpg', '--setopt=keepcache=1', '--installroot=' + yumroot, '--disablerepo=*']
    yumargs.extend(map(lambda x: '--enablerepo=' + x, opts.enablerepo))
    yumargs.append('install')
    yumargs.extend(packages)
    print "Running: %s" % (subprocess.list2cmdline(yumargs), )
    yum_env = dict(os.environ)
    yum_env['KERNEL_INSTALL_NOOP'] = 'yes'
    proc = subprocess.Popen(yumargs, env=yum_env)
    rcode = proc.wait()
    if rcode != 0:
        raise ValueError("Yum exited with code %d" % (rcode, ))

def main():
    parser = optparse.OptionParser('%prog ACTION PACKAGE1 [PACKAGE2...]')
    parser.add_option('', "--repo",
                      action='store', dest='repo_path',
                      default=None,
                      help="Path to OSTree repository (default=/ostree)")
    parser.add_option('', "--deploy",
                      action='store_true',
                      default=False,
                      help="Do a deploy if true")
    parser.add_option('', "--breakpoint",
                      action='store',
                      default=None,
                      help="Stop at given phase")
    parser.add_option('', "--os",
                      action='store', dest='os',
                      default=None,
                      help="OS Name (default from /etc/os-release)")
    parser.add_option('', "--os-version",
                      action='store', dest='os_version',
                      default=None,
                      help="OS version (default from /etc/os-release)")
    parser.add_option('', "--enablerepo",
                      action='append', dest='enablerepo',
                      default=[],
                      help="Enable this yum repo")
    parser.add_option('', "--local-ostree-package",
                      action='store', dest='local_ostree_package',
                      default='ostree',
                      help="Path to local OSTree RPM")

    global opts
    global args
    (opts, args) = parser.parse_args(sys.argv[1:])

    if (opts.os is None or
        opts.os_version is None):
        f = open('/etc/os-release')
        for line in f.readlines():
            if line == '': continue
            (k,v) = line.split('=', 1)
            os_release_data[k.strip()] = v.strip()
        f.close()

    if opts.os is None:
        opts.os = os_release_data['ID']
    if opts.os_version is None:
        opts.os_version = os_release_data['VERSION_ID']

    print "Targeting os=%s version=%s" % (opts.os, opts.os_version)

    action = args[0]
    if action == 'create':
        branchname = args[1]
        ref = '%s/%s/%s' % (opts.os, opts.os_version, branchname)
        packages = args[2:]
        commit_message = 'Commit of %d packages' % (len(packages), )
    else:
        print >>sys.stderr, "Unknown action %s" % (action, )
        sys.exit(1)

    if opts.repo_path is not None:
        repo = OSTree.Repo.new(Gio.File.new_for_path(opts.repo_path))
    else:
        repo = OSTree.Repo.new_default()

    repo.open(None)

    cachedir = '/var/cache/rpm-ostree/work'
    ensuredir(cachedir)

    yumroot = os.path.join(cachedir, 'yum')
    targetroot = os.path.join(cachedir, 'rootfs')
    yumcachedir = os.path.join(yumroot, 'var/cache/yum')
    yumcache_lookaside = os.path.join(cachedir, 'yum-cache')
    logs_lookaside = os.path.join(cachedir, 'logs')

    rmrf(logs_lookaside)
    ensuredir(logs_lookaside)

    shutil.rmtree(yumroot, ignore_errors=True)
    if action == 'create':
        yumroot_varcache = os.path.join(yumroot, 'var/cache')
        if os.path.isdir(yumcache_lookaside):
            print "Reusing cache: " + yumroot_varcache
            ensuredir(yumroot_varcache)
            subprocess.check_call(['cp', '-a', yumcache_lookaside, yumcachedir])
        else:
            print "No cache found at: " + yumroot_varcache
    else:
        print "Cloning active root"
        _clone_current_root_to_yumroot(yumroot)
        print "...done"
        time.sleep(3)

    # Ensure we have enough to modify NSS
    yuminstall(yumroot, ['filesystem', 'glibc', 'nss-altfiles'])

    if opts.breakpoint == 'post-yum-phase1':
        return
    
    # Prepare NSS configuration; this needs to be done
    # before any invocations of "useradd" in %post
    for n in ['passwd', 'group']:
        open(os.path.join(yumroot, 'usr/lib', n), 'w').close()
    replace_nsswitch(os.path.join(yumroot, 'etc'))

    yuminstall(yumroot, packages)

    if opts.breakpoint == 'post-yum-phase2':
        return

    do_kernel_prep(yumroot, logs_lookaside)

    if opts.breakpoint == 'post-yum':
        return

    # Attempt to cache stuff between runs
    rmrf(yumcache_lookaside)
    print "Saving yum cache " + yumcache_lookaside
    os.rename(yumcachedir, yumcache_lookaside)

    yumroot_rpmlibdir = os.path.join(yumroot, 'var/lib/rpm')
    rpmtextlist = os.path.join(cachedir, 'rpm-manifest.txt')
    manifest = subprocess.check_call(['rpm', '-qa', '--dbpath=' + yumroot_rpmlibdir],
                                     stdout=open(rpmtextlist, 'w'))

    rmrf(targetroot)
    _create_rootfs_from_yumroot_content(targetroot, yumroot)

    yumroot_varlog = os.path.join(yumroot, 'var/log')
    for name in os.listdir(yumroot_varlog):
        shutil.move(os.path.join(yumroot_varlog, name), logs_lookaside)

    # To make SELinux work, we need to do the labeling right before this.
    # This really needs some sort of API, so we can apply the xattrs as
    # we're committing into the repo, rather than having to label the
    # physical FS.
    # For now, no xattrs (and hence no SELinux =( )
    print "Committing " + targetroot + "..."
    repo.prepare_transaction(None)
    mtree = OSTree.MutableTree.new()
    modifier = OSTree.RepoCommitModifier.new(OSTree.RepoCommitModifierFlags.SKIP_XATTRS, None, None)
    repo.write_directory_to_mtree(Gio.File.new_for_path(targetroot),
                                  mtree, modifier, None)
    [success, parent] = repo.resolve_rev(ref, True)
    [success, tree] = repo.write_mtree(mtree, None)
    [success, commit] = repo.write_commit(parent, '', commit_message, None, tree, None)
    repo.transaction_set_ref(None, ref, commit)
    repo.commit_transaction(None)

    print "%s => %s" % (ref, commit)

    rmrf(yumroot)
    rmrf(targetroot)

    if opts.deploy:
        subprocess.check_call(['ostree', 'admin', 'deploy', '--os=' + opts.os, ref])
