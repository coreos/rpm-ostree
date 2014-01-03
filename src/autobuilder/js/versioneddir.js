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
const Format = imports.format;

const GSystem = imports.gi.GSystem;
const BuildUtil = imports.buildutil;
const FileUtil = imports.fileutil;
const JsonUtil = imports.jsonutil;

function relpathToVersion(relpath) {
    let parts = relpath.split('/');
	return parts[0] + parts[1] + parts[2] + '.' + parts[3];
}

const VersionedDir = new Lang.Class({
    Name: 'VersionedDir',

    _YMD_SERIAL_VERSION_RE: /^(\d+)(\d\d)(\d\d)\.(\d+)$/,
    _YEAR_OR_SERIAL_VERSION_RE: /^(\d+)$/,
    _MONTH_OR_DAY_VERSION_RE: /^\d\d$/,

    _init: function(path) {
	this.path = path;
	this._cachedResults = null;
	GSystem.file_ensure_directory(this.path, true, null);
    },

    _createPathForParsedVersion: function(year, month, day, serial) {
	return this.path.resolve_relative_path(Format.vprintf('%s/%s/%s/%s',
							      [year, month, day, serial]));
    },

    createPathForVersion: function(ymdSerial) {
	let match = this._YMD_SERIAL_VERSION_RE.exec(ymdSerial);
	if (!match) throw new Error();
	return this.path.resolve_relative_path(match[1] + '/' + match[2] + '/' +
					       match[3] + '/' + match[4]);
    },

    _iterateChildrenMatching: function(dir, pattern, callback, cancellable) {
	let e = dir.enumerate_children('standard::*', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, cancellable);
	let info;
	while ((info = e.next_file(cancellable)) != null) {
	    let srcpath = e.get_child(info);
	    let name = info.get_name();
	    let match = pattern.exec(name);
	    if (!match)
		continue;
	    callback(srcpath, match, cancellable);
	}
	e.close(null);
    },

    _loadYear: function(yeardir, results, cancellable) {
	this._iterateChildrenMatching(yeardir, this._MONTH_OR_DAY_VERSION_RE,
				      Lang.bind(this, function(srcpath, match, cancellable) {
					  this._loadMonth(srcpath, results, cancellable);
				      }), cancellable);
    },

    _loadMonth: function(monthdir, results, cancellable) {
	this._iterateChildrenMatching(monthdir, this._MONTH_OR_DAY_VERSION_RE,
				      Lang.bind(this, function(srcpath, match, cancellable) {
					  this._loadDay(srcpath, results, cancellable);
				      }), cancellable);
    },

    _loadDay: function(daydir, results, cancellable) {
	this._iterateChildrenMatching(daydir, this._YEAR_OR_SERIAL_VERSION_RE,
				      Lang.bind(this, function(srcpath, match, cancellable) {
					  results.push(this.pathToVersion(srcpath));
				      }), cancellable);
    },

    _convertLegacyLayout: function(cancellable) {
	this._iterateChildrenMatching(this.path, this._YMD_SERIAL_VERSION_RE,
				      Lang.bind(this, function(srcpath, match, cancellable) {
					  let path = this._createPathForParsedVersion(match[1], match[2], match[3], match[4]);
					  print("convert " + srcpath.get_path() + " -> " + path.get_path());
					  GSystem.file_ensure_directory(path.get_parent(), true, cancellable);
					  GSystem.file_rename(srcpath, path, cancellable);
				      }), cancellable);
    },

    pathToVersion: function(path) {
	return relpathToVersion(this.path.get_relative_path(path));
    },

    loadVersions: function(cancellable) {
	if (this._cachedResults !== null)
	    return this._cachedResults;
	let results = [];
	this._convertLegacyLayout(cancellable);
	this._iterateChildrenMatching(this.path, this._YEAR_OR_SERIAL_VERSION_RE,
				      Lang.bind(this, function(srcpath, match, cancellable) {
					  this._loadYear(srcpath, results, cancellable);
				      }), cancellable);
	results.sort(BuildUtil.compareVersions);
	this._cachedResults = results;
	return results;
    },

    currentVersion: function(cancellable) {
	let versions = this.loadVersions(cancellable);
	if (versions.length > 0)
	    return versions[versions.length - 1];
	return null;
    },

    _makeDirUpdateIndex: function(path, cancellable) {
	let relpath = this.path.get_relative_path(path);
	if (relpath == null) {
	    GSystem.file_ensure_directory(path, true, cancellable);
	    return;
	}

	let parent = path.get_parent();
	this._makeDirUpdateIndex(parent, cancellable);
	
	let created = false;
	try {
	    path.make_directory(cancellable);
	    created = true;
	} catch (e) {
	    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS))
		throw e;
	}
	let indexJsonPath = parent.get_child('index.json');
	if (!created)
	    created = !indexJsonPath.query_exists(null);
	if (created) {
	    let childNames = [];
	    FileUtil.walkDir(parent, { depth: 1,
				       fileType: Gio.FileType.DIRECTORY },
			     Lang.bind(this, function(filePath, cancellable) {
				 childNames.push(filePath.get_basename());
			     }), cancellable);
	    JsonUtil.writeJsonFileAtomic(indexJsonPath,
					 { 'subdirs': childNames }, cancellable);
	}
    },

    allocateNewVersion: function(cancellable) {
        let currentTime = GLib.DateTime.new_now_utc();
        let currentYmd = Format.vprintf('%d%02d%02d', [currentTime.get_year(),
                                                       currentTime.get_month(),
                                                       currentTime.get_day_of_month()]);
	let versions = this.loadVersions(cancellable);
	let newVersion = null;
	if (versions.length > 0) {
	    let last = versions[versions.length-1];
	    let match = this._YMD_SERIAL_VERSION_RE.exec(last);
	    if (!match) throw new Error();
            let lastYmd = match[1] + match[2] + match[3];
            let lastSerial = match[4];
            if (lastYmd == currentYmd) {
                newVersion = currentYmd + '.' + (parseInt(lastSerial) + 1);
            }
	}
	if (newVersion === null)
	    newVersion = currentYmd + '.0';
	let path = this.createPathForVersion(newVersion);
	this._makeDirUpdateIndex(path, cancellable);
	this._cachedResults.push(newVersion);
	return path;
    },

    deleteCurrentVersion: function(cancellable) {
	let versions = this.loadVersions(cancellable);
	if (versions.length == 0)
	    throw new Error();
	let last = versions.pop();
	let path = this.createPathForVersion(last);
	GSystem.shutil_rm_rf(path, cancellable);
    }
});
