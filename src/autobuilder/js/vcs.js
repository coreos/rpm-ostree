// Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

const Params = imports.params;
const Format = imports.format;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const GSystem = imports.gi.GSystem;

const ProcUtil = imports.procutil;
const BuildUtil = imports.buildutil;
const JSUtil = imports.jsutil;

function getMirrordir(mirrordir, keytype, uri, params) {
    params = Params.parse(params, {prefix: ''});
    let colon = uri.indexOf('://');
    let scheme, rest;
    if (colon >= 0) {
        scheme = uri.substr(0, colon);
        rest = uri.substr(colon+3);
    } else {
        scheme = 'file';
        if (GLib.path_is_absolute(uri))
            rest = uri.substr(1);
        else
            rest = uri;
    }
    let prefix = params.prefix ? params.prefix + '/' : '';
    let relpath = prefix + keytype + '/' + scheme + '/' + rest;
    return mirrordir.resolve_relative_path(relpath);
}

function _processCheckoutSubmodules(mirrordir, parentUri, cwd, cancellable) {
    let lines = ProcUtil.runSyncGetOutputLines(['git', 'submodule', 'status'],
					       cancellable, {cwd: cwd}); 
    let haveSubmodules = false;
    for (let i = 0; i < lines.length; i++) {
	let line = lines[i];
        if (line == '') continue;
        haveSubmodules = true;
        line = line.substr(1);
        let [subChecksum, subName, rest] = line.split(' ');
	let configKey = Format.vprintf('submodule.%s.url', [subName]);
        let subUrl = ProcUtil.runSyncGetOutputUTF8Stripped(['git', 'config', '-f', '.gitmodules', configKey],
							   cancellable, {cwd: cwd});
	print("processing submodule " + subUrl);
	if (subUrl.indexOf('../') == 0) {
	    subUrl = _makeAbsoluteUrl(parentUri, subUrl);
	}
        let localMirror = getMirrordir(mirrordir, 'git', subUrl);
	ProcUtil.runSync(['git', 'config', configKey, 'file://' + localMirror.get_path()],
			 cancellable, {cwd:cwd});
        ProcUtil.runSync(['git', 'submodule', 'update', '--init', subName], cancellable, {cwd: cwd});
	_processCheckoutSubmodules(mirrordir, subUrl, cwd.get_child(subName), cancellable);
    }
}

function getVcsCheckout(mirrordir, component, dest, cancellable, params) {
    params = Params.parse(params, {overwrite: true,
				   quiet: false});
    
    let [keytype, uri] = parseSrcKey(component['src']);
    let revision;
    let moduleMirror;
    let addUpstream;
    if (keytype == 'git' || keytype == 'local') {
	revision = component['revision'];
	moduleMirror = getMirrordir(mirrordir, keytype, uri);
	addUpstream = true;
    } else if (keytype == 'tarball') {
	revision = 'tarball-import-' + component['checksum'];
	moduleMirror = getMirrordir(mirrordir, 'tarball', component['name']);
	addUpstream = false;
    } else {
	throw new Error("Unsupported src uri");
    }
    let checkoutdirParent = dest.get_parent();
    GSystem.file_ensure_directory(checkoutdirParent, true, cancellable);
    let tmpDest = checkoutdirParent.get_child(dest.get_basename() + '.tmp');
    GSystem.shutil_rm_rf(tmpDest, cancellable);
    let ftype = dest.query_file_type(Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
    if (ftype == Gio.FileType.SYMBOLIC_LINK) {
        GSystem.file_unlink(dest, cancellable);
    } else if (ftype == Gio.FileType.DIRECTORY) {
        if (params.overwrite) {
	    GSystem.shutil_rm_rf(dest, cancellable);
        } else {
            tmpDest = dest;
	}
    }
    ftype = tmpDest.query_file_type(Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
    if (ftype != Gio.FileType.DIRECTORY) {
        ProcUtil.runSync(['git', 'clone', '-q', '--origin', 'localmirror',
			  '--no-checkout', moduleMirror.get_path(), tmpDest.get_path()], cancellable);
	if (addUpstream)
            ProcUtil.runSync(['git', 'remote', 'add', 'upstream', uri], cancellable, {cwd: tmpDest});
    } else {
        ProcUtil.runSync(['git', 'fetch', 'localmirror'], cancellable, {cwd: tmpDest});
    }
    ProcUtil.runSync(['git', 'checkout', '-q', revision], cancellable, {cwd: tmpDest});
    _processCheckoutSubmodules(mirrordir, uri, tmpDest, cancellable);
    if (!tmpDest.equal(dest)) {
        GSystem.file_rename(tmpDest, dest, cancellable);
    }
    return dest;
}

function clean(keytype, checkoutdir, cancellable) {
    ProcUtil.runSync(['git', 'clean', '-d', '-f', '-x'], cancellable,
		     {cwd: checkoutdir});
}

function parseSrcKey(srckey) {
    let idx = srckey.indexOf(':');
    if (idx < 0) {
        throw new Error("Invalid SRC uri=" + srckey);
    }
    let keytype = srckey.substr(0, idx);
    if (!(keytype == 'git' || keytype == 'local' || keytype == 'tarball')) {
        throw new Error("Unsupported SRC uri=" + srckey);
    }
    let uri = srckey.substr(idx+1);
    return [keytype, uri];
}
    
function checkoutPatches(mirrordir, patchdir, component, cancellable) {
    let patches = component['patches'];

    let [patchesKeytype, patchesUri] = parseSrcKey(patches['src']);
    if (patchesKeytype == 'local')
	return Gio.File.new_for_path(patchesUri);
    else if (patchesKeytype != 'git')
	throw new Error("Unhandled keytype " + patchesKeytype);

    let patchesMirror = getMirrordir(mirrordir, patchesKeytype, patchesUri);
    getVcsCheckout(mirrordir, patches, patchdir, cancellable,
                   {overwrite: true,
                    quiet: true});
    return patchdir;
}

function getLastfetchPath(mirrordir, keytype, uri, branch) {
    let mirror = getMirrordir(mirrordir, keytype, uri);
    let branchSafename = branch.replace(/[\/.]/g, '_');
    return mirror.get_parent().get_child(mirror.get_basename() + '.lastfetch-' + branchSafename);
}

function _listSubmodules(mirrordir, mirror, keytype, uri, branch, cancellable) {
    let currentVcsVersion = ProcUtil.runSyncGetOutputUTF8(['git', 'rev-parse', branch], cancellable,
							  {cwd: mirror}).replace(/[ \n]/g, '');
    let tmpCheckout = getMirrordir(mirrordir, keytype, uri, {prefix:'_tmp-checkouts'});
    GSystem.shutil_rm_rf(tmpCheckout, cancellable);
    GSystem.file_ensure_directory(tmpCheckout.get_parent(), true, cancellable);
    ProcUtil.runSync(['git', 'clone', '-q', '--no-checkout', mirror.get_path(), tmpCheckout.get_path()], cancellable);
    ProcUtil.runSync(['git', 'checkout', '-q', '-f', currentVcsVersion], cancellable,
		     {cwd: tmpCheckout});
    let submodules = []
    let lines = ProcUtil.runSyncGetOutputLines(['git', 'submodule', 'status'],
					       cancellable, {cwd: tmpCheckout}); 
    for (let i = 0; i < lines.length; i++) {
	let line = lines[i];
        if (line == '') continue;
        line = line.substr(1);
        let [subChecksum, subName, rest] = line.split(' ');
        let subUrl = ProcUtil.runSyncGetOutputUTF8Stripped(['git', 'config', '-f', '.gitmodules',
							    Format.vprintf('submodule.%s.url', [subName])], cancellable,
							   {cwd: tmpCheckout});
        submodules.push([subChecksum, subName, subUrl]);
    }
    GSystem.shutil_rm_rf(tmpCheckout, cancellable);
    return submodules;
}

function _makeAbsoluteUrl(parent, relpath)
{
    let origParent = parent;
    let origRelpath = relpath;
    if (JSUtil.stringEndswith(parent, '/'))
	parent = parent.substr(0, parent.length - 1);
    let methodIndex = parent.indexOf("://");
    if (methodIndex == -1)
	throw new Error("Invalid method");
    let firstSlash = parent.indexOf('/', methodIndex + 3);
    if (firstSlash == -1)
	throw new Error("Invalid url");
    let parentPath = parent.substr(firstSlash);
    while (relpath.indexOf('../') == 0) {
	let i = parentPath.lastIndexOf('/');
	if (i < 0)
	    throw new Error("Relative path " + origRelpath + " is too long for parent " + origParent);
	relpath = relpath.substr(3);
	parentPath = parentPath.substr(0, i);
    }
    parent = parent.substr(0, firstSlash) + parentPath;
    if (relpath.length == 0)
	return parent;
    return parent + '/' + relpath;
}

function ensureVcsMirror(mirrordir, component, cancellable,
			 params) {
    params = Params.parse(params, { fetch: false,
				    fetchKeepGoing: false,
				    timeoutSec: 0 });
    let [keytype, uri] = parseSrcKey(component['src']);
    if (keytype == 'git' || keytype == 'local') {
	let branch = component['branch'] || component['tag'];
	return _ensureVcsMirrorGit(mirrordir, uri, branch, cancellable, params);
    } else if (keytype == 'tarball') {
	let name = component['name'];
	let checksum = component['checksum'];
	if (!checksum) {
	    throw new Error("Component " + name + " missing checksum attribute");
	}
	return _ensureVcsMirrorTarball(mirrordir, name, uri, checksum, cancellable, params);
    } else {
	throw new Error("Unhandled keytype=" + keytype);
    }
}

function _ensureVcsMirrorGit(mirrordir, uri, branch, cancellable, params) {
    let keytype = 'git';
    let fetch = params.fetch;
    let mirror = getMirrordir(mirrordir, keytype, uri);
    let tmpMirror = mirror.get_parent().get_child(mirror.get_basename() + '.tmp');
    let didUpdate = false;
    let lastFetchPath = getLastfetchPath(mirrordir, keytype, uri, branch);
    let lastFetchContents = null;
    let currentTime = GLib.DateTime.new_now_utc();
    let lastFetchContents = null;
    let lastFetchInfo = null;
    try {
	lastFetchInfo = lastFetchPath.query_info('time::modified', Gio.FileQueryInfoFlags.NONE, cancellable);
    } catch (e) {
	if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
	    throw e;
    }
    if (lastFetchInfo != null) {
	lastFetchContents = GSystem.file_load_contents_utf8(lastFetchPath, cancellable).replace(/[ \n]/g, '');
	if (params.timeoutSec > 0) {
	    let lastFetchTime = GLib.DateTime.new_from_unix_local(lastFetchInfo.get_attribute_uint64('time::modified'));
	    let diff = currentTime.difference(lastFetchTime) / 1000 / 1000;
	    if (diff < params.timeoutSec) {
		fetch = false;
	    }
	}
    }
    GSystem.shutil_rm_rf(tmpMirror, cancellable);
    if (!mirror.query_exists(cancellable)) {
        ProcUtil.runSync(['git', 'clone', '--mirror', uri, tmpMirror.get_path()], cancellable,
			 { logInitiation: true });
        ProcUtil.runSync(['git', 'config', 'gc.auto', '0'], cancellable,
			 { cwd: tmpMirror,
			   logInitiation: true });
        GSystem.file_rename(tmpMirror, mirror, cancellable);
    } else if (fetch) {
	try {
            ProcUtil.runSync(['git', 'fetch'], cancellable, { cwd: mirror,
							      logInitiation: true });
	} catch (e) {
	    if (!params.fetchKeepGoing)
		throw e;
	}
    }

    let currentVcsVersion = ProcUtil.runSyncGetOutputUTF8(['git', 'rev-parse', branch], cancellable,
							  {cwd: mirror}).replace(/[ \n]/g, '');

    let changed = currentVcsVersion != lastFetchContents; 
    if (changed) {
        print(Format.vprintf("last fetch %s differs from branch %s", [lastFetchContents, currentVcsVersion]));
	_listSubmodules(mirrordir, mirror, keytype, uri, branch, cancellable).forEach(function (elt) {
	    let [subChecksum, subName, subUrl] = elt;
	    print("Processing submodule " + subName + " at " + subChecksum + " from " + subUrl);
	    if (subUrl.indexOf('../') == 0) {
		subUrl = _makeAbsoluteUrl(uri, subUrl);
		print("Absolute URL: " + subUrl);
	    }
            _ensureVcsMirrorGit(mirrordir, subUrl, subChecksum, cancellable, params);
	});
    }
    
    if (changed || (fetch && params.timeoutSec > 0)) {
	lastFetchPath.replace_contents(currentVcsVersion, null, false, 0, cancellable); 
    }

    return mirror;
}

function _ensureVcsMirrorTarball(mirrordir, name, uri, checksum, cancellable, params) {
    let fetch = params.fetch;
    let mirror = getMirrordir(mirrordir, 'tarball', name);
    let tmpMirror = mirror.get_parent().get_child(mirror.get_basename() + '.tmp');
    
    if (!mirror.query_exists(cancellable)) {
	GSystem.shutil_rm_rf(tmpMirror, cancellable);
	GSystem.file_ensure_directory(tmpMirror, true, cancellable);
	ProcUtil.runSync(['git', 'init', '--bare'], cancellable,
			 { cwd: tmpMirror, logInitiation: true });
        ProcUtil.runSync(['git', 'config', 'gc.auto', '0'], cancellable,
			 { cwd: tmpMirror, logInitiation: true });
	GSystem.file_rename(tmpMirror, mirror, cancellable);
    }
    
    let importTag = 'tarball-import-' + checksum;
    let gitRevision = ProcUtil.runSyncGetOutputUTF8StrippedOrNull(['git', 'rev-parse', importTag],
								  cancellable, { cwd: mirror });
    if (gitRevision != null) {
	return mirror;
    }	

    // First, we get a clone of the tarball git repo
    let tmpCheckoutPath = mirrordir.get_child('tarball-cwd-' + name);
    GSystem.shutil_rm_rf(tmpCheckoutPath, cancellable);
    ProcUtil.runSync(['git', 'clone', mirror.get_path(), tmpCheckoutPath.get_path()], cancellable,
		     { logInitiation: true });
    // Now, clean the contents out
    ProcUtil.runSync(['git', 'rm', '-r', '--ignore-unmatch', '.'], cancellable,
		     { cwd: tmpCheckoutPath,
		       logInitiation: true });

    // Download the tarball
    let tmpPath = mirrordir.get_child('tarball-' + name);
    GSystem.shutil_rm_rf(tmpPath, cancellable);
    GSystem.file_ensure_directory(tmpPath.get_parent(), true, cancellable);
    ProcUtil.runSync(['curl', '-L', '-v', '-o', tmpPath.get_path(), uri], cancellable,
		     { logInitiation: true });

    // And verify the checksum
    let tarballData = GSystem.file_map_readonly(tmpPath, cancellable);
    let actualChecksum = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256, tarballData);
    if (actualChecksum != checksum) {
	throw new Error("Downloaded " + uri + " expected checksum=" + checksum + " actual=" + actualChecksum);
    }

    let decompOpt = null;
    if (JSUtil.stringEndswith(uri, '.xz'))
	decompOpt = '--xz';
    else if (JSUtil.stringEndswith(uri, '.bz2'))
	decompOpt = '--bzip2';
    else if (JSUtil.stringEndswith(uri, '.gz'))
	decompOpt = '--gzip';
    
    // Extract the tarball to our checkout
    let args = ['tar', '-C', tmpCheckoutPath.get_path(), '-x'];
    if (decompOpt !== null)
	args.push(decompOpt);
    args.push('-f');
    args.push(tmpPath.get_path());
    ProcUtil.runSync(args, cancellable, { logInitiation: true });

    tarballData = null; // Clear this out in the hope the GC eliminates it
    GSystem.file_unlink(tmpPath, cancellable);

    // Automatically strip the first element if there's exactly one directory
    let e = tmpCheckoutPath.enumerate_children('standard::*', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
					   cancellable);
    let info;
    let nFiles = 0;
    let lastFileType = null;
    let lastFile = null;
    let lastInfo = null;
    while ((info = e.next_file(cancellable)) != null) {
	if (info.get_name() == '.git')
	    continue;
	nFiles++;
	lastFile = e.get_child(info);
	lastInfo = info;
    }
    e.close(cancellable);
    if (nFiles == 1 && lastInfo.get_file_type() == Gio.FileType.DIRECTORY) {
	e = lastFile.enumerate_children('standard::*', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
					cancellable);
	while ((info = e.next_file(cancellable)) != null) {
	    let child = e.get_child(info);
	    if (!child.equal(lastFile))
		GSystem.file_rename(child, tmpCheckoutPath.get_child(info.get_name()), cancellable);
	}
	lastFile.delete(cancellable);
	e.close(cancellable);
    }
    
    let msg = 'Automatic import of ' + uri;
    ProcUtil.runSync(['git', 'add', '.'],
		     cancellable, { cwd: tmpCheckoutPath });
    ProcUtil.runSync(['git', 'commit', '-a', '--author=Automatic Tarball Importer <ostree-list@gnome.org>', '-m', msg ],
		     cancellable, { cwd: tmpCheckoutPath });
    ProcUtil.runSync(['git', 'tag', '-m', msg, '-a', importTag ],
		     cancellable, { cwd: tmpCheckoutPath });
    ProcUtil.runSync(['git', 'push', '--tags', 'origin', "master:master" ],
		     cancellable, { cwd: tmpCheckoutPath });
    
    GSystem.shutil_rm_rf(tmpCheckoutPath, cancellable);

    return mirror;
}

function uncacheRepository(mirrordir, keytype, uri, branch, cancellable) {
    let lastFetchPath = getLastfetchPath(mirrordir, keytype, uri, branch);
    GSystem.shutil_rm_rf(lastFetchPath, cancellable);
}

function fetch(mirrordir, component, cancellable, params) {
    params = Params.parse(params, {keepGoing: false, timeoutSec: 0});
    ensureVcsMirror(mirrordir, component, cancellable,
		      { fetch:true,
			fetchKeepGoing: params.keepGoing,
			timeoutSec: params.timeoutSec });
}

function describeVersion(dirpath, branch) {
    let args = ['git', 'describe', '--long', '--abbrev=42', '--always'];
    if (branch) {
        args.push(branch);
    }
    return ProcUtil.runSyncGetOutputUTF8(args, null, {cwd:dirpath}).replace(/[ \n]/g, '');
}
