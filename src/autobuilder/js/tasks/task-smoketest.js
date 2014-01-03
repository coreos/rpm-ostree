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

const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;
const Lang = imports.lang;
const Format = imports.format;

const GSystem = imports.gi.GSystem;

const Builtin = imports.builtin;
const ArgParse = imports.argparse;
const ProcUtil = imports.procutil;
const Task = imports.task;
const TestBase = imports.tasks.testbase;
const LibQA = imports.libqa;
const JSUtil = imports.jsutil;
const JSONUtil = imports.jsonutil;

const TaskSmoketest = new Lang.Class({
    Name: 'TaskSmoketest',
    Extends: TestBase.TestBase,

    TaskDef: {
        TaskName: "smoketest",
        TaskAfter: ['builddisks'],
    },

    RequiredMessageIDs: ["0ce153587afa4095832d233c17a88001" // gnome-session startup ok
                        ],

    FailedMessageIDs: ["fc2e22bc6ee647b6b90729ab34a250b1", // coredump
                       "10dd2dc188b54a5e98970f56499d1f73" // gnome-session required component failed
                      ],

    CompletedTag: 'smoketested'
});
