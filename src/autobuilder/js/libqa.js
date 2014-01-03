// Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;
const Lang = imports.lang;
const OSTree = imports.gi.OSTree;

const GSystem = imports.gi.GSystem;
const Params = imports.params;
const ProcUtil = imports.procutil;
const GuestFish = imports.guestfish;

const DEFAULT_GF_PARTITION_OPTS = ['-m', '/dev/sda3', '-m', '/dev/sda1:/boot'];

function linuxGetMemTotalMb() {
    let [success,contents] = GLib.file_get_contents('/proc/meminfo');
    let contentLines = contents.toString().split(/\n/);
    for (let i = 0; contentLines.length; i++) {
	let line = contentLines[i];
	if (line.indexOf('MemTotal:') == 0) {
	    return parseInt(/([0-9]+) kB/.exec(line)[1]) / 1024;
	}
    }
    throw new Error("Couldn't determine total memory from /proc/meminfo");
}

function getQemuPath() {
    let fallbackPaths = ['/usr/libexec/qemu-kvm']
    let qemuPathString = GLib.find_program_in_path('qemu-kvm');
    qemuPathString = GLib.find_program_in_path('qemu-kvm');
    if (!qemuPathString)
	qemuPathString = GLib.find_program_in_path('kvm');
    if (qemuPathString == null) {
        for (let i = 0; i < fallbackPaths.length; i++) {
            let path = Gio.File.new_for_path(fallbackPaths[i]);
            if (!path.query_exists(null))
                continue;
            qemuPathString = path.get_path();
        }
    }
    if (qemuPathString == null) {
        throw new Error("Unable to find qemu-kvm");
    }
    return qemuPathString;
}

function getDefaultQemuOptions(params) {
    params = Params.parse(params, { parallel: false });
    let args = [getQemuPath(), '-vga', 'std', '-usb', '-usbdevice', 'tablet', '-net', 'none'];

    let systemMemoryMb = linuxGetMemTotalMb();
    let minimumGuestMemoryMb = 768;
    let maximumGuestMemoryMb = 4 * 1024;
    // As a guess, use 1/4 of host memory, rounded up to the nearest
    // multiple of 128M; subject to above constraints as a lame
    // default...we need global coordination here.
    let guestMemoryGuessMb = Math.floor(systemMemoryMb / 4 / 128) * 128;
    let guestMemory = Math.floor(Math.max(minimumGuestMemoryMb,
					  Math.min(maximumGuestMemoryMb,
						   guestMemoryGuessMb)));
    args.push.apply(args, ['-m', ''+guestMemory]);
    
    if (params.parallel) {
        let nCores = Math.min(16, GLib.get_num_processors());
        args.push.apply(args, ['-smp', ''+nCores]);
    }

    return args;
}

function newReadWriteMount(diskpath, cancellable) {
    let mntdir = Gio.File.new_for_path('mnt');
    GSystem.file_ensure_directory(mntdir, true, cancellable);
    let gfmnt = new GuestFish.GuestMount(diskpath, {partitionOpts: DEFAULT_GF_PARTITION_OPTS,
                                                    readWrite: true});
    gfmnt.mount(mntdir, cancellable);
    return [gfmnt, mntdir];
}

function createDisk(diskpath, cancellable) {
    let sizeMb = 8 * 1024;
    let bootsizeMb = 200;
    let swapsizeMb = 64;

    let guestfishProcess;
    
    ProcUtil.runSync(['qemu-img', 'create', '-f', 'qcow2', diskpath.get_path(), '' + sizeMb + 'M'], cancellable);
    let makeDiskCmd = 'launch\n\
part-init /dev/sda mbr\n\
blockdev-getsize64 /dev/sda\n\
blockdev-getss /dev/sda\n';
    let gf = new GuestFish.GuestFish(diskpath, {partitionOpts: [], readWrite: true});
    let lines = gf.run(makeDiskCmd, cancellable);
    if (lines.length != 2)
        throw new Error("guestfish returned unexpected output lines (" + lines.length + ", expected 2");
    let diskBytesize = parseInt(lines[0]);
    let diskSectorsize = parseInt(lines[1]);
    print(Format.vprintf("bytesize: %s sectorsize: %s", [diskBytesize, diskSectorsize]));
    let bootsizeSectors = bootsizeMb * 1024 / diskSectorsize * 1024;
    let swapsizeSectors = swapsizeMb * 1024 / diskSectorsize * 1024;
    let rootsizeSectors = diskBytesize / diskSectorsize - bootsizeSectors - swapsizeSectors - 64;
    let bootOffset = 64;
    let swapOffset = bootOffset + bootsizeSectors;
    let rootOffset = swapOffset + swapsizeSectors;
    let endOffset = rootOffset + rootsizeSectors;

    let partconfig = Format.vprintf('launch\n\
part-add /dev/sda p %s %s\n\
part-add /dev/sda p %s %s\n\
part-add /dev/sda p %s %s\n\
part-set-bootable /dev/sda 1 true\n\
mkfs ext4 /dev/sda1\n\
set-e2label /dev/sda1 gnostree-boot\n\
mkswap-L gnostree-swap /dev/sda2\n\
mkfs ext4 /dev/sda3\n\
set-e2label /dev/sda3 gnostree-root\n\
mount /dev/sda3 /\n\
mkdir /boot\n\
', [bootOffset, swapOffset - 1,
    swapOffset, rootOffset - 1,
    rootOffset, endOffset - 1]);
    print("partition config: ", partconfig);
    gf.run(partconfig, cancellable);
}

function createDiskSnapshot(diskpath, newdiskpath, cancellable) {
    ProcUtil.runSync(['qemu-img', 'create', '-f', 'qcow2', '-o', 'backing_file=' + diskpath.get_path(),
		      newdiskpath.get_path()], cancellable);
}

function copyDisk(srcpath, destpath, cancellable) {
    ProcUtil.runSync(['qemu-img', 'convert', '-O', 'qcow2', srcpath.get_path(),
		      destpath.get_path()], cancellable);
}

function getSysrootAndCurrentDeployment(mntdir, osname) {
    let sysroot = OSTree.Sysroot.new(mntdir);
    sysroot.load(null);
    let deployments = sysroot.get_deployments().filter(function (deployment) {
	return deployment.get_osname() == osname;
    });
    if (deployments.length == 0)
	throw new Error("No deployments for " + osname + " in " + mntdir.get_path());
    let current = deployments[0];
    return [sysroot, current];
}

function getDeployDirs(mntdir, osname) {
    let [sysroot, current] = getSysrootAndCurrentDeployment(mntdir, osname);
    let deployDir = sysroot.get_deployment_directory(current);
    return [deployDir, deployDir.get_child('etc')];
}

function modifyBootloaderAppendKernelArgs(mntdir, kernelArgs, cancellable) {
    let confPath = mntdir.resolve_relative_path('boot/syslinux/syslinux.cfg');
    let conf = GSystem.file_load_contents_utf8(confPath, cancellable);
    let lines = conf.split('\n');
    let modifiedLines = [];
    
    let didModify = false;
    let kernelArg = kernelArgs.join(' ');
    let kernelLineRe = /\tAPPEND /;
    for (let i = 0; i < lines.length; i++) {
	let line = lines[i];
	let match = kernelLineRe.exec(line);
	if (!match) {
	    modifiedLines.push(line);
	} else {
	    modifiedLines.push(line + ' ' + kernelArg);
	    didModify = true;
	}
    }
    if (!didModify)
	throw new Error("Failed to find APPEND option in syslinux.cfg");
    let modifiedConf = modifiedLines.join('\n');
    confPath.replace_contents(modifiedConf, null, false,
			      Gio.FileCreateFlags.NONE,
			      cancellable);
}

function getMultiuserWantsDir(currentEtcDir) {
    return currentEtcDir.resolve_relative_path('systemd/system/multi-user.target.wants');
}

function getDatadir() {
    return Gio.File.new_for_path(GLib.getenv('OSTBUILD_DATADIR'));
}

function injectExportJournal(currentDir, currentEtcDir, cancellable) {
    let binDir = currentDir.resolve_relative_path('usr/bin');
    let multiuserWantsDir = getMultiuserWantsDir(currentEtcDir);
    let datadir = getDatadir();
    let exportScript = datadir.resolve_relative_path('tests/gnome-ostree-export-journal-to-serialdev');
    let exportScriptService = datadir.resolve_relative_path('tests/gnome-ostree-export-journal-to-serialdev.service');
    let exportBin = binDir.get_child(exportScript.get_basename());
    exportScript.copy(exportBin, Gio.FileCopyFlags.OVERWRITE, cancellable, null, null);
    GSystem.file_chmod(exportBin, 493, cancellable);
    exportScriptService.copy(multiuserWantsDir.get_child(exportScriptService.get_basename()), Gio.FileCopyFlags.OVERWRITE, cancellable, null, null);
    let journalConfPath = currentEtcDir.resolve_relative_path('systemd/journald.conf');
    journalConfPath.replace_contents('[Journal]\n\
RateLimitInterval=0\n', null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
}

function injectTestUserCreation(currentDir, currentEtcDir, username, params, cancellable) {
    params = Params.parse(params, { password: null });
    let execLine;
    if (params.password === null) {
	execLine = Format.vprintf('/bin/sh -c "/usr/sbin/useradd %s; passwd -d %s"',
				  [username, username]);
    } else {
	execLine = Format.vprintf('/bin/sh -c "/usr/sbin/useradd %s; echo %s | passwd --stdin %s',
				  [username, params.password, username]);
    }
    let addUserService = '[Unit]\n\
Description=Add user %s\n\
Before=gdm.service\n\
[Service]\n\
ExecStart=%s\n\
Type=oneshot\n';
    addUserService = Format.vprintf(addUserService, [username, execLine]);

    let addUserServicePath = getMultiuserWantsDir(currentEtcDir).get_child('gnome-ostree-add-user-' + username + '.service');
    addUserServicePath.replace_contents(addUserService, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
}

function enableAutologin(currentDir, currentEtcDir, username, cancellable) {
    let gdmCustomPath = currentEtcDir.resolve_relative_path('gdm/custom.conf');
    let keyfile = new GLib.KeyFile();
    keyfile.load_from_file(gdmCustomPath.get_path(), GLib.KeyFileFlags.NONE);
    keyfile.set_string('daemon', 'AutomaticLoginEnable', 'true');
    keyfile.set_string('daemon', 'AutomaticLogin', username);
    gdmCustomPath.replace_contents(keyfile.to_data()[0], null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
}

function _findFirstFileMatching(dir, prefix, cancellable) {
    let d = dir.enumerate_children('standard::*', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
    let finfo;
    try {
	while ((finfo = d.next_file(cancellable)) != null) {
	    let name = finfo.get_name();
	    if (name.indexOf(prefix) == 0) {
                return dir.get_child(name);
            }
        }
        throw new Error("Couldn't find " + prefix + " in " + dir.get_path());
    } finally {
        d.close(null);
    }
    return null;
} 

function _findCurrentKernel(mntdir, osname, cancellable) {
    let [sysroot, current] = getSysrootAndCurrentDeployment(mntdir, osname);
    let deployBootdir = sysroot.get_deployment_directory(current).resolve_relative_path('boot');
    return [_findFirstFileMatching(deployBootdir, 'vmlinuz-', cancellable),
	    _findFirstFileMatching(deployBootdir, 'initramfs-', cancellable)];
};

function _findCurrentOstreeBootArg(mntdir, cancellable) {
    let bootLoaderEntriesDir = mntdir.resolve_relative_path('boot/loader/entries');
    let conf = _findFirstFileMatching(bootLoaderEntriesDir, 'ostree-', cancellable);
    let contents = GSystem.file_load_contents_utf8(conf, cancellable);
    let lines = contents.split('\n');
    for (let i = 0; i < lines.length; i++) {
	let line = lines[i];
	if (line.indexOf('options ') != 0)
	    continue;
	let options = line.substr(8).split(' ');
	for (let j = 0; j < options.length; j++) { 
	    let opt = options[j];
	    if (opt.indexOf('ostree=') != 0)
		continue;
	    return opt;
        }
    }
    throw new Error("Failed to find ostree= kernel argument");
}

function pullDeploy(mntdir, srcrepo, osname, target, revision, originRepoUrl, cancellable) {
    let ostreedir = mntdir.get_child('ostree');
    let ostreeOsdir = ostreedir.resolve_relative_path('deploy/' + osname);

    let adminCmd = ['ostree', 'admin', '--sysroot=' + mntdir.get_path()];
    let adminEnv = GLib.get_environ();
    adminEnv.push('LIBGSYSTEM_ENABLE_GUESTFS_FUSE_WORKAROUND=1');
    let procdir = mntdir.get_child('proc');
    if (!procdir.query_exists(cancellable)) {
        ProcUtil.runSync(adminCmd.concat(['init-fs', mntdir.get_path()]), cancellable,
                         {logInitiation: true, env: adminEnv});
    }

    let revOrTarget;
    if (revision)
	revOrTarget = revision;
    else
	revOrTarget = target;

    // Remove any existing bootloader configuration, and stub out an
    // empty syslinux configuration that we can use to bootstrap.
    let bootLoaderLink = mntdir.resolve_relative_path('boot/loader');
    GSystem.shutil_rm_rf(bootLoaderLink, cancellable);
    let bootLoaderDir0 = mntdir.resolve_relative_path('boot/loader.0');
    GSystem.shutil_rm_rf(bootLoaderDir0, cancellable);
    bootLoaderLink.make_symbolic_link('loader.0', cancellable);
    GSystem.file_ensure_directory(bootLoaderDir0, true, cancellable);
    let syslinuxPath = mntdir.resolve_relative_path('boot/loader/syslinux.cfg');
    syslinuxPath.replace_contents('TIMEOUT 10\n', null, false, Gio.FileCreateFlags.NONE, cancellable);
    
    // A compatibility symlink for syslinux
    let syslinuxDir = mntdir.resolve_relative_path('boot/syslinux');
    GSystem.shutil_rm_rf(syslinuxDir, cancellable);
    GSystem.file_ensure_directory(syslinuxDir, true, cancellable);
    let syslinuxLink = mntdir.resolve_relative_path('boot/syslinux/syslinux.cfg');
    syslinuxLink.make_symbolic_link('../loader/syslinux.cfg', cancellable);

    // Also blow alway all existing deployments here for the OS; this
    // will clean up disks that were using the old ostree model.
    GSystem.shutil_rm_rf(ostreeOsdir, cancellable);
    
    let repoPath = ostreedir.get_child('repo');
    let repoArg = '--repo=' + repoPath.get_path();
    ProcUtil.runSync(adminCmd.concat(['os-init', osname]), cancellable,
                     {logInitiation: true, env: adminEnv});
    if (originRepoUrl)
        ProcUtil.runSync(['ostree', repoArg,
                          'remote', 'add', osname, originRepoUrl, target],
                         cancellable, { logInitiation: true });
    
    ProcUtil.runSync(['ostree', repoArg,
                      'pull-local', '--remote=' + osname, srcrepo.get_path(), revOrTarget], cancellable,
                     {logInitiation: true, env: adminEnv});

    let origin = GLib.KeyFile.new();
    origin.set_string('origin', 'refspec', osname + ':' + target);
    let [originData, len] = origin.to_data();
    let tmpOrigin = Gio.File.new_for_path('origin.tmp');
    tmpOrigin.replace_contents(originData, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);

    let rootArg = 'root=LABEL=gnostree-root';
    ProcUtil.runSync(adminCmd.concat(['deploy', '--karg=' + rootArg, '--karg=quiet', '--karg=splash',
				      '--os=' + osname, '--origin-file=' + tmpOrigin.get_path(), revOrTarget]), cancellable,
                     {logInitiation: true, env: adminEnv});

    let defaultFstab = 'LABEL=gnostree-root / ext4 defaults 1 1\n\
LABEL=gnostree-boot /boot ext4 defaults 1 2\n\
LABEL=gnostree-swap swap swap defaults 0 0\n';
    let fstabPath = ostreeOsdir.resolve_relative_path('current/etc/fstab');
    fstabPath.replace_contents(defaultFstab, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
};

function bootloaderInstall(diskpath, workdir, osname, cancellable) {
    let qemuArgs = getDefaultQemuOptions();

    let tmpKernelPath = workdir.get_child('kernel.img');
    let tmpInitrdPath = workdir.get_child('initrd.img');

    let [gfmnt, mntdir] = newReadWriteMount(diskpath, cancellable);
    let ostreeArg;
    try {
	let [kernelPath, initrdPath] = _findCurrentKernel(mntdir, osname, cancellable)
	ostreeArg = _findCurrentOstreeBootArg(mntdir, cancellable);

	// Copy
	kernelPath.copy(tmpKernelPath, 0, cancellable, null, null);
	initrdPath.copy(tmpInitrdPath, 0, cancellable, null, null);
    } finally {
        gfmnt.umount(cancellable);
    }

    let consoleOutput = workdir.get_child('bootloader-console.out');
    
    let kernelArgv = ['console=ttyS0', 'panic=1', 'root=LABEL=gnostree-root', 'rw', ostreeArg,
		      'systemd.journald.forward_to_console=true',
		      'systemd.unit=gnome-ostree-install-bootloader.target'];

    qemuArgs.push.apply(qemuArgs, ['-drive', 'file=' + diskpath.get_path() + ',if=virtio',
                                   '-vnc', 'none',
				   '-no-reboot',
                                   '-serial', 'file:' + consoleOutput.get_path(),
                                   '-chardev', 'socket,id=charmonitor,path=qemu.monitor,server,nowait',
                                   '-mon', 'chardev=charmonitor,id=monitor,mode=control',
				   '-kernel', tmpKernelPath.get_path(),
				   '-initrd', tmpInitrdPath.get_path(),
				   '-append', kernelArgv.join(' ')
				  ]);

    ProcUtil.runSync(qemuArgs, cancellable, { cwd: workdir.get_path(),
					      logInitiation: true });

    tmpKernelPath.delete(cancellable);
    tmpInitrdPath.delete(cancellable);
}
