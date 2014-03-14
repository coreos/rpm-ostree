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
        TaskAfter: ['ensure-disk-caches']
    },

    _exportDiskForProduct: function(ref, revision, cancellable) {
	      let refUnix = ref.replace(/\//g, '-');
        let diskDir = this._imageExportDir.get_child(refUnix);
        GSystem.file_ensure_directory(diskDir, true, cancellable);
        let latestDisk = LibQA.getACachedDisk(this._imageCacheDir.get_child(refUnix), cancellable);

        if (!latestDisk) {
            throw new Error("No cached disk found for " + ref);
        }

        let formats = this._products['image_formats'];
        if (!formats) formats = [];

        let newDiskName = null;
        let newVdiName = null;

        if (formats.indexOf('qcow2') >= 0) {
        let newDiskPath = diskDir.get_child(revision + '.qcow2.xz');
        newDiskName = newDiskPath.get_basename();
        if (!newDiskPath.query_exists(null)) {
            let newDiskPathTmp = Gio.File.new_for_path(revision + '.qcow2.xz.tmp');

            print("Creating " + newDiskPath.get_path());

            let xzCtx = new GSystem.SubprocessContext({ argv: [ 'xz' ] })
            xzCtx.set_stdin_file_path(latestDisk.get_path());
            xzCtx.set_stdout_file_path(newDiskPathTmp.get_path());
            let xz = new GSystem.Subprocess({ context: xzCtx });
            xz.init(cancellable);
            xz.wait_sync_check(cancellable);

            print("Completed compression, renaming to " + newDiskPath.get_path());
            GSystem.file_rename(newDiskPathTmp, newDiskPath, cancellable);
        } else {
            print("Already have " + newDiskPath.get_path());
        }
        BuildUtil.atomicSymlinkSwap(newDiskPath.get_parent().get_child('latest-qcow2.xz'),
                                    newDiskPath, cancellable);
        }

        if (formats.indexOf('vdi') >= 0) {
        let vdiTmpPath = Gio.File.new_for_path(revision + '.vdi.tmp');
        let newVdiPath = diskDir.get_child(revision + '.vdi.bz2'); 
        newVdiName = newVdiPath.get_basename();
        if (!newVdiPath.query_exists(null)) {
            let newVdiPathTmp = Gio.File.new_for_path(revision + '.vdi.bz2.tmp');

            print("Creating " + vdiTmpPath.get_path());
            ProcUtil.runSync(['qemu-img', 'convert', '-O', 'vdi',
                              latestDisk.get_path(), vdiTmpPath.get_path()],
                             cancellable,
                             { logInitiation: true });

            print("Creating " + newVdiPathTmp.get_path());
            let xzCtx = new GSystem.SubprocessContext({ argv: [ 'bzip2' ] })
            xzCtx.set_stdin_file_path(vdiTmpPath.get_path());
            xzCtx.set_stdout_file_path(newVdiPathTmp.get_path());
            let xz = new GSystem.Subprocess({ context: xzCtx });
            xz.init(cancellable);
            xz.wait_sync_check(cancellable);

            print("Completed VDI compression, renaming to " + newVdiPathTmp.get_path());
            GSystem.file_rename(newVdiPathTmp, newVdiPath, cancellable);
            GSystem.file_unlink(vdiTmpPath, cancellable);
        } else {
            print("Already have " + newVdiPath.get_path());
        }
        BuildUtil.atomicSymlinkSwap(newVdiPath.get_parent().get_child('latest-vdi.bz2'),
                                    newVdiPath, cancellable);
        }

        let e = null;
        try {
            e = diskDir.enumerate_children('standard::name', 0, cancellable);
            let info;
            while ((info = e.next_file(cancellable)) != null) {
                let name = info.get_name();
                if (name == newDiskName || name == newVdiName)
                    continue;
                let child = e.get_child(info);
                if (JSUtil.stringEndswith(name, '.qcow2.xz') ||
                    JSUtil.stringEndswith(name, '.vdi.bz2')) {
                    print("Removing old " + child.get_path());
                    GSystem.file_unlink(child, cancellable);
                }
            }
        } finally {
            if (e) e.close(null);
        }
    },

    execute: function(cancellable) {
	      this._imageCacheDir = this.cachedir.get_child('images');
	      this._imageExportDir = this.workdir.get_child('images/auto');
        this._products = JsonUtil.loadJson(this.workdir.get_child('products.json'), cancellable);
        this._productsBuilt = JsonUtil.loadJson(this.builddir.get_child('products-built.json'), cancellable);
        let productTrees = this._productsBuilt['trees'];
        for (let ref in productTrees) {
            this._exportDiskForProduct(ref, productTrees[ref]['rev'], cancellable);
        }
    }
});
