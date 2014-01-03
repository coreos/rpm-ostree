// Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

const Builtin = imports.builtin;
const Snapshot = imports.snapshot;
const Vcs = imports.vcs;
const JsonUtil = imports.jsonutil;

const GitMirror = new Lang.Class({
    Name: 'GitMirror',
    Extends: Builtin.Builtin,

    DESCRIPTION: "Update internal git mirror for one or more components",
    
    _init: function() {
	this.parent();
        this.parser.addArgument('--workdir');
        this.parser.addArgument('--manifest');
        this.parser.addArgument('--snapshot');
        this.parser.addArgument('--timeout-sec', { help: "Cache fetch results for provided number of seconds" });
        this.parser.addArgument('--fetch', {action:'storeTrue',
				       help:"Also do a git fetch for components"});
        this.parser.addArgument(['-k', '--keep-going'], {action:'storeTrue',
						    help: "Don't exit on fetch failures"});
        this.parser.addArgument('components', {nargs:'*'});
    },

    execute: function(args, loop, cancellable) {
	this._initWorkdir(args.workdir, cancellable);

	if (!args.timeout_sec)
	    args.timeout_sec = 0;

        if (args.manifest != null) {
	    let manifestPath = Gio.File.new_for_path(args.manifest)
            this._snapshot = Snapshot.fromFile(manifestPath, cancellable, { prepareResolve: true });
        } else {
	    this._initSnapshot(null, args.snapshot, cancellable);
	}

	let componentNames;
        if (args.components.length == 0) {
	    componentNames = this._snapshot.getAllComponentNames();
        } else {
            componentNames = args.components;
	}

	componentNames.forEach(Lang.bind(this, function (name) {
	    let component = this._snapshot.getComponent(name);

            if (!args.fetch) {
                Vcs.ensureVcsMirror(this.mirrordir, component, cancellable);
	    } else {
		print("Running git fetch for " + name);
		Vcs.fetch(this.mirrordir, component, cancellable,
			  { keepGoing:args.keep_going,
			    timeoutSec: args.timeout_sec });
	    }
	}));
    }
});
