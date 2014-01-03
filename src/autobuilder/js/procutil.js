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

const Format = imports.format;
const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;

const GSystem = imports.gi.GSystem;
const Params = imports.params;
const StreamUtil = imports.streamutil;

function objectToEnvironment(o) {
    let r = [];
    for (let k in o)
	r.push(k + "=" + o[k]);
    return r;
}

function _newContext(argv, params) {
    let context = new GSystem.SubprocessContext({argv: argv});
    params = Params.parse(params, {cwd: null,
				   env: null,
				   stderr: null,
				   logInitiation: false });
    if (typeof(params.cwd) == 'string')
	context.set_cwd(params.cwd);
    else if (params.cwd)
	context.set_cwd(params.cwd.get_path());

    if (params.env)
	context.set_environment(params.env);

    if (params.stderr != null)
	context.set_stderr_disposition(params.stderr);
    return [context, params];
}

function _wait_sync_check_internal(proc, cancellable) {
    try {
	proc.wait_sync_check(cancellable);
    } catch (e) {
	if (e.domain == GLib.spawn_exit_error_quark() ||
	    e.matches(GLib.SpawnError, GLib.SpawnError.FAILED)) {
	    let err = new Error(Format.vprintf("Child process %s: %s", [JSON.stringify(proc.context.argv), e.message]));
	    err.origError = e;
	    throw err;
	} else {
	    throw e;
	}
    }
}

function runSync(argv, cancellable, params) {
    let [context, pparams] = _newContext(argv, params);
    let proc = new GSystem.Subprocess({context: context});
    proc.init(cancellable);
    if (pparams.logInitiation)
	print(Format.vprintf("Started child process %s: pid=%s", [proc.context.argv.map(GLib.shell_quote).join(' '), proc.get_pid()]));
    _wait_sync_check_internal(proc, cancellable);
}

function _runSyncGetOutputInternal(argv, cancellable, params, subParams) {
    subParams = Params.parse(subParams, { splitLines: false,
					  grep: null });
    let [context, pparams] = _newContext(argv, params);
    context.set_stdout_disposition(GSystem.SubprocessStreamDisposition.PIPE);
    context.set_stderr_disposition(GSystem.SubprocessStreamDisposition.INHERIT);
    let proc = new GSystem.Subprocess({context: context});
    proc.init(cancellable);
    if (pparams.logInitiation)
	print(Format.vprintf("Started child process %s: pid=%s", [JSON.stringify(proc.context.argv), proc.get_pid()]));
    let input = proc.get_stdout_pipe();
    let dataIn = Gio.DataInputStream.new(input);

    let result = null;
    if (subParams.grep) {
	let grep = subParams.grep;
	while (true) {
	    let [line, len] = dataIn.read_line_utf8(cancellable);
	    if (line == null)
		break;
	    result = grep.exec(line);
	    if (result != null) {
		break;
	    }
	}
    } else if (subParams.splitLines) {
	result = StreamUtil.dataInputStreamReadLines(dataIn, cancellable);
    } else {
	result = '';
	while (true) {
	    let [line, len] = dataIn.read_line_utf8(cancellable);
	    if (line == null)
		break;
	    result += (line + '\n');
	}
    }
    dataIn.close(cancellable);
    _wait_sync_check_internal(proc, cancellable);
    return result;
}

function runSyncGetOutputLines(args, cancellable, params) {
    return _runSyncGetOutputInternal(args, cancellable, params, { splitLines: true });
}

function runSyncGetOutputUTF8(args, cancellable, params) {
    return _runSyncGetOutputInternal(args, cancellable, params);
}

function runSyncGetOutputUTF8Stripped(args, cancellable, params) {
    return _runSyncGetOutputInternal(args, cancellable, params).replace(/[ \n]+$/, '');
}

function runSyncGetOutputUTF8StrippedOrNull(args, cancellable, params) {
    if (!params)
	params = {};
    try {
	params.stderr = GSystem.SubprocessStreamDisposition.NULL;
	return runSyncGetOutputUTF8Stripped(args, cancellable, params);
    } catch (e) {
	if (e.origError && e.origError.domain == GLib.spawn_exit_error_quark())
	    return null;
	throw e;
    }
}

function runSyncGetOutputGrep(args, pattern, cancellable, params) {
    return _runSyncGetOutputInternal(args, cancellable, params, { grep: pattern });
}

function getExitStatusAndString(ecode) {
    try {
	GLib.spawn_check_exit_status(ecode);
	return [true, null];
    } catch (e) {
	if (e.domain == GLib.spawn_exit_error_quark() ||
	    e.matches(GLib.SpawnError, GLib.SpawnError.FAILED))
	    return [false, e.message];
	else
	    throw e;
    }
}

function asyncWaitCheckFinish(process, result) {
    let [waitSuccess, ecode] = process.wait_finish(result);
    return getExitStatusAndString(ecode);
}

function runWithTempContextAndLoop(func) {
    let mainContext = new GLib.MainContext();
    let mainLoop = GLib.MainLoop.new(mainContext, true);
    try {
        mainContext.push_thread_default();
        return func(mainLoop);
    } finally {
        mainContext.pop_thread_default();
    }
}

function _runProcWithInputSyncGetLinesInternal(mainLoop, argv, cancellable, input) {
    let context = new GSystem.SubprocessContext({ argv: argv });
    context.set_stdout_disposition(GSystem.SubprocessStreamDisposition.PIPE);
    context.set_stdin_disposition(GSystem.SubprocessStreamDisposition.PIPE);
    let proc = new GSystem.Subprocess({context: context});
    proc.init(cancellable);
    let stdinPipe = proc.get_stdin_pipe();
    let memStream = Gio.MemoryInputStream.new_from_bytes(new GLib.Bytes(input));
    let asyncOps = 3;
    function asyncOpComplete() {
        asyncOps--;
        if (asyncOps == 0)
            mainLoop.quit();
    }
    function onSpliceComplete(stdinPipe, result) {
        try {
            let bytesWritten = stdinPipe.splice_finish(result);
        } finally {
            asyncOpComplete();
        }
    }
    let closeBoth = Gio.OutputStreamSpliceFlags.CLOSE_SOURCE | Gio.OutputStreamSpliceFlags.CLOSE_TARGET;
    stdinPipe.splice_async(memStream, closeBoth, GLib.PRIORITY_DEFAULT,
                           cancellable, onSpliceComplete);

    let procException = null;
    function onProcExited(proc, result) {
        try {
            let [success, statusText] = asyncWaitCheckFinish(proc, result);
            if (!success)
                procException = statusText;
        } finally {
            asyncOpComplete();
        }
    }
    proc.wait(cancellable, onProcExited);
    
    let stdoutPipe = proc.get_stdout_pipe();
    let stdoutData = Gio.DataInputStream.new(stdoutPipe);
    let lines = [];
    function onReadLine(datastream, result) {
        try {
            let [line, len] = stdoutData.read_line_finish_utf8(result);
            if (line == null)
                asyncOpComplete();
            else {
                lines.push(line);
                stdoutData.read_line_async(GLib.PRIORITY_DEFAULT, cancellable, onReadLine);
            }
        } catch (e) {
            asyncOpComplete();
            throw e;
        }
    }
    stdoutData.read_line_async(GLib.PRIORITY_DEFAULT, cancellable, onReadLine);

    mainLoop.run();

    return lines;
}

function runProcWithInputSyncGetLines(argv, cancellable, input) {
    return runWithTempContextAndLoop(function (loop) {
        return  _runProcWithInputSyncGetLinesInternal(loop, argv, cancellable, input);
    });
}
