#!/usr/bin/env python3
"""
DEDICATED_CONTROL_TEST.PY

Tests the dedicated server's control channel (port/linux/src/dedicated_control.c;
the protocol is in port/linux/README.md, "Dedicated server control") without
game data:

- the transport, built natively on its own with a small driver: JSON and
  typed commands, ids, arguments, bad lines, long lines, lines written in
  pieces, the input's end, stdout kept for events only, a supervisor that does
  not read (the server must not wait: lines are queued, then dropped and
  counted), and no busy work when there is no input;
- with --binary (build/linux/halo), the server's start: the "starting" event
  and the fatal errors for missing game data and for unreadable maps, with
  stdout carrying nothing but JSON lines.

Usage: tools/dedicated_control_test.py [--binary build/linux/halo] [--cc cc]
"""

import argparse
import json
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "port", "linux", "src")

DRIVER = r"""
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include "dedicated_control.h"

static const char *env(const char *name, const char *fallback)
{
	const char *value = getenv(name);
	return value ? value : fallback;
}

int config_boolean(const char *name)
{
	if (!strcmp(name, "server.control"))
		return atoi(env("DC_CONTROL", "1"));
	return 0;
}

long config_integer(const char *name)
{
	if (!strcmp(name, "server.control_output_fd"))
		return atol(env("DC_OUTPUT_FD", "1"));
	if (!strcmp(name, "server.control_input_fd"))
		return atol(env("DC_INPUT_FD", "0"));
	return 0;
}

const char *config_string(const char *name)
{
	(void)name;
	return "Test \"Server\"";
}

void platform_log(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	fputs("halo-linux: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double cpu(void)
{
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

static void echo(const struct control_command *command)
{
	static const char *const types[] = { "string", "number", "boolean", "null", "array" };
	int index;

	control_begin_reply("echo", command);
	control_field_boolean("json", command->json);
	control_field_string("id_type", command->id_type == _control_id_none ? "none" :
		command->id_type == _control_id_string ? "string" : "number");
	control_key("args");
	control_object_begin();
	for (index = 0; index < command->argument_count; index++)
	{
		control_key(command->arguments[index].key);
		control_object_begin();
		control_field_string("type", types[command->arguments[index].type]);
		control_field_string("value", command->pool + command->arguments[index].offset);
		control_object_end();
	}
	control_object_end();
	control_field_string("text", control_text(command));
	{
		long number;
		int flag;
		if (control_argument_integer(command, "n", &number))
			control_field_integer("n_integer", number);
		if (control_argument_boolean(command, "flag", &flag))
			control_field_boolean("flag_boolean", flag);
	}
	control_end();
	if (!strcmp(command->name, "fail"))
		control_error(command, "failed", "asked to fail: %d", 42);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "echo";

	if (!dedicated_control_initialize())
	{
		fprintf(stderr, "channel off\n");
		return 3;
	}
	/* stdout's other writers go to the log, not the events */
	printf("stray stdout text\n");
	fflush(stdout);

	if (!strcmp(mode, "echo"))
	{
		double deadline = now() + 20;
		while (now() < deadline)
		{
			struct control_command command;
			dedicated_control_poll();
			while (dedicated_control_next_command(&command))
			{
				echo(&command);
				if (!strcmp(command.name, "quit"))
				{
					dedicated_control_drain(2000);
					return 0;
				}
			}
			if (dedicated_control_input_ended())
			{
				control_begin("input_closed");
				control_end();
				dedicated_control_drain(2000);
				return 0;
			}
			usleep(1000);
		}
		return 4;
	}
	if (!strcmp(mode, "unicode"))
	{
		static const unsigned short name[] = { 'M', 0xe9, 0xd83d, 0xde00, '"', '\\', '\n', 0x01, 0xd800, 'x', 0 };
		control_begin("unicode");
		control_field_utf16("name", name, 32);
		control_field_utf16("short", name, 2);
		control_field_real("real", 1.23456, 2);
		control_field_real("nan", 0.0 / 0.0, 2);
		control_field_null("nothing");
		control_key("list");
		control_array_begin();
		control_integer(1);
		control_string("two");
		control_boolean(0);
		control_array_end();
		control_field_format("formatted", "%d-%s", 7, "x");
		control_end();
		dedicated_control_drain(2000);
		return 0;
	}
	if (!strcmp(mode, "flood"))
	{
		int count = atoi(env("DC_FLOOD", "100000"));
		int index;
		double start = now();
		for (index = 0; index < count; index++)
		{
			control_begin("flood");
			control_field_integer("index", index);
			control_field_string("padding", "0123456789012345678901234567890123456789012345678901234567890123456789");
			control_end();
		}
		{
			double elapsed = now() - start;
			fprintf(stderr, "flooded %.3f\n", elapsed);
			dedicated_control_drain(15000);
			control_begin("final");
			control_field_real("flood_seconds", elapsed, 3);
			control_end();
			dedicated_control_drain(15000);
		}
		return 0;
	}
	if (!strcmp(mode, "idle"))
	{
		/* 2000 ticks of 1 ms with nothing to read: the CPU used */
		double start = cpu();
		int ticks, commands = 0, ended = 0;
		for (ticks = 0; ticks < 2000; ticks++)
		{
			struct control_command command;
			dedicated_control_poll();
			while (dedicated_control_next_command(&command))
				commands++;
			ended |= dedicated_control_input_ended();
			usleep(1000);
		}
		control_begin("idle");
		control_field_real("cpu_seconds", cpu() - start, 4);
		control_field_integer("commands", commands);
		control_field_boolean("ended", ended);
		control_end();
		dedicated_control_drain(2000);
		return 0;
	}
	if (!strcmp(mode, "signal"))
	{
		static const char line[] = "{\"event\":\"shutdown\",\"reason\":\"signal\",\"signal\":15,\"immediate\":true}\n";
		control_begin("before");
		control_end();
		dedicated_control_signal_write(line, sizeof(line) - 1);
		return 0;
	}
	return 2;
}
"""

failures = []
passes = 0


def check(condition, what, detail=None):
    global passes
    if condition:
        passes += 1
        print(f"  ok   {what}")
    else:
        failures.append(what)
        print(f"  FAIL {what}" + (f": {detail}" if detail is not None else ""))


def parse_lines(text, what):
    """every non-blank line of an output must be a JSON object with an event"""
    events = []
    bad = []
    for line in text.splitlines():
        if not line.strip():
            continue
        try:
            value = json.loads(line)
            if not isinstance(value, dict) or "event" not in value:
                bad.append(line)
            else:
                events.append(value)
        except ValueError:
            bad.append(line)
    check(not bad, f"{what}: every output line is a JSON event", bad[:3])
    return events


def build(cc, directory):
    driver = os.path.join(directory, "driver.c")
    with open(driver, "w") as handle:
        handle.write(DRIVER)
    binary = os.path.join(directory, "driver")
    command = [cc, "-std=gnu11", "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-O1", "-g", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-I", SOURCE, driver, os.path.join(SOURCE, "dedicated_control.c"),
               os.path.join(SOURCE, "posix_files.c"), "-lm", "-o", binary]
    result = subprocess.run(command, capture_output=True, text=True)
    warnings = [line for line in result.stderr.splitlines()
                if "warning" in line and "dedicated_control" in line]
    check(result.returncode == 0, "the transport builds on its own", result.stderr[-2000:])
    check(not warnings, "the transport builds without warnings (-Wall -Wextra)", warnings[:5])
    if result.returncode:
        sys.exit(1)
    return binary


def run_driver(binary, mode, input_bytes=b"", env=None, timeout=30, **kwargs):
    environment = dict(os.environ)
    environment.update(env or {})
    return subprocess.run([binary, mode], input=input_bytes, capture_output=True, env=environment,
                          timeout=timeout, **kwargs)


def test_commands(binary):
    print("commands:")
    lines = [
        b'{"cmd":"status","id":7}',
        b'{"command":"kick","id":"abc-1","player":3,"flag":true,"note":null}',
        b'{"cmd":"set_rotation","id":1.5,"rotation":["bloodgulch:ctf","sidewinder"]}',
        b'kick 3',
        b'change_map bloodgulch:ctf now',
        b'next_map skip_postgame=true n=12',
        b'',
        b'# a comment',
        b'   ',
        b'{"cmd":"say","id":2,"message":"caf\\u00e9 \\ud83d\\ude00 \\"q\\""}',
        b'{bad json',
        b'{"cmd":"x","id":5,"nested":{"a":1}}',
        b'{"id":6}',
        b'{"cmd":"bad name!","id":8}',
        b'{"cmd":"fail","id":9}',
        b'{"cmd":"crlf","id":10}\r',
        b'[1,2]',
        b'{"cmd":"trailing","id":11} x',
        b'{"cmd":"obj_id","id":{"x":1}}',
    ]
    result = run_driver(binary, "echo", b"\n".join(lines) + b"\n")
    stdout = result.stdout.decode()
    stderr = result.stderr.decode()
    check(result.returncode == 0, "the driver ends at the input's end", result.returncode)
    events = parse_lines(stdout, "commands")
    check("stray stdout text" not in stdout and "stray stdout text" in stderr,
          "stdout's other writers go to stderr, not the events")
    check(events and events[0]["event"] == "starting" and events[0].get("protocol") == 1 and
          events[0].get("name") == 'Test "Server"' and events[0].get("commands") is True,
          "a starting event with protocol 1 comes first", events[:1])
    check([e["seq"] for e in events] == list(range(len(events))), "seq counts up from 0 without gaps")
    check(all(isinstance(e.get("time"), (int, float)) and abs(e["time"] - time.time()) < 60 for e in events),
          "every event has a unix time")
    echoes = [e for e in events if e["event"] == "echo"]
    errors = [e for e in events if e["event"] == "error"]
    by_command = {}
    for e in echoes:
        by_command.setdefault(e["command"], []).append(e)

    status = by_command.get("status", [{}])[0]
    check(status.get("id") == 7 and status.get("id_type") == "number" and status.get("json") is True,
          "a number id comes back as a number", status)
    kick_json = by_command.get("kick", [{}])[0]
    check(kick_json.get("id") == "abc-1" and kick_json["args"].get("player") == {"type": "number", "value": "3"}
          and kick_json["args"].get("flag") == {"type": "boolean", "value": "true"} and
          kick_json["args"].get("note") == {"type": "null", "value": ""} and kick_json.get("flag_boolean") is True,
          "a string id and typed arguments", kick_json)
    rotation = by_command.get("set_rotation", [{}])[0]
    check(rotation.get("id") == 1.5 and rotation["args"].get("rotation") ==
          {"type": "array", "value": "bloodgulch:ctf,sidewinder"}, "an array argument is joined with commas", rotation)
    kick_text = by_command.get("kick", [{}, {}])[1]
    check(kick_text.get("json") is False and kick_text.get("id") is None and kick_text.get("text") == "3",
          "a typed command: its words are the text, no id", kick_text)
    change = by_command.get("change_map", [{}])[0]
    check(change.get("text") == "bloodgulch:ctf now", "a typed command keeps its words", change)
    next_map = by_command.get("next_map", [{}])[0]
    check(next_map["args"].get("skip_postgame", {}).get("value") == "true" and next_map.get("n_integer") == 12
          and next_map.get("flag_boolean") is None, "typed key=value arguments", next_map)
    say = by_command.get("say", [{}])[0]
    check(say["args"].get("message", {}).get("value") == 'café 😀 "q"', "JSON escapes and surrogate pairs", say)
    check("crlf" in by_command, "a CR before the line's end is ignored")
    check(not any(e["command"] in ("", None) for e in echoes), "blank and # lines are ignored")

    codes = [(e.get("id"), e.get("code")) for e in errors]
    check((None, "bad_json") in codes, "a line that is not JSON: bad_json with a null id", codes)
    check((5, "bad_json") in codes, "a nested object: bad_json, with the id when it was read", codes)
    check((6, "bad_request") in codes, "a JSON line without cmd: bad_request", codes)
    check((8, "unknown_command") in codes or (8, "bad_json") in codes, "a bad command name is an error", codes)
    fail = [e for e in errors if e.get("id") == 9]
    check(fail and fail[0].get("command") == "fail" and fail[0].get("message") == "asked to fail: 42" and
          fail[0].get("fatal") is False, "control_error: code, message, id, command, fatal false", fail)
    check(sum(1 for i, c in codes if c == "bad_json") >= 4, "arrays, trailing text and object ids are bad_json",
          codes)
    check(all(e.get("fatal") is False for e in errors), "command errors are not fatal")
    check(events[-1]["event"] == "input_closed", "the input's end is reported once", events[-1])


def test_pieces(binary):
    print("input in pieces:")
    environment = dict(os.environ)
    process = subprocess.Popen([binary, "echo"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=environment)
    process.stdin.write(b'{"cmd":"sta')
    process.stdin.flush()
    time.sleep(0.3)
    process.stdin.write(b'tus","id":1}\n{"cmd":"two",')
    process.stdin.flush()
    time.sleep(0.3)
    process.stdin.write(b'"id":2}\n')
    process.stdin.flush()
    time.sleep(0.3)
    stdout, stderr = process.communicate(b'last_line', timeout=30)
    events = parse_lines(stdout.decode(), "pieces")
    names = [e.get("command") for e in events if e["event"] == "echo"]
    check(names[:2] == ["status", "two"], "a line written in pieces is one command", names)
    check("last_line" in names, "the last line without a newline still counts at the end", names)


def test_long_line(binary):
    print("long lines:")
    long_line = b'{"cmd":"long","id":1,"x":"' + b"a" * 70000 + b'"}'
    big_but_ok = b'{"cmd":"big","id":2,"x":"' + b"b" * 12000 + b'"}'
    too_big_pool = b'{"cmd":"pool","id":3,"x":"' + b"c" * 20000 + b'"}'
    many = b'{"cmd":"many","id":4' + b"".join(b',"k%d":%d' % (i, i) for i in range(20)) + b"}"
    result = run_driver(binary, "echo", b"\n".join([long_line, b'{"cmd":"after","id":5}', big_but_ok,
                                                     too_big_pool, many]) + b"\n")
    events = parse_lines(result.stdout.decode(), "long lines")
    codes = [(e.get("id"), e.get("code")) for e in events if e["event"] == "error"]
    names = [e.get("command") for e in events if e["event"] == "echo"]
    check((None, "line_too_long") in codes, "a line over 64 KiB: line_too_long", codes)
    check("after" in names, "the line after a long one is read", names)
    check("big" in names, "a 12 KB argument is read", names)
    check("pool" not in names and any(i == 3 for i, _ in codes), "arguments over the 16 KiB pool: an error", codes)
    check("many" not in names and any(i == 4 for i, _ in codes), "more than 16 arguments: an error", codes)
    oversized = [e for e in events if e.get("code") == "event_too_large"]
    check(oversized or "big" in names, "an event too large for a line is replaced by an error (or fits)")


def test_many_commands(binary):
    print("many commands at once:")
    lines = b"".join(b'{"cmd":"n","id":%d}\n' % i for i in range(500))
    result = run_driver(binary, "echo", lines)
    events = parse_lines(result.stdout.decode(), "many commands")
    ids = [e.get("id") for e in events if e["event"] == "echo"]
    check(ids == list(range(500)), "500 commands at once: all, in order (32 a tick)", ids[-3:])


def test_unicode(binary):
    print("JSON written:")
    result = run_driver(binary, "unicode")
    events = parse_lines(result.stdout.decode(), "unicode")
    event = next((e for e in events if e["event"] == "unicode"), {})
    check(event.get("name") == 'Mé😀"\\\n\x01\ufffdx', "UTF-16 text: escapes, surrogate pairs, lone surrogates",
          repr(event.get("name")))
    check(event.get("short") == "Mé", "UTF-16 text stops at its length", event.get("short"))
    check(event.get("real") == 1.23 and event.get("nan") is None and event.get("nothing") is None and
          event.get("list") == [1, "two", False] and event.get("formatted") == "7-x", "numbers, null, arrays, format",
          event)


def test_flood(binary):
    print("a supervisor that does not read:")
    read_end, write_end = os.pipe()
    environment = dict(os.environ, DC_FLOOD="100000")
    process = subprocess.Popen([binary, "flood"], stdin=subprocess.DEVNULL, stdout=write_end,
                               stderr=subprocess.PIPE, env=environment)
    os.close(write_end)
    # not read until the flood is over
    flooded = None
    for line in process.stderr:
        if line.startswith(b"flooded"):
            flooded = float(line.split()[1])
            break
    chunks = []
    while True:
        chunk = os.read(read_end, 65536)
        if not chunk:
            break
        chunks.append(chunk)
    os.close(read_end)
    process.wait(timeout=30)
    events = parse_lines(b"".join(chunks).decode(), "flood")
    check(flooded is not None and flooded < 2.0, "100000 events (11 MB) unread: the server never waits",
          flooded)
    dropped = [e for e in events if e["event"] == "dropped"]
    floods = [e for e in events if e["event"] == "flood"]
    check(dropped and dropped[0].get("count", 0) > 0, "the lines with no room are counted in a dropped event",
          dropped[:1])
    total_dropped = sum(e.get("count", 0) for e in dropped)
    check(dropped and len(floods) + total_dropped == 100000,
          "written + dropped account for every line", (len(floods), dropped[:3]))
    indexes = [e["index"] for e in floods]
    check(indexes == sorted(indexes) and indexes[:1] == [0], "the lines kept are whole and in order")
    check(events and events[-1]["event"] == "final", "lines after the drop are written again", events[-1:])
    seqs = [e["seq"] for e in events]
    check(seqs == sorted(seqs), "seq still counts up (with a gap where lines were dropped)",
          [(e["event"], e["seq"]) for i, e in enumerate(events) if i and e["seq"] < events[i - 1]["seq"]][:3])


def test_closed_output(binary):
    print("the supervisor goes away:")
    read_end, write_end = os.pipe()
    os.close(read_end)
    environment = dict(os.environ, DC_FLOOD="2000")
    result = subprocess.run([binary, "flood"], stdin=subprocess.DEVNULL, stdout=write_end, capture_output=False,
                            stderr=subprocess.PIPE, env=environment, timeout=30)
    os.close(write_end)
    stderr = result.stderr.decode()
    check(result.returncode == 0, "a closed output does not kill the server (no SIGPIPE death)",
          (result.returncode, stderr[-300:]))
    check("control output closed" in stderr, "and the log says events are no longer written")


def test_idle(binary):
    print("no input:")
    for label, stdin in (("an idle open pipe", subprocess.PIPE), ("/dev/null", subprocess.DEVNULL)):
        process = subprocess.Popen([binary, "idle"], stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, _ = process.communicate(timeout=30)
        events = parse_lines(stdout.decode(), f"idle ({label})")
        idle = next((e for e in events if e["event"] == "idle"), {})
        check(idle.get("commands") == 0 and idle.get("cpu_seconds", 1) < 0.15,
              f"{label}: 2000 ticks with nothing to read use little CPU", idle)
        if stdin == subprocess.DEVNULL:
            check(idle.get("ended") is True, "/dev/null: the input's end is seen", idle)
    # stdin closed altogether
    result = subprocess.run([binary, "idle"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                            preexec_fn=lambda: os.close(0))
    events = parse_lines(result.stdout.decode(), "idle (closed stdin)")
    check(events and events[0].get("commands") is False, "a closed stdin: starting says commands false",
          events[:1])
    check("control_input_fd 0 is not open" in result.stderr.decode(), "and the log says so")


def test_options(binary):
    print("settings:")
    result = run_driver(binary, "echo", b"", env={"DC_CONTROL": "0"})
    check(result.returncode == 3 and not result.stdout, "server.control = false: no channel, stdout untouched")
    read_end, write_end = os.pipe()
    result = subprocess.run([binary, "echo"], input=b'{"cmd":"x","id":1}\n', stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=dict(os.environ, DC_OUTPUT_FD=str(write_end)),
                            pass_fds=(write_end,), timeout=30)
    os.close(write_end)
    data = b""
    while True:
        chunk = os.read(read_end, 65536)
        if not chunk:
            break
        data += chunk
    os.close(read_end)
    events = parse_lines(data.decode(), "output fd")
    check(any(e["event"] == "echo" for e in events), "server.control_output_fd: events on another descriptor")
    check(b"stray stdout text" in result.stdout, "and stdout is left alone then")
    result = run_driver(binary, "echo", b"", env={"DC_OUTPUT_FD": "57"})
    check("control_output_fd 57 is not open" in result.stderr.decode(), "an output descriptor that is not open")


def test_signal_write(binary):
    print("the signal path:")
    result = run_driver(binary, "signal")
    events = parse_lines(result.stdout.decode(), "signal")
    check([e["event"] for e in events] == ["starting", "before", "shutdown"],
          "a signal's line arrives whole after what was queued", [e["event"] for e in events])


def test_binary(binary):
    print(f"the server ({binary}):")
    directory = tempfile.mkdtemp(prefix="dedicated-control-")
    try:
        environment = dict(os.environ, HOME=directory, HALO_DEDICATED="1", XDG_DATA_HOME=directory,
                           HALO_DATA_ROOT=os.path.join(directory, "nothing"))
        started = time.time()
        result = subprocess.run([binary], stdin=subprocess.DEVNULL, capture_output=True, env=environment,
                                cwd=directory, timeout=60)
        events = parse_lines(result.stdout.decode(), "no game data")
        check(result.returncode == 1, "no game data: exit status 1", result.returncode)
        check([e["event"] for e in events] == ["starting", "error"], "starting, then an error",
              [e["event"] for e in events])
        if len(events) == 2:
            check(events[1].get("code") == "no_game_data" and events[1].get("fatal") is True,
                  "the error is no_game_data, fatal", events[1])
        check(time.time() - started < 10, "and it stops promptly")

        os.makedirs(os.path.join(directory, "data", "maps"))
        environment["HALO_DATA_ROOT"] = os.path.join(directory, "data")
        process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=environment, cwd=directory)
        stdout, stderr = process.communicate(b'{"cmd":"status","id":1}\n', timeout=60)
        events = parse_lines(stdout.decode(), "empty maps folder")
        check(process.returncode == 1, "an empty maps folder: exit status 1", process.returncode)
        codes = [e.get("code") for e in events if e["event"] == "error"]
        check(codes == ["map_load_failed"] and events[-1].get("fatal") is True,
              "the error is map_load_failed, fatal", codes)
        check(b"halo-linux:" in stderr and b"halo-linux:" not in stdout, "the human log stays on stderr")
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="the built game (build/linux/halo) to test the server's start with")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    directory = tempfile.mkdtemp(prefix="dedicated-control-driver-")
    try:
        binary = build(arguments.cc, directory)
        test_commands(binary)
        test_pieces(binary)
        test_long_line(binary)
        test_many_commands(binary)
        test_unicode(binary)
        test_flood(binary)
        test_closed_output(binary)
        test_idle(binary)
        test_options(binary)
        test_signal_write(binary)
        if arguments.binary:
            test_binary(os.path.abspath(arguments.binary))
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    print(f"\n{passes} passed, {len(failures)} failed")
    for failure in failures:
        print(f"  failed: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
