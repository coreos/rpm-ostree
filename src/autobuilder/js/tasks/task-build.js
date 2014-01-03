// Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
const Format = imports.format;

const GSystem = imports.gi.GSystem;
const OSTree = imports.gi.OSTree;

const Task = imports.task;
const Params = imports.params;
const FileUtil = imports.fileutil;
const AsyncUtil = imports.asyncutil;
const ProcUtil = imports.procutil;
const JSUtil = imports.jsutil;
const StreamUtil = imports.streamutil;
const JsonUtil = imports.jsonutil;
const Snapshot = imports.snapshot;
const BuildUtil = imports.buildutil;
const Vcs = imports.vcs;

// From ot-gio-utils.h.
// XXX: Introspect this.
const OSTREE_GIO_FAST_QUERYINFO = ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," +
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev");

const OPT_COMMON_CFLAGS = {'i686': '-O2 -g -m32 -march=i686 -mtune=atom -fasynchronous-unwind-tables',
                           'x86_64': '-O2 -g -m64 -mtune=generic'};

const DEVEL_DIRS = ['usr/include', 'usr/share/aclocal',
		    'usr/share/pkgconfig', 'usr/lib/pkgconfig'];
const DOC_DIRS = ['usr/share/doc', 'usr/share/gtk-doc',
		  'usr/share/man', 'usr/share/info'];

const WARNING_RE = /: warning: /;

const TaskBuild = new Lang.Class({
    Name: "TaskBuild",
    Extends: Task.Task,

    TaskDef: {
        TaskName: "build",
        TaskAfter: ['resolve'],
    },

    DefaultParameters: {forceComponents: []},

    _cleanStaleBuildroots: function(buildrootCachedir, keepRoot, cancellable) {
	let direnum = buildrootCachedir.enumerate_children("standard::*,unix::mtime",
							   Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	let finfo;
	while ((finfo = direnum.next_file(cancellable)) != null) {
	    let child = buildrootCachedir.get_child(finfo.get_name());
	    if (child.equal(keepRoot))
		continue;
	    BuildUtil.timeSubtask("Remove old cached buildroot", Lang.bind(this, function() {
		GSystem.shutil_rm_rf(child, cancellable);
	    }));
	}
	direnum.close(cancellable);
    },

    _composeBuildrootCore: function(workdir, componentName, architecture, rootContents, cancellable) {
        let buildname = Format.vprintf('%s/%s/%s', [this.osname, componentName, architecture]);
        let buildrootCachedir = this.cachedir.resolve_relative_path('roots/' + buildname);
        GSystem.file_ensure_directory(buildrootCachedir, true, cancellable);

        let trees = rootContents.map(Lang.bind(this, function([branch, subpath]) {
            let [, root, commit] = this.ostreeRepo.read_commit(branch, cancellable);
            return [root, commit, subpath];
        }));

        let toChecksumData = '';

	let creds = new Gio.Credentials();
        let uid = creds.get_unix_user();
        let gid = creds.get_unix_user();
        let etcPasswd = Format.vprintf('root:x:0:0:root:/root:/bin/bash\nbuilduser:x:%d:%d:builduser:/:/bin/bash\n', [uid, gid]);
        let etcGroup = Format.vprintf('root:x:0:root\nbuilduser:x:%d:builduser\n', [gid]);

	toChecksumData += etcPasswd;
	toChecksumData += etcGroup;
        trees.forEach(function([root, commit, subpath]) {
            toChecksumData += commit;
        });

	let newRootCacheid = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256, new GLib.Bytes(toChecksumData));

        let cachedRoot = buildrootCachedir.get_child(newRootCacheid);
        if (cachedRoot.query_exists(cancellable)) {
            print("Reusing cached buildroot: " + cachedRoot.get_path());
            this._cleanStaleBuildroots(buildrootCachedir, cachedRoot, cancellable);
            return cachedRoot;
	}

        if (rootContents.length > 0) {
            print(Format.vprintf("composing buildroot from %d parents (last: %s)", [rootContents.length,
										    rootContents[rootContents.length-1][0]]));
	}

        let cachedRootTmp = cachedRoot.get_parent().get_child(cachedRoot.get_basename() + '.tmp');
	BuildUtil.timeSubtask("clean cached buildroot", Lang.bind(this, function () {
	    GSystem.shutil_rm_rf(cachedRootTmp, cancellable);
	}));

	BuildUtil.timeSubtask("compose buildroot", Lang.bind(this, function () {
            trees.forEach(Lang.bind(this, function([root, commit, subpath]) {
		let subtree = root.resolve_relative_path(subpath);
		let subtreeInfo = subtree.query_info(OSTREE_GIO_FAST_QUERYINFO,
                                                     Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
                                                     cancellable);
		
		this.ostreeRepo.checkout_tree(OSTree.RepoCheckoutMode.USER,
                                              OSTree.RepoCheckoutOverwriteMode.UNION_FILES,
                                              cachedRootTmp, subtree, subtreeInfo, cancellable);
            }));
	    this._runTriggersInRoot(cachedRootTmp, cancellable);
	    let builddirTmp = cachedRootTmp.get_child('ostbuild');
	    GSystem.file_ensure_directory(builddirTmp.resolve_relative_path('source/' + componentName), true, cancellable);
	    GSystem.file_ensure_directory(builddirTmp.get_child('results'), true, cancellable);
	    cachedRootTmp.resolve_relative_path('etc/passwd').replace_contents(etcPasswd, null, false,
									       Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
	    cachedRootTmp.resolve_relative_path('etc/group').replace_contents(etcGroup, null, false,
									      Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
            GSystem.file_rename(cachedRootTmp, cachedRoot, cancellable);
	}));

        this._cleanStaleBuildroots(buildrootCachedir, cachedRoot, cancellable);

        return cachedRoot;
    }, 

    _composeBuildroot: function(workdir, componentName, architecture, cancellable) {
        let components = this._snapshot.data['components']
        let component = null;
        let buildDependencies = [];
        for (let i = 0; i < components.length; i++) {
	    let component = components[i];
            if (component['name'] == componentName)
                break;
            buildDependencies.push(component);
	}

        let archBuildrootName = Format.vprintf('%s/bases/%s/%s-devel', [this.osname,
									this._snapshot.data['base']['name'],
									architecture]);

        print("Computing buildroot contents");

        let [success, archBuildrootRev] = this.ostreeRepo.resolve_rev(archBuildrootName, false);

        let rootContents = [[archBuildrootName, '/']];
        for (let i = 0; i < buildDependencies.length; i++) {
	    let dependency = buildDependencies[i];
            let buildname = Format.vprintf('%s/components/%s/%s', [this.osname, dependency['name'], architecture]);
            rootContents.push([buildname, '/runtime']);
            rootContents.push([buildname, '/devel']);
	}

	return this._composeBuildrootCore(workdir, componentName, architecture, rootContents, cancellable);
     },

    _analyzeBuildFailure: function(t, architecture, component, componentSrcdir,
				   currentVcsVersion, previousVcsVersion,
				   cancellable) {
        let dataIn = Gio.DataInputStream.new(t.logfile_path.read(cancellable));
        let lines = StreamUtil.dataInputStreamReadLines(dataIn, cancellable);
        dataIn.close(cancellable);
	let maxLines = 250;
	lines = lines.splice(Math.max(0, lines.length-maxLines), maxLines);
        for (let i = 0; i < lines.length; i++) {
            print("| " + lines[i]);
	}
        if (currentVcsVersion && previousVcsVersion) {
            let args = ['git', 'log', '--format=short'];
            args.push(previousVcsVersion + '...' + currentVcsVersion);
            let env = GLib.get_environ();
            env.push('GIT_PAGER=cat');
	    ProcUtil.runSync(args, cancellable, {cwd: componentSrcdir,
						 env: env});
        } else {
            print("No previous build; skipping source diff");
	}
     },

    _compareAny: function(a, b) {
	if (typeof(a) == 'string') {
	    return a == b;
	} else if (a.length != undefined) {
	    if (a.length != b.length)
		return false;
	    for (let i = 0; i < a.length; i++) {
		if (a[i] != b[i]) {
		    return false;
		}
	    }
	} else {
	    for (let k in a) {
		if (b[k] != a[k])
		    return false;
	    }
	    for (let k in b) {
		if (a[k] == undefined)
		    return false;
	    }
	}
	return true;
    },

    _needsRebuild: function(previousMetadata, newMetadata) {
        let buildKeys = ['config-opts', 'src', 'revision', 'setuid'];
        for (let i = 0; i < buildKeys.length; i++) {
	    let k = buildKeys[i];
            if (previousMetadata[k] && !newMetadata[k]) {
                return 'key ' + k + ' removed';
	    } else if (!previousMetadata[k] && newMetadata[k]) {
                return 'key ' + k + ' added';
	    } else if (previousMetadata[k] && newMetadata[k]) {
                let oldval = previousMetadata[k];
                let newval = newMetadata[k];
                if (!this._compareAny(oldval,newval)) {
                    return Format.vprintf('key %s differs (%s -> %s)', [k, oldval, newval]);
		}
	    }
	}
            
        if (previousMetadata['patches']) {
            if (!newMetadata['patches']) {
                return 'patches differ';
	    }
            let oldPatches = previousMetadata['patches'];
            let newPatches = newMetadata['patches'];
            let oldFiles = oldPatches['files'];
            let newFiles = newPatches['files'];
            if (oldFiles.length != newFiles.length) {
                return 'patches differ';
	    }
            let oldSha256sums = oldPatches['files_sha256sums'];
            let newSha256sums = newPatches['files_sha256sums'];
            if ((!oldSha256sums || !newSha256sums) ||
                !this._compareAny(oldSha256sums, newSha256sums)) {
                return 'patch sha256sums differ';
	    }
	} else if (newMetadata['patches']) {
	    return 'patches differ';
	}
        return null;
    },

    _computeSha256SumsForPatches: function(patchdir, component, cancellable) {
        let patches = BuildUtil.getPatchPathsForComponent(patchdir, component);
        let result = [];
        for (let i = 0; i < patches.length; i++) {
	    let contentsBytes = GSystem.file_map_readonly(patches[i], cancellable);
	    let csum = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256,
						       contentsBytes);
            result.push(csum);
	}
        return result;
    },

    _writeComponentCache: function(key, data, cancellable) {
        this._componentBuildCache[key] = data;
        JsonUtil.writeJsonFileAtomic(this._componentBuildCachePath, this._componentBuildCache, cancellable);
    },

    _saveComponentBuild: function(buildRef, rev, expandedComponent, cancellable) {
	let cachedata = {};
	Lang.copyProperties(expandedComponent, cachedata);
        cachedata['ostree'] = rev;
	this._writeComponentCache(buildRef, cachedata, cancellable);
        return rev;
    },

    _installAndUnlinkRecurse: function(buildResultDir, srcFile, srcInfo, finalResultDir, cancellable) {
	let relpath = buildResultDir.get_relative_path(srcFile);
	let destFile;
	if (relpath === null)
	    destFile = finalResultDir;
	else
	    destFile = finalResultDir.resolve_relative_path(relpath);

	GSystem.file_ensure_directory(destFile.get_parent(), true, cancellable);
	
	if (srcInfo.get_file_type() == Gio.FileType.DIRECTORY) {
	    GSystem.file_ensure_directory(destFile, true, cancellable);
	    let e = srcFile.enumerate_children('standard::*,unix::mode', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	    let info;
	    while ((info = e.next_file(cancellable)) !== null) {
		let child = e.get_child(info);
		this._installAndUnlinkRecurse(buildResultDir, child, info, finalResultDir, cancellable);
	    }
	    e.close(cancellable);
	    srcFile.delete(cancellable);
	} else {
	    GSystem.file_linkcopy(srcFile, destFile, Gio.FileCopyFlags.ALL_METADATA, cancellable);
	    GSystem.file_unlink(srcFile, cancellable);
	} 
    },

    _installAndUnlink: function(buildResultDir, srcFile, finalResultDir, cancellable) {
	let srcInfo = srcFile.query_info('standard::*,unix::mode', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	this._installAndUnlinkRecurse(buildResultDir, srcFile, srcInfo, finalResultDir, cancellable);
    },

    _processBuildResultSplitDebuginfo: function(buildResultDir, debugPath, path, cancellable) {
	let name = path.get_basename();
	// Only process files ending in .so.* or executables
	let soRegex = /\.so\./;
	if (!soRegex.exec(name)) {
	    let finfo = path.query_info('unix::mode', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
					cancellable);
	    let mode = finfo.get_attribute_uint32('unix::mode');
	    if (!(mode & 73))
		return;
	}
	let elfSharedRe = /ELF.*shared/;
	let elfExecRe = /ELF.*executable/;
	let ftype = ProcUtil.runSyncGetOutputUTF8StrippedOrNull(['file', path.get_path()], cancellable);
	if (ftype == null)
	    return;

	let isShared = elfSharedRe.test(ftype);
	let isExec = elfExecRe.test(ftype);

	if (!(isShared || isExec))
	    return;

	let buildIdPattern = /\s+Build ID: ([0-9a-f]+)/;
	let match = ProcUtil.runSyncGetOutputGrep(['eu-readelf', '-n', path.get_path()], buildIdPattern, cancellable);
	if (match == null) {
	    print("WARNING: no build-id for ELF object " + path.get_path());
	    return;
	} 
	let buildId = match[1];
	print("ELF object " + path.get_path() + " buildid=" + buildId);
	let dbgName = buildId[0] + buildId[1] + '/' + buildId.substr(2) + '.debug';
	let objdebugPath = debugPath.resolve_relative_path('usr/lib/debug/.build-id/' + dbgName);
	GSystem.file_ensure_directory(objdebugPath.get_parent(), true, cancellable);
	ProcUtil.runSync(['objcopy', '--only-keep-debug', path.get_path(), objdebugPath.get_path()], cancellable);

	let stripArgs = ['strip', '--remove-section=.comment', '--remove-section=.note']; 
	if (isShared) {
	    stripArgs.push('--strip-unneeded');
	}
	stripArgs.push(path.get_path());
	ProcUtil.runSync(stripArgs, cancellable);
    },
    
    _processBuildResults: function(component, buildResultDir, finalResultDir, cancellable) {
	let runtimePath = finalResultDir.get_child('runtime');
	GSystem.file_ensure_directory(runtimePath, true, cancellable);
	let develPath = finalResultDir.get_child('devel');
	GSystem.file_ensure_directory(develPath, true, cancellable);
	let docPath = finalResultDir.get_child('doc');
	GSystem.file_ensure_directory(docPath, true, cancellable);
	let debugPath = finalResultDir.get_child('debug');
	GSystem.file_ensure_directory(debugPath, true, cancellable);
	let testsPath = finalResultDir.get_child('tests');
	GSystem.file_ensure_directory(testsPath, true, cancellable);

	// Change file modes first; some components install files that
	// are read-only even by the user, which we don't want.
	FileUtil.walkDir(buildResultDir, {}, Lang.bind(this, function(path, cancellable) {
	    let info = path.query_info("standard::type,unix::mode", Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	    if (info.get_file_type() != Gio.FileType.SYMBOLIC_LINK) {
		let minimalMode = 436; // u+rw,g+rw,o+r
		if (info.get_file_type() == Gio.FileType.DIRECTORY)
		    minimalMode |= 64; // u+x
		let mode = info.get_attribute_uint32('unix::mode');
		GSystem.file_chmod(path, mode | minimalMode, cancellable);
	    }
	}), cancellable);

	let datadir = buildResultDir.resolve_relative_path('usr/share');
	let localstatedir = buildResultDir.get_child('var');
	let libdir = buildResultDir.resolve_relative_path('usr/lib');
	let libexecdir = buildResultDir.resolve_relative_path('usr/libexec');

	// Remove /var from the install - components are required to
	// auto-create these directories on demand.
	GSystem.shutil_rm_rf(localstatedir, cancellable);

	// Python .co files contain timestamps
	// .la files are generally evil
	let DELETE_PATTERNS = [{ nameRegex: /\.(py[co])|(la)$/ },
			       { nameRegex: /\.la$/,
				 fileType: Gio.FileType.REGULAR }];
			       
	for (let i = 0; i < DELETE_PATTERNS.length; i++) {
	    let pattern = DELETE_PATTERNS[i];
	    FileUtil.walkDir(buildResultDir, pattern,
			     Lang.bind(this, function(filePath, cancellable) {
				 GSystem.file_unlink(filePath, cancellable);
			     }), cancellable);
	}

	if (libdir.query_exists(null)) {
	    // Move symbolic links for shared libraries to devel
	    FileUtil.walkDir(libdir, { nameRegex: /\.so$/,
				       fileType: Gio.FileType.SYMBOLIC_LINK,
				       depth: 1 },
			     Lang.bind(this, function(filePath, cancellable) {
				 this._installAndUnlink(buildResultDir, filePath, develPath, cancellable);
			     }), cancellable);
	    // Just delete static libraries.  No one should use them.
	    FileUtil.walkDir(libdir, { nameRegex: /\.a$/,
				       fileType: Gio.FileType.REGULAR,
				       depth: 1 },
			     Lang.bind(this, function(filePath, cancellable) {
				 GSystem.file_unlink(filePath, cancellable);
			     }), cancellable);
	}

	FileUtil.walkDir(buildResultDir, { fileType: Gio.FileType.REGULAR },
			 Lang.bind(this, function(filePath, cancellable) {
			     this._processBuildResultSplitDebuginfo(buildResultDir, debugPath, filePath, cancellable);
			 }), cancellable);

	for (let i = 0; i < DEVEL_DIRS.length; i++) {
	    let path = DEVEL_DIRS[i];
	    let oneDevelDir = buildResultDir.resolve_relative_path(path);
	    
	    if (oneDevelDir.query_exists(null)) {
		this._installAndUnlink(buildResultDir, oneDevelDir, develPath, cancellable);
	    }
	}

	for (let i = 0; i < DOC_DIRS.length; i++) {
	    let path = DOC_DIRS[i];
	    let oneDocDir = buildResultDir.resolve_relative_path(path);
	    
	    if (oneDocDir.query_exists(null)) {
		this._installAndUnlink(buildResultDir, oneDocDir, docPath, cancellable);
	    }
	}

	let installedTestFiles = datadir.get_child('installed-tests');
	if (installedTestFiles.query_exists(null)) {
	    this._installAndUnlink(buildResultDir, installedTestFiles, testsPath, cancellable);
	    
	    let installedTestsDataSubdir = null;
	    if (libexecdir.query_exists(null)) {
		let topInstTestsPath = libexecdir.get_child('installed-tests');
		if (topInstTestsPath.query_exists(null)) {
		    installedTestsDataSubdir = topInstTestsPath;
		} else { 
		    FileUtil.walkDir(libexecdir, {fileType: Gio.FileType.DIRECTORY,
						  depth: 1 },
				     Lang.bind(this, function(filePath, cancellable) {
					 let pkgInstTestsPath = filePath.get_child('installed-tests');
					 if (!pkgInstTestsPath.query_exists(null))
					     return;
					 // At the moment we only support one installed tests data
					 if (installedTestsDataSubdir == null)
					     installedTestsDataSubdir = pkgInstTestsPath;
				     }), cancellable);
		}
	    }
	    if (installedTestsDataSubdir)
		this._installAndUnlink(buildResultDir, installedTestsDataSubdir, testsPath, cancellable);
	}

	this._installAndUnlink(buildResultDir, buildResultDir, runtimePath, cancellable);
    },

    _onBuildComplete: function(taskset, success, msg, loop) {
	this._currentBuildSucceded = success;
	this._currentBuildSuccessMsg = msg;
	loop.quit();
    },

    _componentBuildRefFromName: function(componentName, architecture) {
        let archBuildname = Format.vprintf('%s/%s', [componentName, architecture]);
        return this.osname + '/components/' + archBuildname;
    },

    _componentBuildRef: function(component, architecture) {
	return this._componentBuildRefFromName(component['name'], architecture);
    },

    _commitFilter: function(repo, path, fileInfo, setuidFiles) {
        fileInfo.set_attribute_uint32("unix::uid", 0);
        fileInfo.set_attribute_uint32("unix::gid", 0);

        if (setuidFiles.indexOf(path) >= 0) {
            const SETUID_MODE = 2048;
            let mode = fileInfo.get_attribute_uint32("unix::mode");
            fileInfo.set_attribute_uint32("unix::mode", mode | SETUID_MODE);
        }

        return OSTree.RepoCommitFilterResult.ALLOW;
    },

    _writeMtreeFromDirectory: function(directory, setuidFiles, cancellable) {
        let mtree = new OSTree.MutableTree();

        let modifier = OSTree.RepoCommitModifier.new(OSTree.RepoCommitModifierFlags.SKIP_XATTRS,
                                                     Lang.bind(this, this._commitFilter, setuidFiles));
        this.ostreeRepo.write_directory_to_mtree(directory, mtree, modifier, cancellable);

        let [, file] = this.ostreeRepo.write_mtree(mtree, cancellable);
        return file;
    },

    _commit: function(branch, subject, file, cancellable, params) {
	params = Params.parse(params, { withParent: true });
        let [, parentRev] = this.ostreeRepo.resolve_rev(branch, true);
	let changed;
	if (parentRev) {
            let [, parent] = this.ostreeRepo.read_commit(parentRev, cancellable);
            changed = !file.equal(parent);
	} else {
	    changed = true;
	}

        if (changed) {
            let [, rev] = this.ostreeRepo.write_commit(params.withParent ? parentRev : null, subject, "", null, file, cancellable);
            this.ostreeRepo.transaction_set_ref(null, branch, rev);
            return rev;
        } else {
            return parentRev;
        }
    },

    _onBuildResultLine: function(src, result) {
    	let line;
    	try {
    	    [line,len] = src.read_line_finish_utf8(result);
    	} catch (e) {
    	    if (!e.domain) {
    		this._readingOutput = false;
		throw e;
	    }
	    line = '[INVALID UTF-8]';
    	}
    	if (line == null) {
    	    this._readingOutput = false;
    	    return;
    	}
	line += "\n";
    	let match = WARNING_RE.exec(line);
    	if (match && line.indexOf('libtool: ') != 0) {
	    if (JSUtil.stringEndswith(line, '[-Wdeprecated-declarations]\n')) {
		this._nDeprecations++;
    		this._deprecationOutputStream.write_all(line, null);
	    } else {
		this._nWarnings++;
    		this._warningOutputStream.write_all(line, null);
	    }
    	}
    	this._buildOutputStream.write_all(line, null);
    	src.read_line_async(0, null,
    			    Lang.bind(this, this._onBuildResultLine));
    },

    _openReplaceFile: function(path, cancellable) {
	GSystem.shutil_rm_rf(path, cancellable);
	return path.replace(null, false,
			    Gio.FileCreateFlags.REPLACE_DESTINATION,
			    cancellable);
    },

    _buildOneComponent: function(component, architecture, cancellable, params) {
	params = Params.parse(params, { installedTests: false });
        let basename = component['name'];

	if (params.installedTests)
	    basename = basename + '-installed-tests';
        let archBuildname = Format.vprintf('%s/%s', [basename, architecture]);
        let unixBuildname = archBuildname.replace(/\//g, '_');
        let buildRef = this._componentBuildRefFromName(basename, architecture);

        let currentVcsVersion = component['revision'];
        let expandedComponent = this._snapshot.getExpanded(component['name']);
        let previousMetadata = this._componentBuildCache[buildRef];
	let previousBuildVersion = null;
	let previousVcsVersion = null;
        if (previousMetadata != null) {
            previousBuildVersion = previousMetadata['ostree'];
            previousVcsVersion = previousMetadata['revision'];
        } else {
            print("No previous build for " + archBuildname);
	}

	let patchdir;
        if (expandedComponent['patches']) {
            let patchesRevision = expandedComponent['patches']['revision'];
            if (this._cachedPatchdirRevision == patchesRevision) {
                patchdir = this.patchdir;
            } else {
                patchdir = Vcs.checkoutPatches(this.mirrordir,
                                               this.patchdir,
                                               expandedComponent,
					       cancellable);
		this.patchdir = patchdir;
                this._cachedPatchdirRevision = patchesRevision;
	    }
            if ((previousMetadata != null) &&
                previousMetadata['patches'] &&
                previousMetadata['patches']['src'].indexOf('local:') != 0 &&
                previousMetadata['patches']['revision'] &&
                previousMetadata['patches']['revision'] == patchesRevision) {
                // Copy over the sha256sums
                expandedComponent['patches'] = previousMetadata['patches'];
            } else {
                let patchesSha256sums = this._computeSha256SumsForPatches(patchdir, expandedComponent, cancellable);
                expandedComponent['patches']['files_sha256sums'] = patchesSha256sums;
	    }
        } else {
            patchdir = null;
	}

        let forceRebuild = (this.forceBuildComponents[basename] ||
                            expandedComponent['src'].indexOf('local:') == 0);

        if (previousMetadata != null) {
            let rebuildReason = this._needsRebuild(previousMetadata, expandedComponent);
            if (rebuildReason == null) {
                if (!forceRebuild) {
                    print(Format.vprintf("Reusing cached build of %s at %s", [archBuildname, previousVcsVersion]));
                    return previousBuildVersion;
                } else {
                    print("Build forced regardless");
		}
            } else {
                print(Format.vprintf("Need rebuild of %s: %s", [archBuildname, rebuildReason]));
	    }
	}

	let cwd = Gio.File.new_for_path('.');
	let buildWorkdir = cwd.get_child('tmp-' + unixBuildname);
	GSystem.file_ensure_directory(buildWorkdir, true, cancellable);

        let tempMetadataPath = buildWorkdir.get_child('_ostbuild-meta.json');
        JsonUtil.writeJsonFileAtomic(tempMetadataPath, expandedComponent, cancellable);

        let componentSrc = buildWorkdir.get_child(basename);
        let childArgs = ['ostbuild', 'checkout', '--snapshot=' + this._snapshot.path.get_path(),
			 '--workdir=' + this.workdir.get_path(),
			 '--checkoutdir=' + componentSrc.get_path(),
			 '--metadata-path=' + tempMetadataPath.get_path(),
			 '--overwrite', basename];
        if (patchdir) {
            childArgs.push('--patches-path=' + patchdir.get_path());
	}
        ProcUtil.runSync(childArgs, cancellable, { logInitiation: true });

        GSystem.file_unlink(tempMetadataPath, cancellable);

        let componentResultdir = buildWorkdir.get_child('results');
        GSystem.file_ensure_directory(componentResultdir, true, cancellable);

	let rootdir;
	if (params.installedTests)
	    rootdir = this._composeBuildrootCore(buildWorkdir, basename, architecture,
						 [[this._installedTestsBuildrootRev[architecture], '/']], cancellable);
	else
            rootdir = this._composeBuildroot(buildWorkdir, basename, architecture, cancellable);

        let tmpdir=buildWorkdir.get_child('tmp');
        GSystem.file_ensure_directory(tmpdir, true, cancellable);

        let srcCompileOnePath = this.libdir.get_child('ostree-build-compile-one');
        let destCompileOnePath = rootdir.get_child('ostree-build-compile-one');
	srcCompileOnePath.copy(destCompileOnePath, Gio.FileCopyFlags.OVERWRITE,
			       cancellable, null);
        GSystem.file_chmod(destCompileOnePath, 493, cancellable);

        let chrootSourcedir = Gio.File.new_for_path('/ostbuild/source/' + basename);
	let chrootChdir = chrootSourcedir;

	let installedTestsSrcdir = componentSrc.get_child('installed-tests');
	if (params.installedTests) {
	    // We're just building the tests, set our source directory
	    let metaName = '_ostbuild-meta.json';
	    GSystem.file_rename(componentSrc.get_child(metaName), installedTestsSrcdir.get_child(metaName), cancellable);
	    chrootChdir = chrootSourcedir.get_child('installed-tests');
	    if (!componentSrc.query_exists(null)) {
		throw new Error("Component " + basename + " specified with installed tests, but no subdirectory found");
	    }
	}

        childArgs = ['setarch', architecture];
        childArgs.push.apply(childArgs, BuildUtil.getBaseUserChrootArgs());
        childArgs.push.apply(childArgs, [
            '--mount-readonly', '/',
            '--mount-bind', '/', '/sysroot',
            '--mount-proc', '/proc', 
            '--mount-bind', '/dev', '/dev',
            '--mount-bind', componentSrc.get_path(), chrootSourcedir.get_path(),
            '--mount-bind', componentResultdir.get_path(), '/ostbuild/results',
            '--chdir', chrootChdir.get_path(),
            rootdir.get_path(), '/ostree-build-compile-one',
            '--ostbuild-resultdir=/ostbuild/results',
            '--ostbuild-meta=_ostbuild-meta.json']);
	let envCopy = {};
	Lang.copyProperties(BuildUtil.BUILD_ENV, envCopy);
        envCopy['PWD'] = chrootSourcedir.get_path();
        envCopy['CFLAGS'] = OPT_COMMON_CFLAGS[architecture];
        envCopy['CXXFLAGS'] = OPT_COMMON_CFLAGS[architecture];

	let context = new GSystem.SubprocessContext({ argv: childArgs });
	context.set_stdout_disposition(GSystem.SubprocessStreamDisposition.PIPE);
	context.set_stderr_disposition(GSystem.SubprocessStreamDisposition.STDERR_MERGE);
	context.set_environment(ProcUtil.objectToEnvironment(envCopy));

	let buildOutputPath = Gio.File.new_for_path('log-' + basename + '.txt');
	this._buildOutputStream = this._openReplaceFile(buildOutputPath, cancellable);

	this._nWarnings = 0;
	let warningOutputPath = Gio.File.new_for_path('warnings-' + basename + '.txt');
	this._warningOutputStream = this._openReplaceFile(warningOutputPath, cancellable);

	this._nDeprecations = 0;
	let deprecationsOutputPath = Gio.File.new_for_path('deprecations-' + basename + '.txt');
	this._deprecationOutputStream = this._openReplaceFile(deprecationsOutputPath, cancellable);
	
	let proc = new GSystem.Subprocess({ context: context });
	proc.init(cancellable);
	print("Started child process " + context.argv.map(GLib.shell_quote).join(' '));

	let buildInputStream = proc.get_stdout_pipe();
	let buildInputDataStream = Gio.DataInputStream.new(buildInputStream);
	this._readingOutput = true;
	buildInputDataStream.read_line_async(0, cancellable,
					     Lang.bind(this, this._onBuildResultLine));

	let context = GLib.MainContext.default();
	while (this._readingOutput) {
	    context.iteration(true);
	}
	print("build output EOF");

	buildInputDataStream.close(null);
	this._buildOutputStream.close(null);
	this._warningOutputStream.close(null);
	this._deprecationOutputStream.close(null);
	if (this._nWarnings == 0)
	    GSystem.shutil_rm_rf(warningOutputPath, cancellable);
	if (this._nDeprecations == 0)
	    GSystem.shutil_rm_rf(deprecationsOutputPath, cancellable);
	try {
	    proc.wait_sync_check(cancellable);
	} catch (e) {
	    this._failedComponent = {'name': basename};
	    this._writeStatus(cancellable);
	    print("Build of " + basename + " failed");
	    throw e;
	}

	let finalBuildResultDir = buildWorkdir.get_child('post-results');
	GSystem.shutil_rm_rf(finalBuildResultDir, cancellable);
        GSystem.file_ensure_directory(finalBuildResultDir, true, cancellable);

	BuildUtil.timeSubtask("process build results", Lang.bind(this, function() {
	    this._processBuildResults(component, componentResultdir, finalBuildResultDir, cancellable);
	}));

        let recordedMetaPath = finalBuildResultDir.get_child('_ostbuild-meta.json');
        JsonUtil.writeJsonFileAtomic(recordedMetaPath, expandedComponent, cancellable);

        let setuidFiles = expandedComponent['setuid'] || [];

	let rev;
	BuildUtil.timeSubtask("commit build of " + buildRef, Lang.bind(this, function() {
            this.ostreeRepo.prepare_transaction(cancellable);
            let file = this._writeMtreeFromDirectory(finalBuildResultDir, setuidFiles, cancellable);
            rev = this._commit(buildRef, "Build", file, cancellable, { withParent: false });
            this.ostreeRepo.commit_transaction(cancellable);
            print("Commit component  " + buildRef + " is " + rev);
	}));
	BuildUtil.timeSubtask("clean build directory", Lang.bind(this, function() {
	    GSystem.shutil_rm_rf(buildWorkdir, cancellable);
	}));

        let ostreeRevision = this._saveComponentBuild(buildRef, rev, expandedComponent, cancellable);

	this._rebuiltComponents.push({ 'name': basename,
				       'warnings': this._nWarnings,
				       'deprecations': this._nDeprecations });
        return ostreeRevision;
    },

    _checkoutOneTreeCore: function(name, composeContents, cancellable, params) {
	params = Params.parse(params, { runTriggers: true });
        let composeRootdir = Gio.File.new_for_path(name);
	print("Checking out " + composeRootdir.get_path());
	GSystem.shutil_rm_rf(composeRootdir, cancellable);
        GSystem.file_ensure_directory(composeRootdir, true, cancellable);

        composeContents.forEach(Lang.bind(this, function([branch, subpath]) {
            let [, root] = this.ostreeRepo.read_commit(branch, cancellable);
            let subtree = root.resolve_relative_path(subpath);
            let subtreeInfo;
            try {
                subtreeInfo = subtree.query_info(OSTREE_GIO_FAST_QUERYINFO,
                                                 Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
                                                 cancellable);
            } catch(e if e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND)) {
                return;
            }

            this.ostreeRepo.checkout_tree(OSTree.RepoCheckoutMode.USER,
                                          OSTree.RepoCheckoutOverwriteMode.UNION_FILES,
                                          composeRootdir, subtree, subtreeInfo, cancellable);
        }));

	if (params.runTriggers)
	    this._runTriggersInRoot(composeRootdir, cancellable);

        let contentsPath = composeRootdir.resolve_relative_path('usr/share/contents.json');
	GSystem.file_ensure_directory(contentsPath.get_parent(), true, cancellable);
        JsonUtil.writeJsonFileAtomic(contentsPath, this._snapshot.data, cancellable);
        return composeRootdir;
    },

    _checkoutOneTree: function(target, componentBuildRevs, cancellable) {
        let base = target['base'];
        let baseName = this.osname + '/bases/' + base['name'];
        let [, baseRevision] = this.ostreeRepo.resolve_rev(baseName, false);

        let composeContents = [[baseRevision, '/']];
        for (let i = 0; i < target['contents'].length; i++) {
	    let treeContent = target['contents'][i];
            let name = treeContent['name'];
            let rev = componentBuildRevs[name];
            let subtrees = treeContent['trees'];
            for (let j = 0; j < subtrees.length; j++) {
		let subpath = subtrees[j];
                composeContents.push([rev, subpath]);
	    }
	}

	let composeRootdir = this._checkoutOneTreeCore(target['name'], composeContents, cancellable);
	this._postComposeTransform(composeRootdir, cancellable);
        return composeRootdir;
    },
    
    _runTriggersInRoot: function(rootdir, cancellable) {
	let triggersScriptPath = this.libdir.resolve_relative_path('gnome-ostree-run-triggers');
	let triggersPath = this.libdir.resolve_relative_path('triggers');
	
	// FIXME copy the triggers into the root temporarily; it'd be
	// better to add --mount-rbind to linux-user-chroot so we pick
	// up all the host mount points, and thus know we can find
	// data from outside the root.
	let tmpTriggersInRootPath = rootdir.get_child('tmp-triggers');
	GSystem.file_ensure_directory(tmpTriggersInRootPath, false, cancellable);
	let tmpTriggersScriptPath = tmpTriggersInRootPath.get_child(triggersScriptPath.get_basename());
	let tmpTriggersPath = tmpTriggersInRootPath.get_child(triggersPath.get_basename());

	triggersScriptPath.copy(tmpTriggersScriptPath, Gio.FileCopyFlags.OVERWRITE, cancellable,
				null);
	GSystem.shutil_cp_a(triggersPath, tmpTriggersPath, cancellable);

	let childArgs = BuildUtil.getBaseUserChrootArgs();
        childArgs.push.apply(childArgs, [
	    '--mount-bind', '/', '/sysroot',
            '--mount-proc', '/proc', 
            '--mount-bind', '/dev', '/dev',
            rootdir.get_path(), rootdir.get_relative_path(tmpTriggersScriptPath),
	    rootdir.get_relative_path(tmpTriggersPath)]);
	let envCopy = {};
	Lang.copyProperties(BuildUtil.BUILD_ENV, envCopy);
        envCopy['PWD'] = '/';

	let context = new GSystem.SubprocessContext({ argv: childArgs });
	context.set_environment(ProcUtil.objectToEnvironment(envCopy));
	let proc = new GSystem.Subprocess({ context: context });
	proc.init(cancellable);
	print("Started child process " + context.argv.map(GLib.shell_quote).join(' '));
	try {
	    proc.wait_sync_check(cancellable);
	} catch (e) {
	    print("Trigger execution in root " + rootdir.get_path() + " failed");
	    throw e;
	}

	GSystem.shutil_rm_rf(tmpTriggersInRootPath, cancellable);
    },

    _postComposeTransform: function(composeRootdir, cancellable) {
	// Move /etc to /usr/etc, since it contains defaults.
	let etc = composeRootdir.resolve_relative_path("etc");
	let usrEtc = composeRootdir.resolve_relative_path("usr/etc");
	GSystem.file_rename(etc, usrEtc, cancellable);

	// http://lists.freedesktop.org/archives/systemd-devel/2013-July/011770.html
	let machineId = '45bb3b96146aa94f299b9eb43646eb35\n'
	let machineIdPath = usrEtc.resolve_relative_path('machine-id');
	machineIdPath.replace_contents(machineId, null, false,
				       Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
    },

    _commitComposedTree: function(targetName, composeRootdir, cancellable) {
        let treename = this.osname + '/' + targetName;

	print("Preparing commit of " + composeRootdir.get_path() + " to " + targetName);
	let rev;
	BuildUtil.timeSubtask("compose " + targetName, Lang.bind(this, function() {
            this.ostreeRepo.prepare_transaction(cancellable);
            this.ostreeRepo.scan_hardlinks(cancellable);
            let file = this._writeMtreeFromDirectory(composeRootdir, [], cancellable);
            rev = this._commit(treename, "Compose", file, cancellable);
            this.ostreeRepo.commit_transaction(cancellable);
	}));
        print("Compose of " + targetName + " is " + rev);

        return [treename, rev];
    },

    // Return a SHA256 checksum of the contents of the kernel and all
    // modules; this is unlike an OSTree checksum in that we're just
    // checksumming the contents, not the uid/gid/xattrs.
    // Unfortunately, we can't rely on those for /boot anyways.
    _getKernelChecksum: function(kernelPath, kernelRelease, composeRootdir, cancellable) {
	let checksum = GLib.Checksum.new(GLib.ChecksumType.SHA256);
	let contents = GSystem.file_map_readonly(kernelPath, cancellable);
	checksum.update(contents.toArray());
	contents = null;
	let modulesPath = composeRootdir.resolve_relative_path('lib/modules/' + kernelRelease);
	if (modulesPath.query_exists(null)) {
	    // Only checksum .ko files; we don't want to pick up the
	    // modules.order file and such that might contain
	    // timestamps.
	    FileUtil.walkDir(modulesPath, { fileType: Gio.FileType.REGULAR,
					    nameRegex: /\.ko$/,
					    sortByName: true },
			     function (child, cancellable) {
				 let contents = GSystem.file_map_readonly(child, cancellable);
				 checksum.update(contents.toArray());
				 contents = null;
			     }, cancellable);
	}
	return checksum.get_string();
    },

    _prepareKernelAndInitramfs: function(architecture, composeRootdir, initramfsDepends, cancellable) {
	let e = composeRootdir.get_child('boot').enumerate_children('standard::*', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	let info;
	let kernelPath = null;
	while ((info = e.next_file(cancellable)) != null) {
	    let name = info.get_name();
	    let child = e.get_child(info);
            if (name == 'bzImage' && info.get_file_type() == Gio.FileType.SYMBOLIC_LINK) {
                GSystem.file_unlink(child, cancellable);
                continue;
            }
            // Canonicalize kernel name
	    if (name.indexOf('bzImage-') == 0) {
                let newname = name.replace('bzImage-', 'vmlinuz-');
                let targetChild = e.get_container().get_child(newname);
                GSystem.file_rename(child, targetChild, cancellable);
		kernelPath = targetChild;
                break;               
            } else if (name.indexOf('vmlinuz-') == 0) {
                kernelPath = child;
		break;
            }
	}
	e.close(cancellable);
	if (kernelPath === null)
	    throw new Error("Couldn't find vmlinuz- in compose root");

	let kernelName = kernelPath.get_basename();
	let releaseIdx = kernelName.indexOf('-');
	let kernelRelease = kernelName.substr(releaseIdx + 1);

	let kernelContentsChecksum = this._getKernelChecksum(kernelPath, kernelRelease, composeRootdir, cancellable);

        let initramfsCachedir = this.cachedir.resolve_relative_path('initramfs/' + architecture);
	GSystem.file_ensure_directory(initramfsCachedir, true, cancellable);

	let initramfsEpoch = this._snapshot.data['initramfs-build-epoch'];
	let initramfsEpochVersion = 0;
	if (initramfsEpoch)
	    initramfsEpochVersion = initramfsEpoch['version'];
	let fullInitramfsDependsString = 'epoch:' + initramfsEpochVersion +
	    ';kernel:' + kernelContentsChecksum + ';' +
	    initramfsDepends.join(';'); 
	let dependsChecksum = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256,
							      GLib.Bytes.new(fullInitramfsDependsString));

	let cachedInitramfsDirPath = initramfsCachedir.get_child(dependsChecksum);
	if (cachedInitramfsDirPath.query_file_type(Gio.FileQueryInfoFlags.NONE, null) == Gio.FileType.DIRECTORY) {
	    print("Reusing cached initramfs " + cachedInitramfsDirPath.get_path());
	} else {
	    print("No cached initramfs matching " + fullInitramfsDependsString);

	    // Clean out all old initramfs images
	    GSystem.shutil_rm_rf(initramfsCachedir, cancellable);

	    let cwd = Gio.File.new_for_path('.');
	    let workdir = cwd.get_child('tmp-initramfs-' + architecture);
	    let varTmp = workdir.resolve_relative_path('var/tmp');
	    GSystem.file_ensure_directory(varTmp, true, cancellable);
	    let varDir = varTmp.get_parent();
	    let tmpDir = workdir.resolve_relative_path('tmp');
	    GSystem.file_ensure_directory(tmpDir, true, cancellable);
	    let initramfsTmp = tmpDir.get_child('initramfs-ostree.img');

	    // HACK: Temporarily move /usr/etc to /etc to help dracut
	    // find stuff, like the config file telling it to use the
	    // ostree module.
	    let etcDir = composeRootdir.resolve_relative_path('etc');
	    let usrEtcDir = composeRootdir.resolve_relative_path('usr/etc');
	    GSystem.file_rename(usrEtcDir, etcDir, cancellable);
	    let args = [this._linuxUserChrootPath.get_path(),
			'--mount-proc', '/proc',
			'--mount-bind', '/dev', '/dev',
			'--mount-bind', '/', '/sysroot',
			'--mount-bind', tmpDir.get_path(), '/sysroot/tmp',
			'--mount-bind', varDir.get_path(), '/var',
			composeRootdir.get_path(),
			'dracut', '--tmpdir=/tmp', '-f', '/tmp/initramfs-ostree.img',
			kernelRelease];
	    
	    print("Running: " + args.map(GLib.shell_quote).join(' '));
	    BuildUtil.timeSubtask("dracut", Lang.bind(this, function() {
		let context = new GSystem.SubprocessContext({ argv: args });
		let proc = new GSystem.Subprocess({ context: context });
		proc.init(cancellable);
		proc.wait_sync_check(cancellable);
	    }));

	    // HACK: Move /etc back to /usr/etc
	    GSystem.file_rename(etcDir, usrEtcDir, cancellable);

	    GSystem.file_chmod(initramfsTmp, 420, cancellable);

	    let contents = GSystem.file_map_readonly(initramfsTmp, cancellable);
	    let initramfsContentsChecksum = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256, contents);
	    contents = null;

	    let tmpCachedInitramfsDirPath = cachedInitramfsDirPath.get_parent().get_child(cachedInitramfsDirPath.get_basename() + '.tmp');
	    GSystem.shutil_rm_rf(tmpCachedInitramfsDirPath, cancellable);
	    GSystem.file_ensure_directory(tmpCachedInitramfsDirPath, true, cancellable);

	    GSystem.file_rename(initramfsTmp, tmpCachedInitramfsDirPath.get_child('initramfs-' + kernelRelease + '-' + initramfsContentsChecksum), cancellable);
	    GSystem.file_linkcopy(kernelPath, tmpCachedInitramfsDirPath.get_child('vmlinuz-' + kernelRelease + '-' + kernelContentsChecksum),
				  Gio.FileCopyFlags.OVERWRITE, cancellable);
	    
	    GSystem.shutil_rm_rf(cachedInitramfsDirPath, cancellable);
	    GSystem.file_rename(tmpCachedInitramfsDirPath, cachedInitramfsDirPath, cancellable);
	}

	let cachedKernelPath = null;
	let cachedInitramfsPath = null;
	FileUtil.walkDir(cachedInitramfsDirPath, { fileType: Gio.FileType.REGULAR },
			 function (child, cancellable) {
			     if (child.get_basename().indexOf('initramfs-') == 0)
				 cachedInitramfsPath = child;
			     else if (child.get_basename().indexOf('vmlinuz-') == 0)
				 cachedKernelPath = child;
			 }, cancellable);
	if (cachedKernelPath == null || cachedInitramfsPath == null)
	    throw new Error("Missing file in " + cachedInitramfsDirPath);
	let cachedInitramfsPathName = cachedInitramfsPath.get_basename();
	let initramfsContentsChecksum = cachedInitramfsPathName.substr(cachedInitramfsPathName.lastIndexOf('-') + 1);

	let ostreeBootChecksum = GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256,
								  kernelContentsChecksum + initramfsContentsChecksum,
								  -1);
	
	return { kernelRelease: kernelRelease,
		 kernelPath: cachedKernelPath,
		 kernelChecksum: kernelContentsChecksum,
		 initramfsPath: cachedInitramfsPath,
	         initramfsContentsChecksum: initramfsContentsChecksum,
		 ostreeBootChecksum: ostreeBootChecksum };
    },

    // Clear out the target's /boot directory, and replace it with
    // kernel/initramfs that are named with the same
    // ostreeBootChecksum, derived from individual checksums
    _installKernelAndInitramfs: function(kernelInitramfsData, composeRootdir, cancellable) {
	let bootDir = composeRootdir.get_child('boot');
	GSystem.shutil_rm_rf(bootDir, cancellable);
	GSystem.file_ensure_directory(bootDir, true, cancellable);
	let targetKernelPath = bootDir.get_child('vmlinuz-' + kernelInitramfsData.kernelRelease + '-' + kernelInitramfsData.ostreeBootChecksum);
	GSystem.file_linkcopy(kernelInitramfsData.kernelPath, targetKernelPath, Gio.FileCopyFlags.ALL_METADATA, cancellable);
	let targetInitramfsPath = bootDir.get_child('initramfs-' + kernelInitramfsData.kernelRelease + '-' + kernelInitramfsData.ostreeBootChecksum);
	GSystem.file_linkcopy(kernelInitramfsData.initramfsPath, targetInitramfsPath, Gio.FileCopyFlags.ALL_METADATA, cancellable);
    },

    /* Build the Yocto base system. */
    _buildBase: function(architecture, cancellable) {
        let basemeta = this._snapshot.getExpanded(this._snapshot.data['base']['name']);
	let basename = basemeta['name'];
	let buildWorkdir = Gio.File.new_for_path('build-' + basemeta['name'] + '-' + architecture);
        let checkoutdir = buildWorkdir.get_child(basemeta['name']);
        let builddirName = Format.vprintf('build-%s-%s', [basename, architecture]);
        let builddir = this.workdir.get_child(builddirName);
	let buildname = 'bases/' + basename + '-' + architecture;

        let forceRebuild = false; // (this.forceBuildComponents[basename] ||
                                  // basemeta['src'].indexOf('local:') == 0);

        let previousBuild = this._componentBuildCache[buildname];
	let previousVcsVersion = null;
	if (previousBuild != null) {
	    previousVcsVersion = previousBuild['revision'];
	}
	if (forceRebuild) {
	    print(Format.vprintf("%s forced rebuild", [builddirName]));
	} else if (previousVcsVersion == basemeta['revision']) {
	    print(Format.vprintf("Already built %s at %s", [builddirName, previousVcsVersion]));
	    return;
	} else if (previousVcsVersion != null) {
	    print(Format.vprintf("%s was %s, now at revision %s", [builddirName, previousVcsVersion, basemeta['revision']]));
	} 

	let ftype = checkoutdir.query_file_type(Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
        if (ftype == Gio.FileType.SYMBOLIC_LINK)
	    GSystem.file_unlink(checkoutdir, cancellable);

	GSystem.file_ensure_directory(checkoutdir.get_parent(), true, cancellable);

        let [keytype, uri] = Vcs.parseSrcKey(basemeta['src']);
        if (keytype == 'local') {
	    GSystem.shutil_rm_rf(checkoutdir, cancellable);
	    checkoutdir.make_symbolic_link(uri, cancellable);
        } else {
            Vcs.getVcsCheckout(this.mirrordir, basemeta, checkoutdir, cancellable,
                               {overwrite:false});
	}

        // Just keep reusing the old working directory downloads and sstate
        let oldBuilddir = this.workdir.get_child('build-' + basemeta['name']);
        let sstateDir = oldBuilddir.get_child('sstate-cache');
        let downloads = oldBuilddir.get_child('downloads');

        let cmd = [this.libdir.get_path() + '/ostree-build-yocto',
		   checkoutdir.get_path(), builddir.get_path(), architecture,
		   this.repo.get_path()];
        // We specifically want to kill off any environment variables jhbuild
        // may have set.
        let env = {};
	Lang.copyProperties(BuildUtil.BUILD_ENV, env);
        env['DL_DIR'] = downloads.get_path();
        env['SSTATE_DIR'] = sstateDir.get_path();
        ProcUtil.runSync(cmd, cancellable, {env:ProcUtil.objectToEnvironment(env)});

	let componentTypes = ['runtime', 'devel'];
        for (let i = 0; i < componentTypes.length; i++) {
	    let componentType = componentTypes[i];
	    let treename = Format.vprintf('%s/bases/%s/%s-%s', [this.osname, basename, architecture, componentType]);
	    let tarPath = builddir.get_child(Format.vprintf('gnomeos-contents-%s-%s.tar.gz', [componentType, architecture]));

            this.ostreeRepo.prepare_transaction(cancellable);
            let mtree = new OSTree.MutableTree();
            this.ostreeRepo.write_archive_to_mtree(tarPath, mtree, null, false, cancellable);
            let [, file] = this.ostreeRepo.write_mtree(mtree, cancellable);
            this._commit(treename, "Build", file, cancellable);
            this.ostreeRepo.commit_transaction(cancellable);

	    GSystem.file_unlink(tarPath, cancellable);
	}

	GSystem.shutil_rm_rf(checkoutdir, cancellable);

	this._rebuiltComponents.push({'name': basename});
	
	this._writeComponentCache(buildname, basemeta, cancellable);
    },

    _findTargetInList: function(name, targetList) {
	for (let i = 0; i < targetList.length; i++) {
	    if (targetList[i]['name'] == name)
		return targetList[i];
	}
	throw new Error("Failed to find target " + name);
    },

    _writeStatus: function(cancellable) {
	let statusTxtPath = Gio.File.new_for_path('status.txt');
	let msg = '';
	if (this._rebuiltComponents.length > 0) {
	    msg += 'built:';
	    for (let i = 0; i < this._rebuiltComponents.length; i++) {
		msg += ' ' + this._rebuiltComponents[i]['name'];
	    }
	}
	if (this._failedComponent)
	    msg += ' failed: ' + this._failedComponent['name'];
	statusTxtPath.replace_contents(msg + '\n', null, false,
				       Gio.FileCreateFlags.REPLACE_DESTINATION,
				       cancellable);
	let buildDataPath = Gio.File.new_for_path('build.json');
	let buildData = {'built': this._rebuiltComponents,
			 'failed': this._failedComponent };
	JsonUtil.writeJsonFileAtomic(buildDataPath, buildData, cancellable);
    },

    _cleanupGarbage: function(rootdir, cancellable) {
	// Something is injecting this; probably from the triggers?
	// Just nuke it.
	let f = rootdir.get_child('.python-history');
	if (f.query_exists(null))
	    GSystem.file_unlink(f, cancellable);
    },

    execute: function(cancellable) {

	this._linuxUserChrootPath = BuildUtil.findUserChrootPath();
	if (!this._linuxUserChrootPath)
	    throw new Error("You must have linux-user-chroot installed");

        this.forceBuildComponents = {};
	for (let i = 0; i < this.parameters.forceComponents.length; i++)
	    this.forceBuildComponents[this.parameters.forceComponents[i]] = true;
        this.cachedPatchdirRevision = null;

	let snapshotPath = this.builddir.get_child('snapshot.json');
	let workingSnapshotPath = Gio.File.new_for_path('snapshot.json');
	GSystem.file_linkcopy(snapshotPath, workingSnapshotPath, Gio.FileCopyFlags.OVERWRITE,
			      cancellable);
	this._snapshot = Snapshot.fromFile(workingSnapshotPath, cancellable);
        let osname = this._snapshot.data['osname'];
	this.osname = osname;

	this._failedComponent = null;
	this._rebuiltComponents = [];

	this.patchdir = this.workdir.get_child('patches');

        let components = this._snapshot.data['components'];

	// Pick up overrides from $workdir/overrides/$name
        for (let i = 0; i < components.length; i++) {
	    let component = components[i];
	    let name = component['name'];
	    let overridePath = this.workdir.resolve_relative_path('overrides/' + name);
	    if (overridePath.query_exists(null)) {
		print("Using override:  " + overridePath.get_path());
		component['src'] = 'local:' + overridePath.get_path();
		delete component['tag'];
		delete component['tag-reason'];
		// We don't want to attempt to apply patches over top
		// of what the override has.
		delete component['patches'];
	    }
	}

        this._componentBuildCachePath = this.cachedir.get_child('component-builds.json');
        if (this._componentBuildCachePath.query_exists(cancellable)) {
            this._componentBuildCache = JsonUtil.loadJson(this._componentBuildCachePath, cancellable);
        } else {
            this._componentBuildCache = {};
	}

        let baseName = this._snapshot.data['base']['name'];
        let architectures = this._snapshot.data['architectures'];

        for (let i = 0; i < architectures.length; i++) {
            this._buildBase(architectures[i], cancellable);
	}

        let componentToArches = {};

        let runtimeComponents = [];
        let develComponents = [];
        let testingComponents = [];

        for (let i = 0; i < components.length; i++) {
	    let component = components[i];
            let name = component['name']

            let isRuntime = (component['component'] || 'runtime') == 'runtime';
            let isTesting = (component['component'] || 'runtime') == 'testing';

            if (isRuntime) {
                runtimeComponents.push(component);
	    } else if (isTesting) {
		testingComponents.push(component);
	    }
	    develComponents.push(component);

	    let isNoarch = component['noarch'] || false;
	    let componentArches;
            if (isNoarch) {
                // Just use the first specified architecture
                componentArches = [architectures[0]];
            } else {
                componentArches = component['architectures'] || architectures;
	    }
            componentToArches[name] = componentArches;
	}

        let componentsToBuild = [];
        let componentSkippedCount = 0;
        let componentBuildRevs = {};

        for (let i = 0; i < components.length; i++) {
	    let component = components[i];
            for (let j = 0; j < architectures.length; j++) {
                componentsToBuild.push([component, architectures[j]]);
	    }
	}

	let previousBuildEpoch = this._componentBuildCache['build-epoch'];
	let currentBuildEpoch = this._snapshot.data['build-epoch'];
	if (previousBuildEpoch === undefined ||
	    (currentBuildEpoch !== undefined &&
	     previousBuildEpoch['version'] < currentBuildEpoch['version'])) {
	    let currentEpochVer = currentBuildEpoch['version'];
	    let rebuildAll = currentBuildEpoch['all'];
	    let rebuilds = [];
	    if (rebuildAll) {
		for (let i = 0; i < components.length; i++) {
		    rebuilds.push(components[i]['name']);
		}
	    } else {
		rebuilds = currentBuildEpoch['component-names'];
	    }
	    for (let i = 0; i < rebuilds.length; i++) {
		let component = this._snapshot.getComponent(rebuilds[i]);
		let name = component['name'];
		print("Component " + name + " build forced via epoch");
		for (let j = 0; j < architectures.length; j++) {
		    let buildRef = this._componentBuildRef(component, architectures[j]);
		    delete this._componentBuildCache[buildRef];
		}
	    }
	}

	this._componentBuildCache['build-epoch'] = currentBuildEpoch;
        JsonUtil.writeJsonFileAtomic(this._componentBuildCachePath, this._componentBuildCache, cancellable);

        for (let i = 0; i < componentsToBuild.length; i++) {
	    let [component, architecture] = componentsToBuild[i];
            let archname = component['name'] + '/' + architecture;
            let buildRev = this._buildOneComponent(component, architecture, cancellable);
            componentBuildRevs[archname] = buildRev;
	}

        let targetsList = [];
	let componentTypes = ['runtime', 'devel-debug'];
        for (let i = 0; i < componentTypes.length; i++) {
	    let targetComponentType = componentTypes[i];
            for (let i = 0; i < architectures.length; i++) {
		let architecture = architectures[i];
                let target = {};
                targetsList.push(target);
                target['name'] = 'buildmaster/' + architecture + '-' + targetComponentType;

                let baseRuntimeRef = baseName + '/' + architecture + '-runtime';
                let buildrootRef = baseName + '/' + architecture + '-devel';
		let baseRef;
                if (targetComponentType == 'runtime') {
                    baseRef = baseRuntimeRef;
                } else {
                    baseRef = buildrootRef;
		}
                target['base'] = {'name': baseRef,
                                  'runtime': baseRuntimeRef,
                                  'devel': buildrootRef};

		let targetComponents;
                if (targetComponentType == 'runtime') {
                    targetComponents = runtimeComponents;
                } else {
                    targetComponents = develComponents;
		}
                    
                let contents = [];
                for (let i = 0; i < targetComponents.length; i++) {
		    let component = targetComponents[i];
                    if (component['bootstrap']) {
                        continue;
		    }
                    let buildsForComponent = componentToArches[component['name']];
                    if (buildsForComponent.indexOf(architecture) == -1) {
			continue;
		    }
                    let binaryName = component['name'] + '/' + architecture;
                    let componentRef = {'name': binaryName};
                    if (targetComponentType == 'runtime') {
                        componentRef['trees'] = ['/runtime'];
		    } else if (targetComponentType == 'devel-debug') {
                        componentRef['trees'] = ['/runtime', '/devel', '/tests', '/doc', '/debug'];
		    }
                    contents.push(componentRef);
		}
                target['contents'] = contents;
	    }
	}

	this._installedTestsBuildrootRev = {};
	let targetRevisions = {};
	let finalInstalledTestRevisions = {};
	let buildData = { snapshotName: this._snapshot.path.get_basename(),
			  snapshot: this._snapshot.data,
			  targets: targetRevisions };
	buildData['installed-tests'] = finalInstalledTestRevisions;

	// First loop over the -devel trees per architecture, and
	// generate an initramfs.
	let archInitramfsImages = {};
        for (let i = 0; i < architectures.length; i++) {
	    let architecture = architectures[i];
	    let develTargetName = 'buildmaster/' + architecture + '-devel-debug';
	    let develTarget = this._findTargetInList(develTargetName, targetsList);

	    // Gather a list of components upon which the initramfs depends
	    let initramfsDepends = [];
	    for (let j = 0; j < components.length; j++) {
		let component = components[j];
		if (!component['initramfs-depends'])
		    continue;
		let archname = component['name'] + '/' + architecture;
		let buildRev = componentBuildRevs[archname];
		initramfsDepends.push(component['name'] + ':' + buildRev);
	    }

	    let composeRootdir;
	    BuildUtil.timeSubtask("checkout " + develTargetName, Lang.bind(this, function() {
		composeRootdir = this._checkoutOneTree(develTarget, componentBuildRevs, cancellable);
	    }));
	    let kernelInitramfsData = this._prepareKernelAndInitramfs(architecture, composeRootdir, initramfsDepends, cancellable);
	    archInitramfsImages[architecture] = kernelInitramfsData;
	    this._installKernelAndInitramfs(kernelInitramfsData, composeRootdir, cancellable);
	    let [treename, ostreeRev] = this._commitComposedTree(develTargetName, composeRootdir, cancellable);
	    BuildUtil.timeSubtask("cleanup " + develTargetName, Lang.bind(this, function() {
		GSystem.shutil_rm_rf(composeRootdir, cancellable);
	    }));
	    targetRevisions[treename] = ostreeRev;
	    // Also note the revision of this, since it will be used
	    // as the buildroot for installed tests
	    this._installedTestsBuildrootRev[architecture] = ostreeRev;
	}

	// Now loop over the other targets per architecture, reusing
	// the initramfs cached from -devel generation.
	for (let i = 0; i < componentTypes.length; i++) {
	    let target = componentTypes[i];
	    if (target == 'devel-debug')
		continue;
            for (let j = 0; j < architectures.length; j++) {
		let architecture = architectures[j];
		let runtimeTargetName = 'buildmaster/' + architecture + '-' + target;
		let runtimeTarget = this._findTargetInList(runtimeTargetName, targetsList);

		let composeRootdir;
		BuildUtil.timeSubtask("checkout " + runtimeTargetName, Lang.bind(this, function() {
		    composeRootdir = this._checkoutOneTree(runtimeTarget, componentBuildRevs, cancellable);
		}));
		let kernelInitramfsData = archInitramfsImages[architecture];
		this._installKernelAndInitramfs(kernelInitramfsData, composeRootdir, cancellable);
		this._cleanupGarbage(composeRootdir, cancellable);
		let [treename, ostreeRev] = this._commitComposedTree(runtimeTargetName, composeRootdir, cancellable);
		BuildUtil.timeSubtask("cleanup " + runtimeTargetName, Lang.bind(this, function() {
		    GSystem.shutil_rm_rf(composeRootdir, cancellable);
		}));
		targetRevisions[treename] = ostreeRev;
	    }
	}

	let installedTestComponentNames = this._snapshot.data['installed-tests-components'] || [];
	print("Using installed test components: " + installedTestComponentNames.join(', '));
	let installedTestContents = {};
        for (let i = 0; i < architectures.length; i++) {
	    installedTestContents[architectures[i]] = [];
	}
	for (let i = 0; i < testingComponents.length; i++) {
	    let component = testingComponents[i];
	    let name = component['name'];
            for (let j = 0; j < architectures.length; j++) {
		let architecture = architectures[j];
		let archname = component['name'] + '/' + architecture;
		let rev = componentBuildRevs[archname];
		if (!rev)
		    throw new Error("no build for " + buildRef);
		installedTestContents[architecture].push([rev, '/runtime']);
	    }
	}
	for (let i = 0; i < runtimeComponents.length; i++) {
	    let component = runtimeComponents[i];
	    for (let j = 0; j < architectures.length; j++) {
		let architecture = architectures[j];
		let archname = component['name'] + '/' + architecture;
		let rev = componentBuildRevs[archname];
		installedTestContents[architecture].push([rev, '/tests'])
	    }
	}
        for (let i = 0; i < installedTestComponentNames.length; i++) {
	    let componentName = installedTestComponentNames[i];
            for (let j = 0; j < architectures.length; j++) {
		let architecture = architectures[j];
		let archname = componentName + '-installed-tests' + '/' + architecture;
		let component = this._snapshot.getComponent(componentName);
		let buildRev = this._buildOneComponent(component, architecture, cancellable, { installedTests: true });
		installedTestContents[architecture].push([buildRev, '/runtime']);
		installedTestContents[architecture].push([buildRev, '/tests']);
	    }
	}
	for (let architecture in installedTestContents) {
	    let rootName = 'buildmaster/' + architecture + '-installed-tests';
	    let composeContents = [];
	    let contents = installedTestContents[architecture];
            for (let j = 0; j < contents.length; j++) {
		composeContents.push(contents[j]);
	    }

	    let composeRootdir = this._checkoutOneTreeCore(rootName, composeContents, cancellable, { runTriggers: false });
	    let [treename, rev] = this._commitComposedTree(rootName, composeRootdir, cancellable);
	    GSystem.shutil_rm_rf(composeRootdir, cancellable);
	    finalInstalledTestRevisions[treename] = rev;
	}

	this._writeStatus(cancellable);

	JsonUtil.writeJsonFileAtomic(this.builddir.get_child('build.json'), buildData, cancellable);
    }
});
