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

const Params = imports.params;

function walkDirInternal(dir, matchParams, callback, cancellable, queryStr, depth, sortByName) {
    let denum = dir.enumerate_children(queryStr, Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
				       cancellable);
    let info;
    let subdirs = [];

    if (depth > 0) {
	depth -= 1;
    }

    let sortedFiles = [];
    while ((info = denum.next_file(cancellable)) != null) {
	let name = info.get_name();
	let child = dir.get_child(name);
	let ftype = info.get_file_type();
	
	if (depth != 0) {
	    if (ftype == Gio.FileType.DIRECTORY) {
		subdirs.push(child);
		continue;
	    }
	}

	if (matchParams.nameRegex && matchParams.nameRegex.exec(name) === null)
	    continue;
	if (matchParams.fileType !== null && matchParams.fileType != info.get_file_type())
	    continue;
	if (matchParams.contentType != null && matchParams.contentType != info.get_content_type())
	    continue;
	if (!sortByName)
	    callback(child, cancellable);
	else
	    sortedFiles.push(child);
    }
    if (sortByName) {
	sortedFiles.sort(function (a, b) {
	    return a.get_basename().localeCompare(b.get_basename());
	});
	for (let i = 0; i < sortedFiles.length; i++) {
	    callback(sortedFiles[i], cancellable);
	}
    }

    denum.close(cancellable);

    if (sortByName) {
	subdirs.sort(function (a, b) {
	    return a.get_basename().localeCompare(b.get_basename());
	});
    }
    for (let i = 0; i < subdirs.length; i++) {
	walkDirInternal(subdirs[i], matchParams, callback, cancellable, queryStr, depth);
    }
}

function walkDir(dir, matchParams, callback, cancellable) {
    matchParams = Params.parse(matchParams, { nameRegex: null,
					      fileType: null,
					      contentType: null,
					      depth: -1,
					      sortByName: false });
    let queryStr = 'standard::name,standard::type,unix::mode';
    if (matchParams.contentType)
	queryStr += ',standard::fast-content-type';
    let depth = matchParams.depth;
    walkDirInternal(dir, matchParams, callback, cancellable, queryStr, depth, matchParams.sortByName);
}
