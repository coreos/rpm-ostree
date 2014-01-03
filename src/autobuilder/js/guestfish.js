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
const Params = imports.params;
const ProcUtil = imports.procutil;

const LibGuestfs = new Lang.Class({
    Name: 'LibGuestfs',

    _init: function(diskpath, params) {
	this._params = Params.parse(params, {useLockFile: true,
					     partitionOpts: ['-i'],
					     readWrite: false});
	this._diskpath = diskpath;
	this._readWrite = params.readWrite
	this._partitionOpts = params.partitionOpts;
	if (params.useLockFile) {
	    let lockfilePath = diskpath.get_parent().get_child(diskpath.get_basename() + '.guestfish-lock');
	    this._lockfilePath = lockfilePath;
	} else {
	    this._lockfilePath = null;
	}
    },

    _lock: function() {
	if (this._lockfilePath) {
	    let stream = this._lockfilePath.create(Gio.FileCreateFlags.NONE, cancellable);
	    stream.close(cancellable);
	}
    },

    _unlock: function() {
	if (this._lockfilePath != null) {
	    GSystem.file_unlink(this._lockfilePath, cancellable);
	}
    },

    _appendOpts: function(argv) {
	argv.push.apply(argv, ['-a', this._diskpath.get_path()]);
	if (this._readWrite)
	    argv.push('--rw');
	else
	    argv.push('--ro');
	argv.push.apply(argv, this._partitionOpts);
    }
});

const GuestFish = new Lang.Class({
    Name: 'GuestFish',
    Extends: LibGuestfs,

    run: function(input, cancellable) {
	this._lock();
	try {
	    let guestfishArgv = ['guestfish'];
	    this._appendOpts(guestfishArgv);
	    return ProcUtil.runProcWithInputSyncGetLines(guestfishArgv, cancellable, input);
	} finally {
	    this._unlock();
	}
    }
});

const GuestMount = new Lang.Class({
    Name: 'GuestMount',
    Extends: LibGuestfs,

    mount: function(mntdir, cancellable) {
	this._lock();
	try {
	    this._mntdir = mntdir;
	    this._mountPidFile = mntdir.get_parent().get_child(mntdir.get_basename() + '.guestmount-pid');

	    if (this._mountPidFile.query_exists(null))
		throw new Error("guestfish pid file exists: " + this._mountPidFile.get_path());

	    let guestmountArgv = ['guestmount', '-o', 'allow_root',
				  '--pid-file', this._mountPidFile.get_path()];
	    this._appendOpts(guestmountArgv);
	    guestmountArgv.push(mntdir.get_path());
	    print('Mounting ' + mntdir.get_path() + ' : ' + guestmountArgv.join(' '));
            let context = new GSystem.SubprocessContext({ argv: guestmountArgv });
            let proc = new GSystem.Subprocess({ context: context });
	    proc.init(cancellable);

            // guestfish daemonizes, so this process will exit, and
	    // when it has, we know the mount is ready.  If there was
	    // a way to get notified instead of this indirect fashion,
	    // we'd do that.
	    proc.wait_sync_check(cancellable);

	    this._mounted = true;
	} catch (e) {
	    this._unlock();
	}
    },

    umount: function(cancellable) {
	if (!this._mounted)
	    return;

        let pidStr = GSystem.file_load_contents_utf8(this._mountPidFile, cancellable);
        if (pidStr.length == 0) {
	    this._mounted = false;
	    return;
	}

        for (let i = 0; i < 30; i++) {
            // See "man guestmount" for why retry loops here might be needed if this
            // script is running on a client machine with programs that watch for new mounts
            try {
                ProcUtil.runSync(['fusermount', '-u', this._mntdir.get_path()], cancellable,
                                 {logInitiation: true});
                break;
            } catch (e) {
                if (!(e.origError && e.origError.domain == GLib.spawn_exit_error_quark()))
                    throw e;
                else {
                    let proc = GSystem.Subprocess.new_simple_argv(['fuser', '-m', this._mntdir.get_path()],
                                                                  GSystem.SubprocessStreamDisposition.INHERIT,
                                                                  GSystem.SubprocessStreamDisposition.INHERIT,
                                                                  cancellable);
                    proc.init(cancellable);
                    proc.wait_sync(cancellable);
                    let creds = new Gio.Credentials();
                    proc = GSystem.Subprocess.new_simple_argv(['ls', '-al', '/proc/' + creds.get_unix_pid() + '/fd'],
                                                              GSystem.SubprocessStreamDisposition.INHERIT,
                                                              GSystem.SubprocessStreamDisposition.INHERIT,
                                                              cancellable);
                    proc.init(cancellable);
                    proc.wait_sync(cancellable);
                    
                    GLib.usleep(GLib.USEC_PER_SEC);
                }
            }
        }

        let pid = parseInt(pidStr);
	let guestfishExited = false;
	let guestfishTimeoutSecs = 5 * 60;
        for (let i = 0; i < guestfishTimeoutSecs; i++) {
            let killContext = new GSystem.SubprocessContext({argv: ['kill', '-0', ''+pid]});
            killContext.set_stderr_disposition(GSystem.SubprocessStreamDisposition.NULL);
            let killProc = new GSystem.Subprocess({context: killContext});
            killProc.init(null);
            let [waitSuccess, ecode] = killProc.wait_sync(null);
            let [killSuccess, statusStr] = ProcUtil.getExitStatusAndString(ecode);
            if (killSuccess) {
                print("Awaiting termination of guestfish, pid=" + pid + " timeout=" + (guestfishTimeoutSecs - i) + "s");
                GLib.usleep(GLib.USEC_PER_SEC);
            } else {
		guestfishExited = true;
                break;
            }
        }
	if (!guestfishExited)
	    throw new Error("guestfish failed to exit");
	this._mounted = false;
	this._unlock();
    },
});
