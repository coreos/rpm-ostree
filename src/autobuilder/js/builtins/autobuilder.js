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

	// Build every hour
	this._buildTimeout = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT,
						      60 * 60,
						      Lang.bind(this, this._triggerBuild));
	this._lastBuildPath = null;
	this._triggerBuild();

	this._updateStatus();

	let commandSocketAddress = Gio.UnixSocketAddress.new(Gio.File.new_for_path("cmd.socket").get_path());
	this._cmdSocketService = Gio.SocketService.new();
	this._cmdSocketService.add_address(commandSocketAddress,
					   Gio.SocketType.STREAM,
					   Gio.SocketProtocol.DEFAULT,
					   null);

	this._cmdSocketService.connect('incoming', this._onCmdSocketIncoming.bind(this));
	this._clientIdSerial = 0;
	this._clients = {};

	loop.run();
    },

    _onCmdSocketIncoming: function(svc, connection, source) {
	this._clientIdSerial++;
	let clientData = { 'serial': this._clientIdSerial,
			   'connection': connection,
			   'datainstream': Gio.DataInputStream.new(connection.get_input_stream()),
			   'outstandingWrite': false,
			   'bufs': [] };
	this._clients[this._clientIdSerial] = clientData;
	print("Connection from client " + clientData.serial);
	clientData.datainstream.read_line_async(GLib.PRIORITY_DEFAULT, null,
						this._onClientLineReady.bind(this, clientData));
    },

    _onClientSpliceComplete: function(clientData, stream, result) {
	stream.splice_finish(result);
	clientData.outstandingWrite = false;
	this._rescheduleClientWrite(clientData);
    },

    _rescheduleClientWrite: function(clientData) {
	if (clientData.bufs.length == 0 || 
	    clientData.outstandingWrite)
	    return;
	let buf = clientData.bufs.shift();
	let membuf = Gio.MemoryInputStream.new_from_bytes(GLib.Bytes.new(buf));
	clientData.outstandingWrite = true;
	clientData.connection.get_output_stream()
	    .splice_async(membuf, 0, GLib.PRIORITY_DEFAULT, null,
			  this._onClientSpliceComplete.bind(this, clientData));
    },

    _writeClient: function(clientData, buf) {
	clientData.bufs.push(buf + '\n');
	this._rescheduleClientWrite(clientData);
    },

    _parseParameters: function(paramStrings) {
	let params = {};
	for (let i = 0; i < paramStrings.length; i++) { 
	    let param = paramStrings[i];
	    let idx = param.indexOf('=');
	    if (idx == -1)
		throw new Error("Invalid key=value syntax");
	    let k = param.substr(0, idx);
	    let v = JSON.parse(param.substr(idx+1));
	    params[k] = v;
	}
	return params;
    },

    _onClientLineReady: function(clientData, stream, result) {
	let [line,len] = stream.read_line_finish_utf8(result);
	if (line == null) {
	    print("Disconnect from client " + clientData.serial);
	    delete this._clients[clientData.serial];
	    return;
	}
	let spcIdx = line.indexOf(' ');
	let cmd, rest = "";
	if (spcIdx == -1)
	    cmd = line;
	else {
	    cmd = line.substring(0, spcIdx);
	    rest = line.substring(spcIdx + 1);
	}
	print("[client " + clientData.serial + ']' + " Got cmd: " + cmd + " rest: " + rest);
	
	if (cmd == 'status') {
	    this._writeClient(clientData, this._status);
	} else if (cmd == 'build') {
	    this._triggerBuild();
	    this._writeClient(clientData, 'Build queued');
	} else if (cmd == 'pushtask') {
	    let nextSpcIdx = rest.indexOf(' ');
	    let taskName;
	    let args = {};
	    if (nextSpcIdx != -1) {
		taskName = rest.substring(0, nextSpcIdx);
		rest = rest.substring(nextSpcIdx + 1);
	    } else {
		taskName = rest;
		rest = null;
	    }
	    taskName = taskName.replace(/ /g, '');
	    let parsedArgs = true;
	    if (rest != null) {
		try {
		    args = this._parseParameters(rest.split(' '));
		} catch (e) {
		    this._writeClient(clientData, 'Invalid parameters: ' + e);
		    parsedArgs = false;
		}
	    }
	    if (parsedArgs) {
		try {
		    this._taskmaster.pushTask(this._lastBuildPath, taskName, args);
		    this._updateStatus();
		    this._writeClient(clientData, this._status);
		} catch (e) {
		    this._writeClient(clientData, 'Caught exception: ' + e);
		    throw e;
		}
	    }
	} else {
	    this._writeClient(clientData, 'Unknown command: ' + cmd);
	}
	print("Processed cmd: " + cmd);
	clientData.datainstream.read_line_async(GLib.PRIORITY_DEFAULT, null,
						this._onClientLineReady.bind(this, clientData));
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
	let runningTaskNames = [];
	let queuedTaskNames = [];
	for (let i = 0; i < taskstateList.length; i++) {
	    let taskstate = taskstateList[i];
	    if (taskstate.running) {
		runningTasks.push(taskstate);
		runningTaskNames.push(taskstate.task.name);
	    } else {
		queuedTaskNames.push(taskstate.task.name);
	    }
	}
	if (runningTasks.length == 0 && queuedTaskNames.length == 0) {
	    newStatus = "[idle]";
	} else {
	    newStatus = "running:";
	    for (let i = 0; i < runningTasks.length; i++) {
		newStatus += ' ' + runningTasks[i].task.name;
		let params = runningTasks[i].task.taskData.parameters;
		for (let n in params) {
		    newStatus += ' ' + n + '=' + params[n];
		}
	    }
	    if (queuedTaskNames.length > 0)
		newStatus += " queued:";
	    for (let i = 0; i < queuedTaskNames.length; i++) {
		newStatus += " " + queuedTaskNames[i];
	    }
	}
	if (newStatus != this._status) {
	    this._status = newStatus;
	    print(this._status);
	    let [success,loadAvg,etag] = Gio.File.new_for_path('/proc/loadavg').load_contents(null);
	    loadAvg = loadAvg.toString().replace(/\n$/, '').split(' ');
	    let statusPath = Gio.File.new_for_path('autobuilder-status.json');
	    JsonUtil.writeJsonFileAtomic(statusPath, {'running': runningTaskNames,
						      'queued': queuedTaskNames,
						      'systemLoad': loadAvg}, null);
	}
    },

    _triggerBuild: function() {
	let cancellable = null;
	
	if (this._taskmaster.isTaskQueued('build'))
	    return true;

	this._lastBuildPath = this._buildsDir.allocateNewVersion(cancellable);
	this._taskmaster.pushTask(this._lastBuildPath, 'build', { });

	this._updateStatus();

	return true;
    }
});
