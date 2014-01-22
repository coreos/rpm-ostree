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
const Task = imports.task;
const ProcUtil = imports.procutil;
const BuildUtil = imports.buildutil;
const LibQA = imports.libqa;
const JsonUtil = imports.jsonutil;
const JSUtil = imports.jsutil;
const GuestFish = imports.guestfish;

const IMAGE_RETAIN_COUNT = 2;

const TaskBuildDisks = new Lang.Class({
    Name: 'TaskBuildDisks',
    Extends: Task.Task,

    TaskDef: {
        TaskName: "builddisks",
        TaskAfter: ['build'],
    },

    _inheritPreviousDisk: true,

    _buildDiskForProduct: function(ref, cancellable) {
        let osname = this._products['osname'];
        let originRepoUrl = this._products['repo'];

        let [,revision] = this.ostreeRepo.resolve_rev(ref, false);
	      let refUnix = ref.replace(/\//g, '-');
	      let diskName = refUnix + '-' + this._buildName + '.qcow2';
        let diskPath = this._imageCacheDir.get_child(diskName);
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

        print("Successfully updated " + diskPath.get_path() + " to " + revision);

        this._postDiskCreation(refUnix, diskPath, cancellable);
    },

    execute: function(cancellable) {
        this._imageCacheDir = this.cachedir.get_child('images');
        GSystem.file_ensure_directory(this._imageCacheDir, true, cancellable);

        this._products = JsonUtil.loadJson(this.workdir.get_child('products.json'), cancellable);
        let productsBuilt = JsonUtil.loadJson(this.builddir.get_child('products-built.json'), cancellable);
        let productsBuiltSuccessful = productsBuilt['successful'];
        print("Preparing to update disks for " + JSON.stringify(productsBuiltSuccessful));
        for (let i = 0; i < productsBuiltSuccessful.length; i++) {
            this._buildDiskForProduct(productsBuiltSuccessful[i], cancellable);
        }
    },

    _postDiskCreation: function(squashedName, diskPath, cancellable) {
        // Nothing, this is used by zdisks
    }
});
