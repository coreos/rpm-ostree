// Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

const Gio = imports.gi.Gio;
const Lang = imports.lang;
const GSystem = imports.gi.GSystem;

const Task = imports.task;
const ProcUtil = imports.procutil;
const JsonUtil = imports.jsonutil;
const Snapshot = imports.snapshot;
const Vcs = imports.vcs;

const TaskResolve = new Lang.Class({
    Name: "TaskResolve",
    Extends: Task.Task,

    TaskDef: {
        TaskName: "resolve",
    },

    DefaultParameters: {fetchAll: false,
                        fetchSrcUrls: [],
			fetchComponents: [],
		        timeoutSec: 10},

    _writeSnapshotToBuild: function(cancellable) {
        let data = this._snapshot.data;
        let buf = JsonUtil.serializeJson(data);

        let oldSnapshot = this.builddir.get_child('last-build/snapshot.json');
        if (oldSnapshot.query_exists(cancellable)) {
            let oldBytes = GSystem.file_map_readonly(oldSnapshot, cancellable);
            let oldCsum = GLib.compute_checksum_for_bytes(GLib.ChecksumType.SHA256, oldBytes);
            let newCsum = GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256, buf, -1);
            if (oldCsum == newCsum)
                return false;
        }

        let snapshot = this.builddir.get_child('snapshot.json');
        JsonUtil.writeJsonFileAtomic(snapshot, data, cancellable);
        return true;
    },

    execute: function(cancellable) {
        let manifestPath = this.workdir.get_child('manifest.json');
        this._snapshot = Snapshot.fromFile(manifestPath, cancellable, { prepareResolve: true });

        let componentsToFetch = this.parameters.fetchComponents.slice();
        let srcUrls = this.parameters.fetchSrcUrls;
        for (let i = 0; i < srcUrls; i++) {
            let matches = snapshot.getMatchingSrc(srcUrls[i]);
            componentsToFetch.push.apply(matches);
        }

        let gitMirrorArgs = ['ostbuild', 'git-mirror', '--timeout-sec=' + this.parameters.timeoutSec,
			     '--workdir=' + this.workdir.get_path(),
			     '--manifest=' + manifestPath.get_path()];
        if (this.parameters.fetchAll || componentsToFetch.length > 0) {
            gitMirrorArgs.push('--fetch');
            gitMirrorArgs.push('-k');
	    gitMirrorArgs.push.apply(gitMirrorArgs, componentsToFetch);
	}
	ProcUtil.runSync(gitMirrorArgs, cancellable, { logInitiation: true });
	
	let componentNames = this._snapshot.getAllComponentNames();
	for (let i = 0; i < componentNames.length; i++) {
	    let component = this._snapshot.getComponent(componentNames[i]);
	    let tagOrBranch = component['tag'] || component['branch'];
            let mirrordir = Vcs.ensureVcsMirror(this.mirrordir, component, cancellable);
            let revision = Vcs.describeVersion(mirrordir, tagOrBranch);
            component['revision'] = revision;
	}

        let modified = this._writeSnapshotToBuild(cancellable);
        if (modified) {
            print("New source snapshot");
        } else {
            print("Source snapshot unchanged");
	}

        let modifiedPath = Gio.File.new_for_path('modified.json');
        JsonUtil.writeJsonFileAtomic(modifiedPath, { modified: modified }, cancellable);
    }
});
