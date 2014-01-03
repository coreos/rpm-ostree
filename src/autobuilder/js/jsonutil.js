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

/* jsonutil.js:
 * Read/write JSON to/from GFile paths, very inefficiently.
 */

function serializeJson(data) {
    return JSON.stringify(data, null, "  ");
}

function writeJsonToStream(stream, data, cancellable) {
    let buf = serializeJson(data);
    stream.write_bytes(new GLib.Bytes(buf), cancellable);
}

function writeJsonToStreamAsync(stream, data, cancellable, onComplete) {
    let buf = serializeJson(data);
    stream.write_bytes_async(new GLib.Bytes(buf), GLib.PRIORITY_DEFAULT,
			     cancellable, function(stream, result) {
				 let err = null;
				 try {
				     stream.write_bytes_finish(result);
				 } catch (e) {
				     err = e;
				 } 
				 onComplete(err != null, err);
			     });
}

function loadJsonFromStream(stream, cancellable) {
    let membuf = Gio.MemoryOutputStream.new_resizable();
    membuf.splice(stream, Gio.OutputStreamSpliceFlags.CLOSE_TARGET, cancellable);
    let bytes = membuf.steal_as_bytes();
    return JSON.parse(bytes.toArray().toString());
}

function loadJsonFromStreamAsync(stream, cancellable, onComplete) {
    let membuf = Gio.MemoryOutputStream.new_resizable();
    membuf.splice_async(stream, Gio.OutputStreamSpliceFlags.CLOSE_TARGET, GLib.PRIORITY_DEFAULT,
			cancellable, function(stream, result) {
			    let err = null;
			    let res = null;
			    try {
				stream.splice_finish(result);
				let bytes = membuf.steal_as_bytes();
				res = JSON.parse(bytes.toArray().toString());
			    } catch (e) {
				err = e;
			    }
			    onComplete(res, err);
			});
}

function writeJsonFileAtomic(path, data, cancellable) {
    let s = path.replace(null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
    writeJsonToStream(s, data, cancellable);
    s.close(cancellable);
}

function loadJson(path, cancellable) {
    let [success,contents,etag] = path.load_contents(cancellable);
    return JSON.parse(contents);
}

