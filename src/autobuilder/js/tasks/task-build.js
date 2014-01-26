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

    DefaultParameters: {onlyTreesMatching: null},

    BuildState: { 'failed': 'failed',
		  'successful': 'successful',
		  'unchanged': 'unchanged' },

    _composeProduct: function(ref, productName, treeName, treeData, release, architecture, cancellable) {
	let repos = ['fedora-' + release];
	if (release != 'rawhide')
	    repos.push('fedora-' + release + '-updates');

	let addRepos = this._productData['add-repos'];
	if (addRepos)
	    repos.push.apply(repos, addRepos);

	let packages = treeData['packages'];
	let baseRequired = this._productData['base_required_packages'];
	packages.push.apply(packages, baseRequired);

	let [,origRevision] = this.ostreeRepo.resolve_rev(ref, true);
	if (origRevision == null)
	    print("Starting new build of " + ref);
	else
	    print("Starting build of " + ref + " previous: " + origRevision);

	let argv = ['rpm-ostree',
		    '--workdir=' + this.workdir.get_path()];
	argv.push.apply(argv, repos.map(function (a) { return '--enablerepo=' + a; }));
	argv.push.apply(argv, ['create', ref]);
	argv.push.apply(argv, packages);
	let productNameUnix = ref.replace(/\//g, '_');
	let buildOutputPath = Gio.File.new_for_path('log-' + productNameUnix + '.txt');
	
	let procContext = new GSystem.SubprocessContext({ argv: argv });
	GSystem.shutil_rm_rf(buildOutputPath, cancellable);
	procContext.set_stdout_file_path(buildOutputPath.get_path());
	procContext.set_stderr_disposition(GSystem.SubprocessStreamDisposition.STDERR_MERGE);
	let proc = new GSystem.Subprocess({ context: procContext });
	proc.init(cancellable);
	try {
	    proc.wait_sync_check(cancellable);
	} catch (e) {
	    print("Build of " + productName + " failed");
	    return [this.BuildState.failed, origRevision];
	}
	let [,newRevision] = this.ostreeRepo.resolve_rev(ref, false);
	if (origRevision == newRevision)
	    return [this.BuildState.unchanged, newRevision];
	return [this.BuildState.successful, newRevision];
    },

    execute: function(cancellable) {
	let productPath = this.workdir.get_child('products.json');
	let productData = JsonUtil.loadJson(productPath, cancellable);
	this._productData = productData;

	let releases = productData['releases'];
	let architectures = productData['architectures'];
	let products = productData['products'];
	let successful = [];
	let failed = [];
	let unchanged = [];
	let productTrees = {};
	for (let i = 0; i < releases.length; i++) {
	    for (let j = 0; j < architectures.length; j++) {
		for (let productName in products) {
		    for (let treeName in products[productName]) {
			let release = releases[i];
			let architecture = architectures[j];
			let ref = [this._productData['osname'], release, architecture, productName, treeName].join('/');
			if (this.parameters.onlyTreesMatching &&
			    ref.indexOf(this.parameters.onlyTreesMatching) == -1) {
			    log("Skipping " + ref + " which does not match " + this.parameters.onlyTreesMatching);
			    continue;
			}
			let [result, revision] =
			    this._composeProduct(ref, productName, treeName, products[productName][treeName],
						 release, architecture,
						 cancellable);
			productTrees[ref] = revision;
			switch (result) {
			    case this.BuildState.successful: {
				successful.push(ref);
			    }
			    break;
			    case this.BuildState.failed: {
				failed.push(ref);
			    }
			    break;
			    case this.BuildState.unchanged: {
				unchanged.push(ref);
			    }
			    break;
			    default:
			    throw new Error("Invalid result from composeProduct: " + result);
			}
		    }
		}
	    }
	}
	let productsBuilt = { successful: successful,
			      failed: failed,
			      unchanged: unchanged,
			      trees: productTrees };
	let productsBuiltPath = this.builddir.get_child('products-built.json');
	JsonUtil.writeJsonFileAtomic(productsBuiltPath, productsBuilt, cancellable);
	print("Successful: " + successful.join(' '));
	print("Failed: " + failed.join(' '));
	print("Unchanged: " + unchanged.join(' '));
    }
});
