// -*- indent-tabs-mode: nil; tab-width: 2; -*-
// Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

const BuildDisks = imports.tasks['task-builddisks'];

const TaskZDisks = new Lang.Class({
    Name: 'TaskZDisks',
    Extends: BuildDisks.TaskBuildDisks,

    TaskDef: {
        TaskName: "zdisks",
        TaskAfter: ['smoketest'],
        TaskScheduleMinSecs: 3*60*60,  // Only do this every 3 hours
    },

    _imageSubdir: 'images/z',
    _inheritPreviousDisk: false,
    _onlyTreeSuffixes: ['-runtime', '-devel-debug'],

    _postDiskCreation: function(squashedName, diskPath, cancellable) {
        let parent = diskPath.get_parent();
        let outPath = parent.get_child(squashedName + '-' + this._buildName + '.qcow2.gz');
        let outStream = outPath.create(Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
        let compressor = Gio.ZlibCompressor.new(Gio.ZlibCompressorFormat.GZIP, 7);
        let outConverter = Gio.ConverterOutputStream.new(outStream, compressor);
        let inStream = diskPath.read(cancellable);
        outConverter.splice(inStream, Gio.OutputStreamSpliceFlags.CLOSE_SOURCE | 
                            Gio.OutputStreamSpliceFlags.CLOSE_TARGET, cancellable);
        diskPath.delete(cancellable);
    }
});
