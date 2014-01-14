// Copyright (C) 2011,2013,2014 Colin Walters <walters@verbum.org>
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
const ProcUtil = imports.procutil;
const JSUtil = imports.jsutil;
const StreamUtil = imports.streamutil;
const JsonUtil = imports.jsonutil;
const Snapshot = imports.snapshot;
const BuildUtil = imports.buildutil;
const Vcs = imports.vcs;

const TaskBuild = new Lang.Class({
    Name: "TaskBuild",
    Extends: Task.Task,

    TaskDef: {
        TaskName: "build",
    },

    DefaultParameters: {forceComponents: []},

    _composeProduct: function(productName, treeName, treeData, release, architecture) {
	let repos = ['fedora-' + release,
		     'walters-nss-altfiles'];
	if (release != 'rawhide')
	    repos.push('fedora-' + release + '-updates');

	let ref = [this._productData['osname'], release, architecture, name].join('/');
	let packages = treeData['packages'];

	let argv = ['rpm-ostree',
		    '--repo=' + this.workdir.get_child('repo').get_path()];
	argv += repos.map(function (a) { return '--enablerepo=' + a });
	argv += ['--os=fedora', '--os-version=' + release,
		 'create', ref];
	argv += packages;
	ProcUtil.runSync(argv, cancellable, { logInitiation: true,
					      cwd: this.workdir });
    },

    execute: function(cancellable) {
	let productPath = this.workdir.get_child('products.json');
	let productData = JsonUtil.loadJson(productPath, cancellable);
	this._productData = productData;

	let releases = productData['releases'];
	let architectures = productData['architectures'];
	let products = productData['products'];
	for (let i = 0; i < releases.length; i++) {
	    for (let j = 0; j < architectures.length; j++) {
		for (let productName in products) {
		    for (let treeName in products[productName]) {
			this._composeProduct(productName, treeName, products[productName][treeName], releases[i], architectures[j]);
		    }
		}
	    }
	}
    }
});
