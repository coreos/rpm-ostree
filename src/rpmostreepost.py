#!/usr/bin/env python
#
# Copyright (C) 2012,2013,2014 Colin Walters <walters@verbum.org>
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

def log(msg):
    sys.stdout.write(msg)
    sys.stdout.write('\n')
    sys.stdout.flush()

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
            log("Removing RPM-generated " + initramfs_path)
            rmrf(initramfs_path)

    if kernel_path is None:
        raise ValueError("Failed to find vmlinuz- in " + bootdir)

    kname = os.path.basename(kernel_path)
    kver = kname[kname.find('-') + 1:]
    log("Kernel version is " + kver)
           
    # OSTree will take care of this
    loaderdir = os.path.join(bootdir, 'loader')
    rmrf(loaderdir)

    args = ['chroot', yumroot, 'depmod', kver]
    log("Running: %s" % (subprocess.list2cmdline(args), ))
    subprocess.check_call(args)

    # Copy of code from gnome-continuous; yes, we hardcode
    # the machine id for now, because distributing pre-generated
    # initramfs images with dracut/systemd at the moment
    # effectively requires this.
    # http://lists.freedesktop.org/archives/systemd-devel/2013-July/011770.html
    log("Hardcoding machine-id")
    f = open(os.path.join(yumroot, 'etc', 'machine-id'), 'w')
    f.write('45bb3b96146aa94f299b9eb43646eb35\n')
    f.close()

    args = ['chroot', yumroot,
            'dracut', '-v', '--tmpdir=/tmp',
            '-f', '/tmp/initramfs.img', kver];
    log("Running: %s" % (subprocess.list2cmdline(args), ))
    subprocess.check_call(args)
    
    initramfs_path = os.path.join(yumroot, 'tmp', 'initramfs.img')
    if not os.path.exists(initramfs_path):
        raise ValueError("Failed to find " + initramfs_path)

    os.rename(initramfs_path, os.path.join(bootdir, 'initramfs-' + kver + '.img'))
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
    shutil.copy(os.path.join(PKGLIBDIR, 'tmpfiles-ostree-integration.conf'), target_tmpfilesd)

def main():
    parser = optparse.OptionParser('%prog ROOTFS REFNAME')
    parser.add_option('', "--repo",
                      action='store', dest='repo_path',
                      default=None,
                      help="Path to OSTree repository (default=/ostree)")
    parser.add_option('-m', "--message",
                      action='store', dest='message',
                      default="",
                      help="Commit message")
    parser.add_option('', "--gpg-sign",
                      action='store',
                      default=None,
                      help="Sign commits using given GPG key ID")

    global opts
    global args
    (opts, args) = parser.parse_args(sys.argv[1:])

    rootfs_path = args[0]
    refname = args[1]

    rootfs_tmp = rootfs_path + '.tmp'
    rmrf(rootfs_tmp)
    _create_rootfs_from_yumroot_content(rootfs_tmp, rootfs_path)
    rmrf(rootfs_path)
    os.rename(rootfs_tmp, rootfs_path)

    if opts.repo_path is not None:
        repo = OSTree.Repo.new(Gio.File.new_for_path(opts.repo_path))
    else:
        repo = OSTree.Repo.new_default()

    repo.open(None)

    # To make SELinux work, we need to do the labeling right before this.
    # This really needs some sort of API, so we can apply the xattrs as
    # we're committing into the repo, rather than having to label the
    # physical FS.
    # For now, no xattrs (and hence no SELinux =( )
    log("Committing " + rootfs_path + "...")
    repo.prepare_transaction(None)
    mtree = OSTree.MutableTree.new()
    modifier = OSTree.RepoCommitModifier.new(OSTree.RepoCommitModifierFlags.SKIP_XATTRS, None, None)
    repo.write_directory_to_mtree(Gio.File.new_for_path(rootfs_path),
                                  mtree, modifier, None)
    [success, parent] = repo.resolve_rev(refname, True)
    [success, tree] = repo.write_mtree(mtree, None)
    [success, commit] = repo.write_commit(parent, '', opts.message, None, tree, None)
    if opts.gpg_sign is not None:
        repo.sign_commit(commit, opts.gpg_sign, None, None)
    repo.transaction_set_ref(None, refname, commit)
    repo.commit_transaction(None)

    log("%s => %s" % (refname, commit))

    if 'RPM_OSTREE_PRESERVE_ROOTFS' not in os.environ:
        rmrf(rootfs_path)
    else:
        log("Preserved " + rootfs_path)
