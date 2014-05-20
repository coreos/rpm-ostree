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
const Guestfs = imports.gi.Guestfs;

const GSystem = imports.gi.GSystem;
const Params = imports.params;
const ProcUtil = imports.procutil;
const GuestFish = imports.guestfish;
const JSUtil = imports.jsutil;

const BOOT_UUID = "fdcaea3b-2775-45ef-b441-b46a4a18e8c4";
const ROOT_UUID = "d230f7f0-99d3-4244-8bd9-665428054831";
const SWAP_UUID = "61f066e3-ac18-464e-bcc7-e7c3a623cec1";

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
    let qemuPathString;
    qemuPathString = GLib.find_program_in_path('qemu-system-x86_64');
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
        throw new Error("Unable to find qemu-kvm or qemu-system-x86_64");
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

function _installSyslinux(gfHandle, cancellable) {
    let syslinuxPaths = ['/usr/share/syslinux/mbr.bin', '/usr/lib/syslinux/mbr.bin'].map(function (a) { return Gio.File.new_for_path(a); });
    let syslinuxPath = null;
    for (let i = 0; i < syslinuxPaths.length; i++) {
	let path = syslinuxPaths[i];
	if (path.query_exists(null)) {
	    syslinuxPath = path;
	    break;
	}
    }
    if (syslinuxPath == null)
	throw new Error("Couldn't find syslinux mbr.bin in any of " + JSON.stringify(syslinuxPaths));

    let [,syslinuxData,] = syslinuxPath.load_contents(cancellable);

    gfHandle.pwrite_device("/dev/sda", syslinuxData, 0);
}

function createDisk(diskpath, cancellable, params) {
    params = Params.parse(params, { sizeMb: 8 * 1024,
				    bootsizeMb: 200,
				    swapsizeMb: 64 });
    let guestfishProcess;
    
    ProcUtil.runSync(['qemu-img', 'create', '-o', 'compat=0.10', '-f', 'qcow2', diskpath.get_path(), '' + params.sizeMb + 'M'], cancellable);
    let gfHandle = Guestfs.Session.new();
    gfHandle.add_drive(diskpath.get_path(), null);
    gfHandle.launch();
    gfHandle.part_init("/dev/sda", "mbr");
    let diskBytesize = gfHandle.blockdev_getsize64("/dev/sda");
    let diskSectorsize = gfHandle.blockdev_getss("/dev/sda");
    print(Format.vprintf("bytesize: %s sectorsize: %s", [diskBytesize, diskSectorsize]));
    let bootsizeSectors = 0;
    if (params.bootsizeMb)
	bootsizeSectors = params.bootsizeMb * 1024 / diskSectorsize * 1024;
    let swapsizeSectors = 0;
    if (params.swapsizeMb)
	swapsizeSectors = params.swapsizeMb * 1024 / diskSectorsize * 1024;
    let rootsizeSectors = diskBytesize / diskSectorsize - bootsizeSectors - swapsizeSectors - 64;
    print(Format.vprintf("boot: %s swap: %s root: %s", [bootsizeSectors, swapsizeSectors, rootsizeSectors]));
    let bootOffset = 64;
    let swapOffset = bootOffset + bootsizeSectors;
    let rootOffset = swapOffset + swapsizeSectors;
    let endOffset = rootOffset + rootsizeSectors;

    let bootPartitionOffset = -1
    let swapPartitionOffset = -1;
    let rootPartitionOffset = 1;
    if (bootsizeSectors > 0) {
	gfHandle.part_add("/dev/sda", "p", bootOffset, swapOffset - 1);
	bootPartitionOffset = 1;
	rootPartitionOffset++;
    }
    if (swapsizeSectors > 0) {
	gfHandle.part_add("/dev/sda", "p", swapOffset, rootOffset - 1);
	swapPartitionOffset = 2;
	rootPartitionOffset++;
    }
    gfHandle.part_add("/dev/sda", "p", rootOffset, endOffset - 1);
    if (bootsizeSectors > 0) {
	gfHandle.mkfs("ext4", "/dev/sda" + bootPartitionOffset, new Guestfs.Mkfs({ features: "^64bit" }));
	gfHandle.set_e2uuid("/dev/sda1", BOOT_UUID);
    }
    if (swapsizeSectors > 0) {
	gfHandle.mkswap_U(SWAP_UUID, "/dev/sda" + swapPartitionOffset);
    }
    let rootPartition = "/dev/sda" + rootPartitionOffset;
    gfHandle.mkfs("xfs", rootPartition, null);
    gfHandle.xfs_admin(rootPartition, new Guestfs.XfsAdmin({ "uuid": ROOT_UUID}));
    gfHandle.mount(rootPartition, "/");
    gfHandle.mkdir_mode("/boot", 493);
    if (bootPartitionOffset > 0)
	gfHandle.mount("/dev/sda" + bootPartitionOffset, "/boot");
    gfHandle.extlinux("/boot");
    // It's understandable that extlinux wants the loader to be
    // immutable...except that later breaks our ability to set SELinux
    // security contexts on it.
    gfHandle.set_e2attrs("/boot/ldlinux.sys", "i", new Guestfs.SetE2attrs({ clear: Guestfs.Tristate.TRUE }));
    gfHandle.umount_all();
    _installSyslinux(gfHandle, cancellable);
    gfHandle.part_set_bootable("/dev/sda", 1, true);
    gfHandle.shutdown();
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
    let exportScript = datadir.resolve_relative_path('rpm-ostree-export-journal-to-serialdev');
    let exportScriptService = datadir.resolve_relative_path('rpm-ostree-export-journal-to-serialdev.service');
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

function pullDeploy(mntdir, srcrepo, osname, target, revision, originRepoUrl, cancellable,
		    params) {
    params = Params.parse(params, { addKernelArgs: ['quiet'] });
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
    syslinuxPath.replace_contents('TIMEOUT 20\nPROMPT 1\n', null, false, Gio.FileCreateFlags.NONE, cancellable);
    
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
                      'pull-local', '--disable-fsync', '--remote=' + osname, srcrepo.get_path(), revOrTarget], cancellable,
                     {logInitiation: true, env: adminEnv});

    let origin = GLib.KeyFile.new();
    origin.set_string('origin', 'refspec', osname + ':' + target);
    let [originData, len] = origin.to_data();
    let tmpOrigin = Gio.File.new_for_path('origin.tmp');
    tmpOrigin.replace_contents(originData, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);

    let deployCmd = adminCmd.concat([]);
    deployCmd.push('deploy');

    let rootArg = 'root=UUID=' + ROOT_UUID;
    deployCmd.push('--karg=' + rootArg);

    for (let i = 0; i < params.addKernelArgs.length; i++)
	deployCmd.push('--karg=' + params.addKernelArgs[i]);

    deployCmd.push.apply(deployCmd, ['--os=' + osname, '--origin-file=' + tmpOrigin.get_path(), revOrTarget]);

    ProcUtil.runSync(deployCmd, cancellable,
                     {logInitiation: true, env: adminEnv});

    let sysroot = OSTree.Sysroot.new(mntdir);
    sysroot.load(null);
    let deployments = sysroot.get_deployments();
    let newDeployment = deployments[0];
    let newDeploymentDirectory = sysroot.get_deployment_directory(newDeployment);

    let defaultFstab = 'UUID=' + ROOT_UUID + ' / xfs defaults 1 1\n\
UUID=' + BOOT_UUID + ' /boot ext4 defaults 1 2\n\
UUID=' + SWAP_UUID + ' swap swap defaults 0 0\n';
    let fstabPath = newDeploymentDirectory.resolve_relative_path('etc/fstab');
    fstabPath.replace_contents(defaultFstab, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);

    print("Labeling deployment root");
    let relabelCmd = adminCmd.concat(['instutil', 'selinux-ensure-labeled', newDeploymentDirectory.get_path(), ""]);
    ProcUtil.runSync(relabelCmd,
		     cancellable,
		     { logInitiation: true });
};

function requestSnapshotForTree(task, resultdir, refname, revision) {
    let imageCacheDir = task.cachedir.get_child('images');
    let osname = task._products['osname'];
    let originRepoUrl = this._products['repo'];

    let refUnix = ref.replace(/\//g, '-');
    let diskDir = this._imageCacheDir.get_child(refUnix);
    GSystem.file_ensure_directory(diskDir, true, cancellable);
    let diskName = revision + '.qcow2';
    let diskPath = diskDir.get_child(diskName);
    let diskPathTmp = diskDir.get_child(diskName);
    if (!diskPath.query_exists(null))
        LibQA.createDisk(diskPath, cancellable);
    let mntdir = Gio.File.new_for_path('mnt');
    GSystem.file_ensure_directory(mntdir, true, cancellable);
    let gfmnt = new GuestFish.GuestMount(diskPath, { partitionOpts: LibQA.DEFAULT_GF_PARTITION_OPTS,
                                                     readWrite: true });
    gfmnt.mount(mntdir, cancellable);
    try {
        LibQA.pullDeploy(mntdir, this.repo, osname, ref, revision, originRepoUrl,
                         cancellable);
    } finally {
        gfmnt.umount(cancellable);
    }

    print("Successfully updated " + diskDir.get_path() + " to " + revision);
} 

function getACachedDisk(diskDir, cancellable) {
    let cachedDisk = null;
    let e = null;
    try {
        e = diskDir.enumerate_children('standard::name', 0, cancellable);
        let info;
        while ((info = e.next_file(cancellable)) != null) {
            let name = info.get_name();
            if (!JSUtil.stringEndswith(name, '.qcow2'))
                continue;
            cachedDisk = e.get_child(info);
            break;
        }
    } finally {
        if (e) e.close(null);
    }
    
    return cachedDisk;
}
