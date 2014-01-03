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

const Make = new Lang.Class({
    Name: 'Make',
    Extends: Builtin.Builtin,

    DESCRIPTION: "Execute tasks",

    _init: function() {
	this.parent();
	this.parser.addArgument(['-n', '--only'], { action: 'storeTrue',
						    help: "Don't process tasks after this" });
	this.parser.addArgument(['-x', '--skip'], { action: 'append',
						    help: "Don't process tasks after this" });
	this.parser.addArgument('taskname');
	this.parser.addArgument('parameters', { nargs: '*' });
    },

    execute: function(args, loop, cancellable) {
	this._initWorkdir(null, cancellable);
	this._loop = loop;
	this._failed = false;
	this._cancellable = cancellable;
	this._oneOnly = args.only;
	let taskmaster = new Task.TaskMaster(this.workdir,
					     { onEmpty: Lang.bind(this, this._onTasksComplete),
					       processAfter: !args.only,
					       skip: args.skip });
	this._taskmaster = taskmaster;
	taskmaster.connect('task-executing', Lang.bind(this, this._onTaskExecuting));
	taskmaster.connect('task-complete', Lang.bind(this, this._onTaskCompleted));
	let params = this._parseParameters(args.parameters);
        let buildPath = Gio.File.new_for_path('local');
        GSystem.file_ensure_directory(buildPath, false, cancellable);
	taskmaster.pushTask(buildPath, args.taskname, params);
	loop.run();
	if (!this._failed)
	    print("Success!")
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

    _onTaskExecuting: function(taskmaster, task) {
	print("Task " + task.name + " executing on " + task.buildName);
	let output = task.taskCwd.get_child('output.txt');
	if (this._oneOnly) {
	    let context = new GSystem.SubprocessContext({ argv: ['tail', '-f', output.get_path() ] });
	    this._tail = new GSystem.Subprocess({ context: context });
	    this._tail.init(null);
	}
    },

    _onTaskCompleted: function(taskmaster, task, success, error) {
	if (this._oneOnly)
	    this._tail.request_exit();
	if (success) {
	    print("Task " + task.name + " complete: " + task.buildName);
	} else {
	    this._failed = true;
	    print("Task " + task.name + " failed: " + task.buildName);
	}
    },

    _onTasksComplete: function() {
	this._loop.quit();
    }
});
