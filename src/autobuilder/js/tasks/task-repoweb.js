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
const OSTree = imports.gi.OSTree;
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

const TaskRepoweb = new Lang.Class({
    Name: 'TaskRepoweb',
    Extends: Task.Task,

    TaskDef: {
        TaskName: "repoweb",
        TaskAfter: ['build']
    },

    MAXDEPTH: 100,

    DefaultParameters: { },

    _readFileLinesToObject: function(path, cancellable) {
        let stream = path.read(cancellable);
        let datain = Gio.DataInputStream.new(stream);
        let result = {};
        while (true) {
	          let [line, len] = dataIn.read_line_utf8(cancellable);
	          if (line == null)
	              break;
	          result[line] = 1;
        }
        datain.close(null);
        return result;
    },

    _statsForDiff: function(diffTxt) {
        let nAdded = 0;
        let nRemoved = 0;
        let nModified = 0;
        
        let i = 0;
        while (true) {
            let next = diffTxt.indexOf('\n', i);
            if (diffTxt[i] == 'A')
                nAdded++;
            else if (diffTxt[i] == 'D')
                nRemoved++;
            else if (diffTxt[i] == 'M')
                nModified++;
            if (next < 0)
                break;
            else
                i = next+1;
        }
        return [nAdded, nRemoved, nModified];
    },

    _generateHistoryFor: function(refname, revision, curDepth, cancellable) {
        let commitDataOut = this._repoWebPath.get_child('commit-' + revision + '.json');
        if (curDepth > this.MAXDEPTH)
            return;
        if (commitDataOut.query_exists(null))
            return;

        let [,commitObject] = this._repo.load_variant(OSTree.ObjectType.COMMIT,
                                                      revision);
        let ts = OSTree.commit_get_timestamp(commitObject);
        let parent = OSTree.commit_get_parent(commitObject);

        if (parent) {
            let [,parentCommit] = this._repo.load_variant_if_exists(OSTree.ObjectType.COMMIT,
                                                                    parent);
            if (!parentCommit) {
                print("For ref " + refname + ": couldn't find parent " + parent);
                parent = null;
            }
        }
        
        let commitData = { 't': ts,
                           'parent': parent };

        if (parent) {
            let argv = ['ostree', '--repo='+this._repo.get_path().get_path(), 'diff', parent, revision];
            let procctx = new GSystem.SubprocessContext({ argv: argv });
            let diffTxtPath = Gio.File.new_for_path('diff.txt');
            procctx.set_stdout_file_path(diffTxtPath.get_path());
            let proc = GSystem.Subprocess.new(procctx, cancellable);
            proc.init(null);
            proc.wait_sync_check(cancellable);
            let diffTxt = GSystem.file_load_contents_utf8(diffTxtPath, cancellable);
            let [nAdded, nRemoved, nModified] = this._statsForDiff(diffTxt);
            GSystem.file_unlink(diffTxtPath, cancellable);
            commitData['diffstats'] = { 'added': nAdded,
                                        'removed': nRemoved,
                                        'modified': nModified
                                      };
            let diffData = {'difftxt': diffTxt};
            JsonUtil.writeJsonFileAtomic(this._repoWebPath.get_child('diff-' + revision + '.json'), diffData, cancellable);
        }

        print("Generated data for " + revision);
        JsonUtil.writeJsonFileAtomic(commitDataOut, commitData, cancellable);

        if (parent)
            this._generateHistoryFor(refname, parent, curDepth+1, cancellable);
    },

    execute: function(cancellable) {
	      this._repoPath = this.workdir.get_child('repo');
	      this._repoWebPath = this.workdir.get_child('repoweb-data');

        GSystem.file_ensure_directory(this._repoWebPath, true, cancellable);

        this._repo = new OSTree.Repo({ path: this._repoPath });
        this._repo.open(cancellable);
        
        let [,allRefs] = this._repo.list_refs(null, cancellable);
        let allRefsCopy = {};

        for (let refName in allRefs) {
            let revision = allRefs[refName];
            allRefsCopy[refName] = revision;
            print("Generating history for " + refName);
            this._generateHistoryFor(refName, revision, 0, cancellable);
        }
        JsonUtil.writeJsonFileAtomic(this._repoWebPath.get_child('refs.json'), { 'refs': allRefsCopy }, cancellable);
    },
});
