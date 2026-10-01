"""Regression test: a silent or slow-loading machine stays connected.

The dedicated server must never drop a machine for being slow or silent: a
browser tab in the background loads slowly and sends nothing for as long as
it is hidden, and its player should still be in the game when it comes back.
Only the loss of its connection (or a kick) drops a machine. Upstream's join,
silence, late-joiner and playerless timeouts are not taken, the lockstep
host's 2 second stall timeout is gone, and a machine still loading when the
wait for the others runs out joins the game in progress once it has loaded
(network_server_manager.c) instead of being removed.

This runs a dedicated server (build/linux/halo, which needs the game's maps)
and three stand-in machines (tools/system_link_bots.py):

- "quick" loads at once and plays (sends its client updates);
- "slow" takes longer than the server's wait for the others to load
  (NETWORK_GAME_SERVER_MAXIMUM_WAIT_TIME_FOR_LEVEL_LOADING, 60 s), sending
  nothing while it loads;
- "silent" loads, then sends nothing at all for longer than any timeout
  upstream had (15 s in game) while its connection stays open.

It passes when, at the end, all three players are in the server's game (the
control channel's status), no machine was removed or timed out (the server's
events and log), and the slow machine took up the game once loaded.

    python3 tools/silent_player_test.py --binary build/linux/halo \\
        --data-root /path/to/data   # a folder with maps/bloodgulch.map

Without the maps it says so and exits 0 (skipped), as CI has no game data.
"""

import argparse
import json
import os
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import system_link_bots as bots  # noqa: E402

HOST_ADDRESS = "127.0.0.1"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default="build/linux/halo")
    parser.add_argument("--data-root", default=os.environ.get("HALO_TEST_DATA_ROOT", ""),
                        help="the data folder (with maps/); default $HALO_TEST_DATA_ROOT")
    parser.add_argument("--map", default="bloodgulch")
    parser.add_argument("--slow-load-seconds", type=float, default=75.0,
                        help="how long the slow machine takes to load (more than the server's 60 s wait)")
    parser.add_argument("--silent-seconds", type=float, default=40.0,
                        help="how long the silent machine sends nothing once in the game")
    parser.add_argument("--first-address", default="127.0.0.42")
    options = parser.parse_args()

    if not options.data_root or not os.path.isfile(os.path.join(options.data_root, "maps", options.map + ".map")):
        print("silent player test: skipped (no maps: give --data-root or HALO_TEST_DATA_ROOT)")
        return 0
    options.binary = os.path.abspath(options.binary)
    if not os.path.isfile(options.binary):
        print("silent player test: no binary at %s" % options.binary)
        return 1

    started = time.monotonic()

    def log(text):
        print("[%7.2f] %s" % (time.monotonic() - started, text), flush=True)

    save_root = tempfile.mkdtemp(prefix="halo-silent-test-")
    env = dict(os.environ)
    env.update({
        "HALO_DEDICATED": "1", "HALO_DATA_ROOT": os.path.abspath(options.data_root), "HALO_SAVE_ROOT": save_root,
        "HALO_SERVER_NAME": "silent-test", "HALO_SERVER_ROTATION": options.map + ":slayer",
        "HALO_SERVER_MINIMUM_PLAYERS": "3", "HALO_SERVER_COUNTDOWN": "1", "HALO_SERVER_EMPTY": "600",
        "HALO_NET_ADDRESS": HOST_ADDRESS, "HALO_NETCODE": "distributed", "HALO_UPDATE_AUTO": "0",
        "HALO_NET_ONLINE": "0", "HALO_SERVER_CONTROL": "1", "HALO_SERVER_STATUS_INTERVAL": "0",
    })
    server = subprocess.Popen([options.binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env=env, cwd=save_root)
    events = []
    log_lines = []
    lock = threading.Lock()

    def reader():
        for raw in server.stdout:
            line = raw.decode(errors="replace").rstrip("\n")
            with lock:
                if line.startswith("{"):
                    try:
                        events.append(json.loads(line))
                        continue
                    except ValueError:
                        pass
                log_lines.append(line)

    threading.Thread(target=reader, daemon=True).start()

    def command(text):
        server.stdin.write((text + "\n").encode())
        server.stdin.flush()

    def wait_event(predicate, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            with lock:
                for event in events:
                    if predicate(event):
                        return event
            if server.poll() is not None:
                return None
            time.sleep(0.1)
        return None

    failures = []

    def check(condition, text):
        log(("ok   " if condition else "FAIL ") + text)
        if not condition:
            failures.append(text)

    machines = []
    selector = selectors.DefaultSelector()
    try:
        check(wait_event(lambda e: e.get("event") == "lobby", 60) is not None, "the server opened its lobby")
        first = int.from_bytes(socket.inet_aton(options.first_address), "big")
        for index, (name, load) in enumerate((("quick", 1.0), ("slow", options.slow_load_seconds), ("silent", 1.0))):
            machine = bots.Machine(index + 1, socket.inet_ntoa((first + index).to_bytes(4, "big")), HOST_ADDRESS, log)
            machine.name = name
            machine.load_seconds = load
            machine.connect()
            selector.register(machine.tcp, selectors.EVENT_READ | selectors.EVENT_WRITE, machine)
            machines.append(machine)
        quick, slow, silent = machines
        silent_from = None
        game_seen = slow_in_game_at = None
        last_tick = 0
        deadline = time.monotonic() + options.slow_load_seconds + options.silent_seconds + 90
        while time.monotonic() < deadline:
            now = time.monotonic()
            for key, mask in selector.select(timeout=0.01):
                machine = key.data
                if machine.state == "connecting" and mask & selectors.EVENT_WRITE:
                    if machine.tcp.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR):
                        machine.close(selector)
                        continue
                    selector.modify(machine.tcp, selectors.EVENT_READ, machine)
                    machine.joined()
                if mask & selectors.EVENT_READ and machine.state != "closed":
                    if not machine.receive() or machine.state == "closed":
                        machine.close(selector)
            if now - last_tick >= 1.0 / 30:
                last_tick = now
                for machine in machines:
                    if machine.state == "closed":
                        continue
                    if machine is silent and silent.state == "ingame":
                        # in the game: from now on it reads, but sends nothing
                        if silent_from is None:
                            silent_from = now
                            log("silent: in the game; sending nothing for %.0f s" % options.silent_seconds)
                        continue
                    machine.tick(now)
            if game_seen is None and quick.state == "ingame":
                game_seen = now
                log("the game started (quick is in it); slow is %s" % slow.state)
            if slow_in_game_at is None and slow.state == "ingame":
                slow_in_game_at = now
                log("slow: loaded and in the game")
            if any(m.state in ("closed", "rejected") for m in machines):
                break
            if (slow_in_game_at is not None and silent_from is not None and
                    now - silent_from >= options.silent_seconds and now - slow_in_game_at >= 5):
                break

        for machine in machines:
            check(machine.state not in ("closed", "rejected"), "%s's connection is open (state %s)" % (machine.name, machine.state))
        check(game_seen is not None, "the game started without waiting for ever on the slow machine")
        check(slow_in_game_at is not None, "the slow machine (%.0f s to load) took up the game once loaded" % options.slow_load_seconds)
        check(silent_from is not None and time.monotonic() - silent_from >= options.silent_seconds - 1,
              "the silent machine sent nothing for %.0f s" % options.silent_seconds)
        command('{"cmd":"status","id":"final"}')
        status = wait_event(lambda e: e.get("event") == "status" and e.get("id") == "final", 10)
        players = sorted(p.get("name") for p in ((status or {}).get("game") or {}).get("players", []))
        check(players == ["quick", "silent", "slow"], "all three players are in the server's game: %s" % players)
        with lock:
            left = [e for e in events if e.get("event") == "player_left"]
            dropped = [l for l in log_lines if "timed out" in l or "forcibly removing" in l or
                       "timeout" in l.lower() and "remov" in l.lower()]
        check(not left, "no player_left event (%s)" % [(e.get("name"), e.get("reason")) for e in left])
        check(not dropped, "no timeout removal in the server's log %s" % dropped[:3])
    finally:
        for machine in machines:
            if machine.tcp:
                machine.tcp.close()
            machine.udp.close()
        try:
            command('{"cmd":"quit"}')
            server.wait(15)
        except Exception:
            server.kill()
    print("silent player test: %s" % ("ok" if not failures else "%d failed" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
