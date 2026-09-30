# Halo: Combat Evolved: dedicated servers and multiplayer in the browser

This repository is a fork of
[cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal),
the port of the Halo: Combat Evolved decompilation to Linux, Windows and
Android. The decompilation is of the Xbox build 2342 (`cachebeta.exe`,
SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

The original port starts from the decompilation of
[bnunu/halo-1](https://github.com/bnunu/halo-1), a fork of
[punpckhdq/halo](https://github.com/punpckhdq/halo).

## What this fork adds

The original project is a port for players: a game that you download,
point at a disc image and play, on a local network or over the internet.
This fork keeps that game, and adds what a service needs to **host**
Halo multiplayer:

| Part | What it is | Where |
| --- | --- | --- |
| **Dedicated server** | The Linux build, started headless: no window, GPU, audio or player of its own. It hosts system link games forever from a map and game type rotation, and a supervising process controls it through JSON over its pipes. | [port/linux/README.md, "Dedicated server"](port/linux/README.md#dedicated-server) |
| **WebAssembly client** | The game compiled with Emscripten (`ninja web`) for the browser, WebGL 2. By default it is **multiplayer only, with no menus**: the page that embeds it picks the player's name, colour, team and server, and the game loads straight into that server's lobby. | [docs/gateway.md](docs/gateway.md), [docs/wasm-spike.md](docs/wasm-spike.md) |
| **Gateway** (`halo-gateway`) | A small Go daemon that runs next to the dedicated servers. A browser has no UDP or TCP sockets, so the web client's sockets are framed onto one WebSocket to the gateway, which owns the real sockets. The game's datagrams move to a WebRTC data channel (unordered, no retransmits, as UDP) when one can open, with the WebSocket as the fallback. Access is by short-lived signed join tokens. | [port/gateway](port/gateway), [docs/gateway.md](docs/gateway.md) |

The fork also changes the game where multiplayer with a dedicated server
needs it: joining games in progress and after a game's end, team balance
(with a team preference from the web client), per-player colours reported
by the server, a client datagram queue that holds several ticks against
rubber-banding, and larger map cache slots for community multiplayer maps.
Most changes are behind `#ifdef` (`HALO_LINUX`, `HALO_WEB`,
`HALO_MULTIPLAYER_ONLY`), so the byte-matching build is unchanged.

## How an application uses it

The intended user is an application, for example a web site that lets
its users play Halo multiplayer in the browser. It needs no Halo code of
its own; it runs these pieces and glues them together:

```
 browser page ── halo.js / halo.wasm (the web client)
      │  WebSocket (+ WebRTC data channel for datagrams)
      ▼
 halo-gateway ── system link over loopback ──► dedicated servers (build/linux/halo)
      ▲                                              ▲
      │ control API (register servers)               │ JSON events / commands on pipes
      └────────────── the application's supervisor ──┘
```

1. **Run dedicated servers.** The application starts one `build/linux/halo`
   process per server with `HALO_DEDICATED=1`, a name, a rotation (for
   example `bloodgulch:slayer,sidewinder:ctf`) and its own loopback address
   (`HALO_NET_ADDRESS=127.0.1.N`, `HALO_NET_BROADCAST=127.64.0.1`). It reads
   the server's events (`server_started`, `player_joined`, `game_started`,
   `status`, ...) from stdout to show a live server list and
   player counts, and writes commands (`kick`, `change_map`, `set_rotation`, ...) to
   stdin. Refer to "Dedicated server control" in
   [port/linux/README.md](port/linux/README.md#dedicated-server-control).
2. **Run the gateway** beside the servers, with a shared secret. The
   supervisor registers each server with the gateway's loopback control API
   (`PUT /servers/{id}`) when it is up, and removes it when it stops. The
   site's reverse proxy forwards `wss://<site>/gateway` to it; for WebRTC,
   one UDP port (3478 by default) must reach it.
3. **Issue join tokens.** When a signed-in user clicks "play" on a server,
   the application signs a token (HMAC-SHA256 over a small JSON claim set:
   user, one-time session id, expiry, the servers the user may join, and
   the session's address) and answers token refreshes from a URL of its
   own. The token format is in [docs/gateway.md](docs/gateway.md#join-tokens).
4. **Serve the client.** The page serves `port/web/shell/play.html` (or its
   own page with `halo-loader.js`) and `build/web/halo.{js,wasm}` with the
   cross-origin isolation headers (`Cross-Origin-Opener-Policy: same-origin`,
   `Cross-Origin-Embedder-Policy: require-corp`), and the maps under
   `/maps/` with Range requests. It passes the settings as
   `HALO_WEB_*` values: the gateway URL, the token, the player's name,
   colour and team, and where to go when the player leaves. The client
   downloads `ui.map` first and each multiplayer map when a game needs it.
5. **Handle leaving.** When the player quits, is refused or loses the
   connection, the page gets a cancelable `halo:leave` event, or the client
   goes to `HALO_WEB_EXIT_URL`, so the site can take the player back to its
   server list.

Native players (Linux, Windows and Android builds of this repository) can
join the same dedicated servers over system link or the internet, next to
the browser players.

[docs/gateway.md](docs/gateway.md) has the details: addressing, the frame
format, WebRTC, the gateway's settings, an example reverse proxy
configuration, and the multiplayer-only build.

## Game data

This repository does not include the game data. Use your own Xbox disc
image (`.xiso` or `.iso`) of Halo: Combat Evolved. All versions of the game
operate. The maps of the European (PAL) version were made for a slower
console; the port changes them to play as the North American (NTSC) maps
do, so the two versions can play together.

- **Native game:** at the first start, the game asks for the disc image and
  extracts the `maps/` folder (next to the executable on Linux and Windows,
  in the app's data folder on Android; refer to
  [port/android/README.md](port/android/README.md)).
- **Dedicated server:** needs a `maps/` folder in its data root
  (`HALO_DATA_ROOT`) with `ui.map` and the maps of its rotation. It needs no
  display, GPU or audio device.
- **Web client:** the site serves `ui.map` and the multiplayer maps. The
  multiplayer-only build refuses campaign levels, so they are not needed.

## Build

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](port/include/xdk/README.md).

1. Install Python and [ninja](https://ninja-build.org/), and the tools for
   your platform (refer to the README for the platform).
2. In the root folder of the repository, enter `python configure.py`.
3. Enter `ninja` with a target:

| Target | Result | Instructions |
| --- | --- | --- |
| `ninja linux` | `build/linux/halo`: the game, and the dedicated server (32-bit x86, OpenGL 4.5, SDL3) | [port/linux/README.md](port/linux/README.md) |
| `ninja windows` (on Windows) | `build/windows/halo.exe` and `SDL3.dll` (32-bit x86, OpenGL 4.5, SDL3) | [port/windows/README.md](port/windows/README.md) |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` (arm64, OpenGL ES 3, SDL3) | [port/android/README.md](port/android/README.md) |
| `ninja web` | `build/web/halo.js` and `halo.wasm` (wasm32, WebGL 2, SDL3). Needs [Emscripten](https://emscripten.org/) (`source emsdk_env.sh` first). | [docs/wasm-spike.md](docs/wasm-spike.md), [docs/gateway.md](docs/gateway.md) |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use.

The gateway is a separate Go program:
`go build ./port/gateway` (a static binary; tests:
`cd port/gateway && go test -race ./...`).

To try the web client locally, run a dedicated server and the gateway, then
`python3 port/web/serve.py --maps /path/to/maps` serves the page with the
right headers. The web client uses threads (`SharedArrayBuffer`) and JSPI;
it runs in current Chrome and Firefox. Safari would need an ASYNCIFY build,
which is not made yet.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Linux and Windows builds operate on all x86-64 processors. Use this option for builds that you give to other persons or deploy to servers. |
| `--web-campaign` | `ninja web` with the campaign and the menus, instead of the multiplayer-only build. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile. Refer to "Optimization profiles". |

Without `--portable`, the Linux and Windows builds use all the instructions
of the processor that builds them (`-march=native`). Such a build does not
always start on a different computer.

`tools/ci_build.py` makes the same native builds as GitHub Actions, for
example `python tools/ci_build.py linux release`.

### Optimization profiles

The native builds use profiles of the game to optimize the code:
`pgo/halo_linux.profdata` for Linux and Android, and
`pgo/halo_windows.profdata` for Windows. The profiles need clang 22 or
later; with an older clang, the builds do not use them.

To record a new profile, delete the profile, enter
`python configure.py --pgo=train`, then `ninja linux` or `ninja windows`.
The build then plays the main menu and the first minute of each campaign
level (approximately 15 minutes). The game data must be in `assets/`.

## Builds from GitHub Actions

GitHub Actions builds the native ports (Linux, Windows and Android, debug
and release) for each pushed commit. Each build of `main` that passes on all
three platforms is published on this repository's
[Releases](../../releases) page, which keeps the last five. The web client
and the gateway are not built by the workflow; build them with `ninja web`
and `go build` as above.

Use the release build to play or to host. The debug build stops at the
first failed assertion and writes it to the log; use it to find and report
problems.

The self-updater of the native game (refer to "Updates" in
[port/linux/README.md](port/linux/README.md#updates)) still looks for
releases of the original repository. Turn it off (`update.auto = false`)
for builds of this fork; the dedicated server never updates itself.

## Multiplayer

- System link games on a local network or over the internet. The port
  raises the Xbox's limits to 128 players on up to 128 machines; a
  dedicated server's games take up to 16 players (`server.max_players`).
- Linux, Windows, Android and browser clients can play in the same game.
- The default netcode is new: each machine moves its own player at once,
  and the host makes the decisions for the game. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).
- Players can join a game in progress; a dedicated server keeps the game
  on when only one player is left.
- Without a dedicated server, one player's machine hosts, and an invite
  link lets a machine join it over the internet, as in the original
  project.

The Linux README also gives the controls, the settings and the multiplayer
functions of the native game. These are almost the same on all platforms.

## The byte-matching build

The decompilation also has a byte-matching build. That build compiles the
game with the compiler of the Xbox SDK and compares the result with
`cachebeta.exe`. This project does not generate that build, because the
Xbox SDK is not free to distribute. The sources of that build are not
changed. To use it again, set `SolutionConfig.matching` in
`tools/project_x86.py`. You must also have the Xbox SDK in `xbox/` and
`cachebeta.exe` in the root folder.
