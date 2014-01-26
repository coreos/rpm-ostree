// -*- indent-tabs-mode: nil; tab-width: 2; -*-
// Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

const GSystem = imports.gi.GSystem;

const Builtin = imports.builtin;
const ArgParse = imports.argparse;
const Task = imports.task;
const ProcUtil = imports.procutil;
const BuildUtil = imports.buildutil;
const LibQA = imports.libqa;
const JsonUtil = imports.jsonutil;
const JSUtil = imports.jsutil;
const GuestFish = imports.guestfish;

const TaskZDisks = new Lang.Class({
    Name: 'TaskZDisks',
    Extends: Task.Task,

    TaskDef: {
        TaskName: "zdisks",
        TaskAfter: ['build'],
        TaskScheduleMinSecs: 3*60*60  // Only do this every 3 hours
    },

    _exportDiskForProduct: function(ref, revision, cancellable) {
	      let refUnix = ref.replace(/\//g, '-');
        let productDir = this._imageExportDir.get_child(refUnix);
        let diskPathTmp = this.workdir.get_child(refUnix + '.qcow2');
        LibQA.createDisk(diskPathTmp, cancellable);
        let mntdir = Gio.File.new_for_path('mnt');
        GSystem.file_ensure_directory(mntdir, true, cancellable);
        let gfmnt = new GuestFish.GuestMount(diskPathTmp, { partitionOpts: LibQA.DEFAULT_GF_PARTITION_OPTS,
                                                            readWrite: true });
        gfmnt.mount(mntdir, cancellable);
        try {
            let osname = this._products['osname'];
            let originRepoUrl = this._products['repo'];
            LibQA.pullDeploy(mntdir, this.repo, osname, ref, revision, originRepoUrl,
                             cancellable);
        } finally {
            gfmnt.umount(cancellable);
        }
        let imageExportName = diskPathTmp.get_basename() + '.xz';
        let diskPathXz = diskPathTmp.get_parent().get_child(imageExportName);
        ProcUtil.runSync(['xz', diskPathTmp.get_path() ], { cwd: diskPathTmp.get_parent(),
                                                            logInitiation: true });
        let imageExportTarget = productDir.get_child(imageExportName);
        GSystem.file_rename(diskPathXz, imageExportTarget, cancellable);
        print("Successfully created installed " + imageExportTarget.get_path());

        let e = null;
        try {
            e = productDir.enumerate_children('standard::name');
            while ((info = e.next_file(cancellable)) != null) {
                let name = info.get_name();
                if (!JSUtil.stringEndsWith(name, '.qcow2'))
                    continue;
                if (name == imageExportName)
                    continue;
                print("Deleting old " + name);
                GSystem.file_unlink(e.get_child(info), cancellable);
            }
        } finally {
            if (e) e.close(null);
        }
    },

    execute: function(cancellable) {
	      this._imageExportDir = this.workdir.get_child('images');
        this._products = JsonUtil.loadJson(this.workdir.get_child('products.json'), cancellable);
        this._productsBuilt = JsonUtil.loadJson(this.builddir.get_child('products-built.json'), cancellable);
        let productTrees = this._productsBuilt['trees'];
        for (let ref in productTrees) {
            this._exportDiskForProduct(ref, productTrees[ref], cancellable);
        }
    }
});
