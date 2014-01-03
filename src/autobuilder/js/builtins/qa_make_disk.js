// -*- indent-tabs-mode: nil; tab-width: 2; -*-
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

const Builtin = imports.builtin;
const ArgParse = imports.argparse;
const ProcUtil = imports.procutil;
const GuestFish = imports.guestfish;

const QaMakeDisk = new Lang.Class({
    Name: 'QaMakeDisk',
    Extends: Builtin.Builtin,

    DESCRIPTION: "Generate a disk image",

    _init: function() {
        this.parent();
        this.parser.addArgument('diskpath');
    },

    execute: function(args, loop, cancellable) {
        let path = Gio.File.new_for_path(args.diskpath);
        if (path.query_exists(null))
            throw new Error("" + path.get_path() + " exists");

        let tmppath = path.get_parent().get_child(path.get_basename() + '.tmp');
        GSystem.shutil_rm_rf(tmppath, cancellable);
        LibQA.createDisk(tmppath, cancellable);
        GSystem.file_rename(tmppath, path, cancellable);
        print("Created: " + path.get_path());
    }
});
