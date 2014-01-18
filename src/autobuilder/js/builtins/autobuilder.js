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

const GSystem = imports.gi.GSystem;

const Builtin = imports.builtin;
const Task = imports.task;
const JsonUtil = imports.jsonutil;
const BuildUtil = imports.buildutil;
const ProcUtil = imports.procutil;
const VersionedDir = imports.versioneddir;

const Autobuilder = new Lang.Class({
    Name: 'Autobuilder',
    Extends: Builtin.Builtin,

    DESCRIPTION: "Trigger builds every 3 hours",

    _init: function() {
	this.parent();

	this._buildNeeded = true;
	this._initialResolveNeeded = true;
	this._fullResolveNeeded = true;
	this._resolveTimeout = 0;
	this._resolveSrcUrls = [];
    },

    execute: function(args, loop, cancellable) {
	this._initWorkdir(null, cancellable);

        this._buildsDir = new VersionedDir.VersionedDir(this.workdir.get_child('builds'));

        this._resultsDir = this.workdir.get_child('results');
        GSystem.file_ensure_directory(this._resultsDir, true, cancellable);

	this._taskmaster = new Task.TaskMaster(this.workdir);
	this._taskmaster.connect('task-executing', Lang.bind(this, this._onTaskExecuting));
	this._taskmaster.connect('task-complete', Lang.bind(this, this._onTaskCompleted));

	// Build every 3 hours
	this._buildTimeout = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT,
						      60 * 60 * 3,
						      Lang.bind(this, this._triggerBuild));
	this._triggerBuild();

	this._updateStatus();

	loop.run();
    },

    _onTaskExecuting: function(taskmaster, task) {
	print("Task " + task.name + " executing on " + task.buildName);
	this._updateStatus();
    },

    _onTaskCompleted: function(taskmaster, task, success, error) {
        let cancellable = null;

        if (task.name == 'resolve') {
	    if (!task.changed) {
		print("Resolve is unchanged");
		this._buildsDir.deleteCurrentVersion(null);
	    }
	    this._runResolve();
	}

        let resultsPath;

	if (success) {
	    print("Task " + task.name + " complete: " + task.buildName);
            resultsPath = this._resultsDir.get_child('successful');
	} else {
	    print("Task " + task.name + " failed: " + task.buildName);
            resultsPath = this._resultsDir.get_child('failed');
	}

        BuildUtil.atomicSymlinkSwap(resultsPath, task.buildPath, null);

	this._updateStatus();
    },

    _updateStatus: function() {
	let newStatus = "";
	let taskstateList = this._taskmaster.getTaskState();
	let runningTasks = [];
	let queuedTasks = [];
	for (let i = 0; i < taskstateList.length; i++) {
	    let taskstate = taskstateList[i];
	    let name = taskstate.task.name;
	    if (taskstate.running)
		runningTasks.push(name);
	    else
		queuedTasks.push(name);
	}
	if (runningTasks.length == 0 && queuedTasks.length == 0) {
	    newStatus = "[idle]";
	} else {
	    newStatus = "running: " + JSON.stringify(runningTasks);
	    if (queuedTasks.length)
		newStatus += " queued: " + JSON.stringify(queuedTasks);
	}
	if (newStatus != this._status) {
	    this._status = newStatus;
	    print(this._status);
	    let [success,loadAvg,etag] = Gio.File.new_for_path('/proc/loadavg').load_contents(null);
	    loadAvg = loadAvg.toString().replace(/\n$/, '').split(' ');
	    let statusPath = Gio.File.new_for_path('autobuilder-status.json');
	    JsonUtil.writeJsonFileAtomic(statusPath, {'running': runningTasks,
						      'queued': queuedTasks,
						      'systemLoad': loadAvg}, null);
	}
    },

    _triggerBuild: function() {
	let cancellable = null;
	
	if (this._taskmaster.isTaskQueued('build'))
	    return;

	let buildPath = this._buildsDir.allocateNewVersion(cancellable);
	this._taskmaster.pushTask(buildPath, 'build', { });

	this._updateStatus();
    }
});
