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

var AutoBuilderIface = '<node> \
<interface name="org.gnome.OSTreeBuild.AutoBuilder"> \
<method name="queueResolve"> \
    <arg type="as" direction="in" /> \
</method> \
<property name="Status" type="s" access="read" /> \
</interface> \
</node>';

const Autobuilder = new Lang.Class({
    Name: 'Autobuilder',
    Extends: Builtin.Builtin,

    DESCRIPTION: "Automatically fetch git repositories and build",

    _init: function() {
	this.parent();

        this.parser.addArgument('--autoupdate-self', { action: 'store' });

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

	if (args.autoupdate_self)
	    this._autoupdate_self = Gio.File.new_for_path(args.autoupdate_self);

	this._ownId = Gio.DBus.session.own_name('org.gnome.OSTreeBuild', Gio.BusNameOwnerFlags.NONE,
						function(name) {},
						function(name) { loop.quit(); });

	this._impl = Gio.DBusExportedObject.wrapJSObject(AutoBuilderIface, this);
	this._impl.export(Gio.DBus.session, '/org/gnome/OSTreeBuild/AutoBuilder');

	this._taskmaster = new Task.TaskMaster(this.workdir);
	this._taskmaster.connect('task-executing', Lang.bind(this, this._onTaskExecuting));
	this._taskmaster.connect('task-complete', Lang.bind(this, this._onTaskCompleted));

	/* Start an initial, non-fetching resolve */
	this._runResolve();
	/* Flag immediately that we need a full resolve */
	this._fullResolveNeeded = true;
	/* And set a timeout for 10 minutes for the next full resolve */
	this._resolveTimeout = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT,
							 60 * 10, Lang.bind(this, this._triggerFullResolve));

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
	    this._impl.emit_property_changed('Status', new GLib.Variant("s", this._status));
	}
    },

    get Status() {
	return this._status;
    },

    queueResolve: function(srcUrls) {
	print("Queuing force resolve for " + JSON.stringify(srcUrls));
        this._resolveSrcUrls.push.apply(this._resolveSrcUrls, srcUrls);
        this._runResolve();
    },

    _triggerFullResolve: function() {
	this._fullResolveNeeded = true;
	this._runResolve();
	return true;
    },

    _runResolve: function() {
	let cancellable = null;
	
	if (!(this._initialResolveNeeded ||
	      this._resolveSrcUrls.length > 0 ||
	      this._fullResolveNeeded))
	    return;

	if (this._taskmaster.isTaskQueued('resolve'))
	    return;

	if (this._autoupdate_self)
	    ProcUtil.runSync(['git', 'pull', '-r'], cancellable,
			     { cwd: this._autoupdate_self })

	let previousVersion = this._buildsDir.currentVersion(cancellable);
        let buildPath = this._buildsDir.allocateNewVersion(cancellable);
        let version = this._buildsDir.pathToVersion(buildPath);
        if (previousVersion) {
            let lastBuildPath = this._buildsDir.createPathForVersion(previousVersion);
            BuildUtil.atomicSymlinkSwap(buildPath.get_child('last-build'), lastBuildPath, null);
        }

	if (this._initialResolveNeeded) {
	    this._initialResolveNeeded = false;
	    this._taskmaster.pushTask(buildPath, 'resolve', { });
	} else if (this._fullResolveNeeded) {
	    this._fullResolveNeeded = false;
	    this._taskmaster.pushTask(buildPath, 'resolve', { fetchAll: true });
	} else {
	    this._taskmaster.pushTask(buildPath, 'resolve', { fetchSrcUrls: this._resolveSrcUrls });
	}
	this._resolveSrcUrls = [];

	this._updateStatus();
    }
});
