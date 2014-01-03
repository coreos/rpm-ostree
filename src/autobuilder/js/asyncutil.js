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

const AsyncSet = new Lang.Class({
    Name: 'AsyncSet',

    _init: function(callback, cancellable) {
	this._callback = callback;
	this._cancellable = cancellable;
	this._results = {};
	this._err = null;
	this._children = [];
    },

    addGAsyncResult: function(name, callback) {
	this._children.push(callback);
	let wrapped = Lang.bind(this, function(object, result) {
	    let success = false;
	    try {
		this._results[name] = callback(object, result);
		success = true;
	    } catch (e) {
		if (this._cancellable)
		    this._cancellable.cancel();
		if (!this._err) {
		    this._err = e.toString();
		    this._checkCallback();
		    return;
		}
	    }

	    let i;
	    for (i = 0; i < this._children.length; i++) {
		let child = this._children[i];
		if (child === callback) {
		    break;
		}
	    }
	    if (i == this._children.length)
		throw new Error("Failed to find child task");
	    this._children.splice(i, 1);
	    this._checkCallback();
	});
	return wrapped;
    },
    
    _checkCallback: function() {
	if (this._err)
	    this._callback(null, this._err);
	else if (this._children.length == 0)
	    this._callback(this._results, null);
    }
});
