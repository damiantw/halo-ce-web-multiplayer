#!/usr/bin/env python3
"""Regression test: the join of web.join (port/linux/game/auto_join.c) and the
dedicated server's empty game (port/linux/game/dedicated_server.c).

Runs a dedicated server (build/linux/halo, which needs the game's maps) and
real game clients that join it by themselves (HALO_WEB_JOIN=first, as the
web build does; under xvfb-run, without audio), and checks:

- lobby join: two clients join a lobby that waits for both; each adds its
  player within a second or so of joining (it used to wait a fixed 3 s
  after the join), and the game starts with both;
- late join: a third client joins the game in progress and is in it;
- refusal: a game that ends while a client joins it (the server's
  end_game, timed to land on the join) does not end the client's visit: it
  joins again (the web build would have left the page, "error 2");
- empty game: once every client has gone, the game ends after
  server.empty_seconds and the lobby opens at once (no end sequence, no
  scores; the postgame event says 0 seconds), but a client joining as the
  countdown runs out keeps the game going and is in it;
- a real match keeps its scores: end_game with players in shows them for
  server.postgame_seconds;
- CTF teams: with bloodgulch:ctf, two clients end up on different teams.

    python3 tools/auto_join_test.py --binary build/linux/halo \\
        --data-root /path/to/data   # a folder with maps/bloodgulch.map

Without the maps (or without xvfb-run) it says so and exits 0 (skipped), as
CI has no game data.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SERVER_ADDRESS = "127.0.0.211"
CLIENT_ADDRESSES = ["127.0.0.%d" % n for n in range(212, 222)]


class Process:
    """A game process whose output lines are kept with the time they came."""

    def __init__(self, name, args, env, cwd, started):
        self.name = name
        self.lines = []
        self.events = []
        self.lock = threading.Lock()
        self.started = started
        # (its own process group: xvfb-run's game and X server go with it)
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, env=env, cwd=cwd, start_new_session=True)
        self.spawned = time.monotonic()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for raw in self.process.stdout:
            line = raw.decode(errors="replace").rstrip("\n")
            now = time.monotonic()
            with self.lock:
                if line.startswith("{"):
                    try:
                        self.events.append((now, json.loads(line)))
                        continue
                    except ValueError:
                        pass
                self.lines.append((now, line))

    def find(self, text, after=0.0):
        with self.lock:
            for when, line in self.lines:
                if when >= after and text in line:
                    return when
        return None

    def count(self, text):
        with self.lock:
            return sum(1 for _, line in self.lines if text in line)

    def event(self, predicate, after=0.0):
        with self.lock:
            for when, event in self.events:
                if when >= after and predicate(event):
                    return when, event
        return None

    def wait(self, check, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = check()
            if result:
                return result
            if self.process.poll() is not None:
                return check()
            time.sleep(0.05)
        return check()

    def command(self, text):
        try:
            self.process.stdin.write((text + "\n").encode())
            self.process.stdin.flush()
        except OSError:
            pass

    def stop(self):
        """Ends the process and its group (xvfb-run's game and X server) at
        once, as closing the browser's tab does: the server sees the
        connection go."""
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            pass
        try:
            # (a command left in the pipe would raise at exit)
            self.process.stdin.close()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default="build/linux/halo")
    parser.add_argument("--data-root", default=os.environ.get("HALO_TEST_DATA_ROOT", ""),
                        help="the data folder (with maps/); default $HALO_TEST_DATA_ROOT")
    parser.add_argument("--keep", action="store_true", help="keep the logs' folder")
    options = parser.parse_args()

    if not options.data_root or not os.path.isfile(os.path.join(options.data_root, "maps", "bloodgulch.map")):
        print("auto join test: skipped (no maps: give --data-root or HALO_TEST_DATA_ROOT)")
        return 0
    if not shutil.which("xvfb-run"):
        print("auto join test: skipped (no xvfb-run for the clients)")
        return 0
    options.binary = os.path.abspath(options.binary)
    if not os.path.isfile(options.binary):
        print("auto join test: no binary at %s" % options.binary)
        return 1

    started = time.monotonic()
    root = tempfile.mkdtemp(prefix="halo-auto-join-test-")
    processes = []
    failures = []

    def log(text):
        print("[%7.2f] %s" % (time.monotonic() - started, text), flush=True)

    def check(condition, text):
        log(("ok   " if condition else "FAIL ") + text)
        if not condition:
            failures.append(text)
        return condition

    base = dict(os.environ)
    base.update({
        "HALO_DATA_ROOT": os.path.abspath(options.data_root), "HALO_NO_AUDIO": "1", "HALO_FULLSCREEN": "false",
        "HALO_NO_VSYNC": "1", "HALO_NET_ONLINE": "0", "HALO_UPDATE_AUTO": "0",
        "HALO_NET_BROADCAST": ",".join([SERVER_ADDRESS] + CLIENT_ADDRESSES),
    })

    def server(name, rotation, minimum_players, lobby):
        folder = os.path.join(root, name)
        os.makedirs(folder)
        env = dict(base)
        env.update({
            "HOME": folder, "HALO_SAVE_ROOT": folder, "HALO_DEDICATED": "1", "HALO_NET_ADDRESS": SERVER_ADDRESS,
            "HALO_SERVER_NAME": name, "HALO_SERVER_ROTATION": rotation,
            "HALO_SERVER_LOBBY": "1" if lobby else "0", "HALO_SERVER_MINIMUM_PLAYERS": str(minimum_players),
            "HALO_SERVER_COUNTDOWN": "1", "HALO_SERVER_EMPTY": "8", "HALO_SERVER_POSTGAME": "6",
            "HALO_NETCODE": "distributed", "HALO_SERVER_CONTROL": "1", "HALO_SERVER_STATUS_INTERVAL": "0",
        })
        process = Process(name, [options.binary], env, folder, started)
        processes.append(process)
        return process

    client_count = [0]

    def client(name):
        address = CLIENT_ADDRESSES[client_count[0] % len(CLIENT_ADDRESSES)]
        client_count[0] += 1
        folder = os.path.join(root, name)
        os.makedirs(folder)
        env = dict(base)
        env.update({"HOME": folder, "XDG_DATA_HOME": folder, "HALO_SAVE_ROOT": folder, "HALO_NET_ADDRESS": address,
                    "HALO_WEB_JOIN": "first"})
        process = Process(name, ["xvfb-run", "-a", "-s", "-screen 0 800x600x24", options.binary], env, folder, started)
        processes.append(process)
        return process

    def lobby_open(host, after=0.0):
        return host.event(lambda e: e.get("event") == "lobby" and e.get("reason") == "opened", after)

    def status(host, tag):
        host.command(json.dumps({"cmd": "status", "id": tag}))
        found = host.wait(lambda: host.event(lambda e: e.get("event") == "status" and e.get("id") == tag), 10)
        return found[1] if found else {}

    def players(state):
        return (state.get("game") or {}).get("players") or (state.get("lobby") or {}).get("players") or []

    def add_delay(c):
        joining = c.find("auto join: joining")
        added = c.find("auto join: player added", joining or 0.0)
        return None if joining is None or added is None else added - joining

    try:
        # ---- slayer in a lobby that waits for two players
        host = server("slayer", "bloodgulch:slayer", 2, True)
        check(host.wait(lambda: lobby_open(host), 90) is not None, "the server opened its lobby")
        a, b = client("a"), client("b")
        for c in (a, b):
            check(c.wait(lambda c=c: c.find("auto join: in the game"), 120) is not None, "%s is in the game" % c.name)
        for c in (a, b):
            delay = add_delay(c)
            check(delay is not None and delay < 1.5,
                  "%s added its player %.2f s after joining (was a fixed 3 s)" % (c.name, delay if delay is not None else -1))
        check(host.wait(lambda: host.event(lambda e: e.get("event") == "game_started"), 60) is not None, "the game started")

        # ---- a third machine joins the game in progress
        late = client("late")
        check(late.wait(lambda: late.find("auto join: in the game"), 120) is not None, "late joins the game in progress")
        names = players(status(host, "three"))
        check(len(names) == 3, "the server's game has three players (%d)" % len(names))

        # ---- a real match keeps its scores; a machine joining as it ends
        # is let go (the game is closed) and joins the next game
        joiner = client("joiner")
        check(joiner.wait(lambda: joiner.find("auto join: joining"), 120) is not None, "joiner is joining")
        ended_at = time.monotonic()
        host.command(json.dumps({"cmd": "end_game", "id": "end"}))
        postgame = host.wait(lambda: host.event(lambda e: e.get("event") == "postgame", ended_at), 30)
        check(postgame is not None and postgame[1].get("postgame_seconds") == 6,
              "a game with players shows its scores (postgame_seconds %s)" % (postgame and postgame[1].get("postgame_seconds")))
        reopened = host.wait(lambda: lobby_open(host, ended_at), 60)
        check(postgame is not None and reopened is not None and reopened[0] - postgame[0] >= 5.0,
              "the lobby opened after the scores (%.1f s)" % ((reopened[0] - postgame[0]) if postgame and reopened else -1))
        for c in (a, b, late):
            check(c.find("web: leaving") is None and c.find("was lost") is None and c.process.poll() is None,
              "%s stayed through the end" % c.name)
        check(joiner.wait(lambda: joiner.find("auto join: in the game"), 120) is not None and joiner.process.poll() is None,
              "joiner is in a game (joins: %d; refusals taken as a retry: %d)" % (
                  joiner.count("auto join: joining"), joiner.count("joining again")))

        # ---- empty game: the game ends at once, straight to the lobby
        restarted = host.wait(lambda: host.event(lambda e: e.get("event") == "game_started", reopened[0] if reopened else 0), 90)
        check(restarted is not None, "the next game started with the players")
        time.sleep(3)
        names = players(status(host, "four"))
        check(len(names) == 4, "the next game has all four players, joiner included (%d)" % len(names))
        for c in (a, b, late, joiner):
            c.stop()
        empty_from = time.monotonic()
        ended = host.wait(lambda: host.event(lambda e: e.get("event") == "game_ended" and e.get("reason") == "empty",
                                             empty_from), 60)
        reopened = host.wait(lambda: lobby_open(host, empty_from), 30)
        empty_post = host.event(lambda e: e.get("event") == "postgame", empty_from)
        check(ended is not None, "the empty game ended")
        check(ended is not None and reopened is not None and reopened[0] - ended[0] < 2.0,
              "the lobby opened %.2f s after the empty game ended (was ~12 s)" % ((reopened[0] - ended[0]) if ended and reopened else -1))
        check(empty_post is not None and empty_post[1].get("postgame_seconds") == 0, "its postgame event says 0 seconds")

        # ---- a client joining as the empty countdown runs out keeps the game
        # (two players start a game, both leave; the racer joins just before
        # the countdown would end the empty game)
        pair = [client("solo"), client("duo")]
        for c in pair:
            check(c.wait(lambda c=c: c.find("auto join: in the game"), 120) is not None, "%s is in a new game" % c.name)
        boot = pair[0].find("auto join: joining") - pair[0].spawned
        game_from = time.monotonic()
        for c in pair:
            c.stop()
        left = host.wait(lambda: len([e for e in host.events if e[0] >= game_from and e[1].get("event") == "player_left"]) >= 2
                         and [e for e in host.events if e[0] >= game_from and e[1].get("event") == "player_left"][-1], 30)
        racer_start = (left[0] if left else time.monotonic()) + 8.0 - boot - 0.3
        time.sleep(max(0.0, racer_start - time.monotonic()))
        racer = client("racer")
        check(racer.wait(lambda: racer.find("auto join: in the game"), 120) is not None, "racer is in the game")
        early_end = host.event(lambda e: e.get("event") == "game_ended", game_from)
        check(early_end is None or not left or early_end[0] - left[0] > 8.5,
              "the empty game was not ended under a joining racer")
        joined = racer.find("auto join: joining")
        early_end = host.event(lambda e: e.get("event") == "game_ended", game_from)
        log("racer joined %.2f s after the last player left (the empty countdown: 8 s); %s" % (
            (joined - left[0]) if joined and left else -1,
            "the game ended at %.2f s" % (early_end[0] - left[0]) if early_end and left else "the game did not end"))
        check(racer.find("web: leaving") is None and racer.count("auto join: joining") >= 1, "racer never gave up")
        racer.stop()
        host.command(json.dumps({"cmd": "quit"}))
        host.wait(lambda: host.process.poll() is not None, 15)
        host.stop()
        time.sleep(1)

        # ---- CTF: the teams
        ctf = server("ctf", "bloodgulch:ctf", 2, True)
        check(ctf.wait(lambda: lobby_open(ctf), 90) is not None, "the CTF server opened its lobby")
        red, blue = client("red"), client("blue")
        for c in (red, blue):
            check(c.wait(lambda c=c: c.find("auto join: in the game"), 120) is not None, "%s is in the CTF game" % c.name)
        teams = sorted(p.get("team") for p in players(status(ctf, "teams")))
        check(teams == [0, 1], "the CTF players are on different teams (%s)" % teams)
        for c in (red, blue):
            delay = add_delay(c)
            check(delay is not None and delay < 1.5, "%s added its player %.2f s after joining" % (c.name, delay if delay is not None else -1))
    finally:
        for process in processes:
            if process.name in ("slayer", "ctf"):
                process.command(json.dumps({"cmd": "quit"}))
        for process in processes:
            process.stop()
        for process in processes:
            with open(os.path.join(root, process.name + ".log"), "w") as out:
                for when, line in process.lines:
                    out.write("[%7.2f] %s\n" % (when - started, line))
                for when, event in process.events:
                    out.write("[%7.2f] %s\n" % (when - started, json.dumps(event)))
        if failures or options.keep:
            log("logs in %s" % root)
        else:
            shutil.rmtree(root, ignore_errors=True)
    print("auto join test: %s" % ("ok" if not failures else "%d failed" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
