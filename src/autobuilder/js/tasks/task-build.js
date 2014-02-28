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

    _composeProduct: function(ref, treefileData, cancellable) {
	let [,origRevision] = this.ostreeRepo.resolve_rev(ref, true);
	if (origRevision == null)
	    print("Starting new build of " + ref);
	else
	    print("Starting build of " + ref + " previous: " + origRevision);

	let argv = ['rpm-ostree',
		    '--workdir=' + this.workdir.get_path()];

	let treefilePath = Gio.File.new_for_path('treefile.json');
	JsonUtil.writeJsonFileAtomic(treefilePath, treefileData, cancellable);

	argv.push.apply(argv, ['create', treefilePath.get_path()]);
	let productNameUnix = ref.replace(/\//g, '_');
	let buildOutputPath = Gio.File.new_for_path('log-' + productNameUnix + '.txt');
	print("Running: " + argv.map(GLib.shell_quote).join(' '));
	let procContext = new GSystem.SubprocessContext({ argv: argv });
	GSystem.shutil_rm_rf(buildOutputPath, cancellable);
	procContext.set_stdout_file_path(buildOutputPath.get_path());
	procContext.set_stderr_disposition(GSystem.SubprocessStreamDisposition.STDERR_MERGE);
	let proc = new GSystem.Subprocess({ context: procContext });
	proc.init(cancellable);
	try {
	    proc.wait_sync_check(cancellable);
	} catch (e) {
	    print("Build of " + ref + " failed");
	    return this.BuildState.failed;
	}
	let [,newRevision] = this.ostreeRepo.resolve_rev(ref, true);
	treefileData.rev = newRevision;
			
	if (origRevision == newRevision)
	    return this.BuildState.unchanged;
	return this.BuildState.successful;
    },

    _inheritAttribute: function(obj, source, attrName, defaultValue) {
	let value = source[attrName];
	if (!value)
	    value = this._productData[attrName];
	if (!value)
	    value = defaultValue;
	obj[attrName] = value;
    },

    _inheritExtendAttributeList: function(obj, source, attrName) {
	let result = [];
	let value = this._productData[attrName];
	if (value)
	    result.push.apply(result, value);
	let subValue = source[attrName];
	if (subValue)
	    result.push.apply(result, subValue);
	obj[attrName] = result;
    },

    _generateTreefile: function(ref, release, architecture,
				subproductData) {
	let treefile = JSON.parse(JSON.stringify(this._productData));
	delete treefile.products;
	
	treefile.ref = ref;

	this._inheritExtendAttributeList(treefile, subproductData, 'packages');
	this._inheritExtendAttributeList(treefile, subproductData, 'postprocess');
	this._inheritExtendAttributeList(treefile, subproductData, 'units');

	treefile.release = release;
	delete treefile.releases;
	treefile.architecture = architecture;
	delete treefile.architectures;

	this._inheritAttribute(treefile, subproductData, "comment", "");

	this._inheritExtendAttributeList(treefile, subproductData, 'repos');
	this._inheritExtendAttributeList(treefile, subproductData, 'repos_data');

	let overrideRepo = this.workdir.get_child('overrides');
	if (overrideRepo.query_exists(null)) {
	    print("Using override repo: " + overrideRepo.get_path()); 
	    treefile.repos_data.push('[rpm-ostree-internal-overrides]\n' +
				     'name=Internal rpm-ostee overrides\n' +
				     'baseurl=file://' + overrideRepo.get_path() + '\n' +
				     'metadata_expire=1m\n' +
				     'enabled=1\ngpgcheck=0\n');
	}

	return treefile;
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
	let buildmasterName = 'buildmaster';
	for (let i = 0; i < releases.length; i++) {
	    for (let j = 0; j < architectures.length; j++) {
		for (let productName in products) {
		    for (let treeName in products[productName]) {
			let release = releases[i];
			let architecture = architectures[j];
			let ref = [this._productData['osname'], release, architecture, buildmasterName, productName, treeName].join('/');
			if (this.parameters.onlyTreesMatching &&
			    ref.indexOf(this.parameters.onlyTreesMatching) == -1) {
			    log("Skipping " + ref + " which does not match " + this.parameters.onlyTreesMatching);
			    continue;
			}
			let subproductData = products[productName][treeName];
			let treefileData = this._generateTreefile(ref, release, architecture, subproductData);
			let result = this._composeProduct(ref, treefileData, cancellable);
			switch (result) {
			    case this.BuildState.successful: {
				productTrees[ref] = treefileData;
				successful.push(ref);
			    }
			    break;
			    case this.BuildState.failed: {
				failed.push(ref);
			    }
			    break;
			    case this.BuildState.unchanged: {
				productTrees[ref] = treefileData;
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

        let modifiedPath = Gio.File.new_for_path('modified.json');
        JsonUtil.writeJsonFileAtomic(modifiedPath, { modified: successful.length > 0 }, cancellable);
    }
});
