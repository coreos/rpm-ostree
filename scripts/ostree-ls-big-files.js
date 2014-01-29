#!/usr/bin/env gjs

// Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

const Gio = imports.gi.Gio;
const OSTree = imports.gi.OSTree;

let repoPath = ARGV[0];
let ref = ARGV[1];

let cancellable = null;

let repo = OSTree.Repo.new(Gio.File.new_for_path(repoPath));
repo.open(cancellable);

let [,root] = repo.read_commit(ref, cancellable);

let sizes = [];

function tallySizes(dir, sizes, cancellable) {
    let e = dir.enumerate_children("standard::name,standard::type,standard::size",
				   Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
				   cancellable);
    let info;
    while ((info = e.next_file (cancellable)) != null) {
	let child = e.get_child(info);
	let ftype = info.get_file_type();
	if (ftype == Gio.FileType.DIRECTORY) {
	    tallySizes(child, sizes, cancellable);
	} else if (ftype == Gio.FileType.REGULAR) {
	    sizes.push([child, info.get_size()]);
	}
    }
    e.close(cancellable);
}

tallySizes(root, sizes, cancellable);

sizes.sort(function(a,b) {
    let sizeA = a[1];
    let sizeB = b[1];

    if (sizeA > sizeB)
	return -1;
    else if (sizeA < sizeB)
	return 1;
    else
	return 0;
});

for (let i = 0; i < 100 && i < sizes.length; i++) {
    let [path,size] = sizes[i];
    print("" + size + " " + path.get_path());
}



