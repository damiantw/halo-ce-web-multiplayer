# Linux

`ninja linux` compiles the game with clang for 32-bit x86 Linux. The result
is a native executable, `build/linux/halo`. The game shows its graphics with
OpenGL 4.5. It plays sound through SDL3. It accepts keyboard, mouse and
gamepad input.

The game is 32-bit code because its data (tags, cache files, saved games)
contains 32-bit pointers, as on the Xbox.

## Requirements

You do not need the Xbox SDK. The declarations that the game uses are in
`port/include/xdk`.

To build:

- Python and ninja.
- clang. The option `--linux-cc` of `configure.py` selects a different
  compiler.
- The 32-bit glibc development files: `lib32-glibc` on Arch Linux,
  `gcc-multilib` and `libc6-dev-i386` on Debian and Ubuntu.
- The 32-bit SDL3: `lib32-sdl3` on Arch Linux, `libsdl3-dev:i386` on Debian
  and Ubuntu.

To start the game:

- The 32-bit OpenGL libraries (`lib32-mesa`).
- The 32-bit PipeWire or PulseAudio client libraries (`lib32-pipewire` or
  `lib32-libpulse`).

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja linux`.

## Start the game

Enter `build/linux/halo`.

The game data is the folder that contains `maps/`, from an Xbox disc image
of any version of the game. The game looks for this folder in this
sequence:

1. `paths.data` in `config.toml`.
2. The current folder.
3. The folder of the executable.
4. `assets/` in the current folder, and `assets/` in the repository that
   contains the executable.

If the game finds no data, it asks for an Xbox disc image (`.xiso` or
`.iso`). This occurs at the first start:

- Select "No" to stop the game.
- Select "Yes" to open a file picker. Select the disc image. The game copies
  `maps/` next to the executable and shows the progress.

The game writes the copy to `maps.partial`. When the copy is complete, the
game changes the name to `maps`. If the copy stops before it is complete,
the game asks for the disc image again at the next start.

## Files and folders

| Xbox drive | Folder |
| --- | --- |
| `d:\` | The data root: the folder that contains `maps/`. |
| `z:\` | `z/` in the save root. This folder contains the cache (approximately 800 MB of map data) and the saved games. |
| `u:\` | `u/` in the save root. This folder contains the user data. |

The save root is `paths.saves` in `config.toml`. If that setting is empty,
the save root is `$XDG_DATA_HOME/halo-linux` (usually
`~/.local/share/halo-linux`).

The game makes the folders when it needs them. Names of files and folders
are not case-sensitive, as on the Xbox.

These files are in the data root:

| File | Contents |
| --- | --- |
| `debug.txt` | The log of the game. At start-up, the game shows the data root in the terminal. |
| `init.txt` | Console commands that the game does at start-up. For example, `map_name levels\a10\a10` starts the first campaign level. |

The settings are in `config.toml` next to the executable. Refer to
"Settings".

If the game stops because of a fatal signal, it writes the address and a
backtrace to the standard error. To find the function at the address, enter
`addr2line -e build/linux/halo <address>`.

## Controls

The keyboard and the mouse operate controller 1. The game adds the input of
the first gamepad to controller 1. The other gamepads operate controllers 2
to 4.

| Key | Controller | Function in the game |
| --- | --- | --- |
| W, A, S, D | left stick | move |
| mouse | (direct aim) | aim |
| left mouse button | right trigger | fire |
| right mouse button, G | left trigger | throw a grenade |
| space, enter | A | jump, accept |
| F, backspace, mouse button 4 | B | melee, back |
| E, R | X | action, reload |
| tab, mouse wheel | Y | change the weapon |
| Q | white | flashlight |
| X | black | change the grenade |
| left ctrl, C | left stick click | crouch |
| Z, middle mouse button | right stick click | zoom |
| arrow keys | D-pad | |
| escape | start | pause menu |
| F1 | back | |
| \` | | open the developer console |
| F12 | | release or capture the mouse (not in the web build: there Escape releases it and a click captures it) |
| F11 | | change between fullscreen and window |

One movement of the mouse wheel changes the weapon one time. A second
movement after a short pause changes it again.

In the web build, the button prompts in the HUD and in the menus show these
keys instead of the Xbox buttons: for example "Hold E to pick up",
"F = quit", "Hold F1 for score" and the pause menu's "F=CANCEL
Space=SELECT". Each button's first key in this table is used. The Xbox buttons
come back as soon as a gamepad is used, and the keys return when the keyboard
or mouse is used again.

In the menus, the mouse moves a pointer:

- The item below the pointer gets the focus.
- A left click selects the item. On a setting with values, a click on the
  left or right half changes the value. On a button in the key of a screen
  (for example "B = Back"), a click pushes that button.
- A right click goes back.
- The mouse wheel moves through the items.

The keyboard also operates the menus. When the game continues, the mouse
aims again. A mouse button that you hold from the menu does not fire until
you push it again.

## Settings

The settings are in `config.toml` next to the executable
(`build/linux/config.toml`). At the first start, the game writes the file
with the default values and a comment for each setting. To get the default
values again, delete the file.

Two exceptions:

- The environment variable `HALO_CONFIG` gives the path of the file for one
  start of the game.
- A dedicated server started with `HALO_DEDICATED` in the environment uses
  `config.toml` in the save root (`HALO_SAVE_ROOT`, else
  `$XDG_DATA_HOME/halo-linux` or `~/.local/share/halo-linux`), not the file
  next to the executable. Thus each server with its own save root has its
  own file, and the folder of the executable can be read-only (for example,
  a bundle that starts the game through its own loader). `paths.saves` in
  that file does not move the file.

The game reads the file one time, at start-up. If a key is not correct, or
a value has the wrong type, the game writes the line to the log and uses the
default value.

Each setting has an environment variable. The environment variable changes
the setting for one start of the game. It has priority over the file.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `display.fullscreen` | `true` | `HALO_FULLSCREEN` | `true`: fullscreen at the resolution of the display. The picture has 480 lines of the game and the width of the display. `false`: a window with the 640x480 picture of the Xbox. F11 changes between the two. |
| `display.window_scale` | `2` | `HALO_WINDOW_SCALE` | The size of the window, as a multiple of 640x480. You can change the size of the window. |
| `display.vsync` | `true` | `HALO_NO_VSYNC=1` sets `false` | `true`: each frame waits for the display. |
| `display.interpolation` | `true` | `HALO_INTERPOLATION` | `true`: one frame for each refresh of the display. `false`: 30 frames each second, as on the Xbox. Refer to "Frame rate". |
| `display.player_names` | `"all"` | `HALO_PLAYER_NAMES` | In multiplayer, whose names are drawn above their heads: `"all"`, `"allies"`, `"enemies"` or `"none"`. An ally's name is drawn above the triangle the game shows over teammates. An enemy's name shows only while the enemy is in sight and not camouflaged, so it never shows where an enemy hides, and only as far away as the weapon in hand turns its reticle red over an enemy (at least 20 world units, the motion sensor's reach, and at most 70). |
| `display.player_name_scale` | `1.0` | `HALO_PLAYER_NAME_SCALE` | How large the players' names are drawn: `1.0` is three quarters of the size of the HUD's text, from `0.25` to `4`. |
| `display.scoreboard_team_layout` | `"teams"` | `HALO_SCOREBOARD_TEAM_LAYOUT` | How the multiplayer scoreboard (hold BACK, or F1) lists a team game's players. `"teams"`: a column for each team, red on the left and blue on the right. `"score"`: all the players in order of score. With more players than fit, the mouse wheel, Page Up / Page Down and a gamepad's d-pad scroll the scoreboard. |
| `display.scoreboard_background` | `true` | `HALO_SCOREBOARD_BACKGROUND` | `true`: the multiplayer scoreboard (hold BACK, or F1) has a panel behind its text, for clearer text. |
| `display.scoreboard_background_color` | `"16, 16, 16, 150"` | `HALO_SCOREBOARD_BACKGROUND_COLOR` | The colour of the scoreboard's panel: `"red, green, blue, alpha"`, each from `0` to `255` (commas or spaces between: on the web, `env=` takes spaces). Alpha `0` is see-through, `255` is solid. |
| `audio.enabled` | `true` | `HALO_NO_AUDIO=1` sets `false` | `false`: no audio device. The sound continues without output. |
| `audio.volume` | `1.0` | `HALO_VOLUME` | The master volume. |
| `input.mouse_sensitivity` | `1.0` | `HALO_MOUSE_SENSITIVITY` | The multiplier for the mouse aim. |
| `input.invert_mouse` | `false` | `HALO_MOUSE_INVERT=1` sets `true` | `true`: the vertical mouse aim is inverted. |
| `game.language` | `""` | `HALO_LANGUAGE` | The language of the menus: `ja`, `de`, `fr`, `es` or `it`. Empty: English. |
| `paths.data` | `""` | `HALO_DATA_ROOT` | The data root. Refer to "Start the game". |
| `paths.saves` | `""` | `HALO_SAVE_ROOT` | The save root. Refer to "Files and folders". |
| `network.address` | `""` | `HALO_NET_ADDRESS` | The IPv4 address of this machine for system link. Refer to "Play on one computer". |
| `network.broadcast` | `""` | `HALO_NET_BROADCAST` | IPv4 addresses, with commas between them, that get the broadcasts of the game. Empty: 255.255.255.255. |
| `network.online` | `true` | `HALO_NET_ONLINE` | `true`: internet play. `false`: system link on the local network only. |
| `network.join_from_clipboard` | `true` | `HALO_NET_JOIN_FROM_CLIPBOARD` | `true`: when the game comes to the front, it joins the game of an invite link on the clipboard. |
| `network.tunnel_port` | `0` | `HALO_NET_TUNNEL_PORT` | The UDP port for internet play. `0`: the game selects a port. Refer to "Internet play". |
| `network.allow_upnp` | `true` | `HALO_NET_ALLOW_UPNP` | `true`: internet play can ask the router to forward its port (UPnP). `false`: the game does not ask. Refer to "Internet play". |
| `network.signalling_brokers` | three public brokers | `HALO_NET_BROKERS` | The public MQTT brokers (`host:port`, with commas between them) that let the machines of an invite find each other. |
| `network.stun_servers` | Google and Cloudflare | `HALO_NET_STUN` | The public STUN servers (`host:port`, with commas between them) that give the internet address of a machine. |
| `discord.application_id` | the application of the project | `HALO_DISCORD_APPLICATION` | The Discord application for invites. Empty: no Discord. |
| `update.auto` | `true` | `HALO_UPDATE_AUTO` | `true`: at start-up, the game looks for a new version. Refer to "Updates". `false`: the game does not look. |
| `server.dedicated` | `false` | `HALO_DEDICATED` | `true`: a headless dedicated server for system link. Refer to "Dedicated server". |
| `server.name`, `server.rotation`, `server.lobby`, `server.countdown`, `server.minimum_players`, `server.max_players`, `server.postgame_seconds`, `server.empty_seconds`, `server.rehost_seconds` | refer to "Dedicated server" | `HALO_SERVER_NAME`, `HALO_SERVER_ROTATION`, `HALO_SERVER_LOBBY`, `HALO_SERVER_COUNTDOWN`, `HALO_SERVER_MINIMUM_PLAYERS`, `HALO_SERVER_MAX_PLAYERS`, `HALO_SERVER_POSTGAME`, `HALO_SERVER_EMPTY`, `HALO_SERVER_REHOST` | The settings of the dedicated server. |
| `server.status_interval`, `server.control`, `server.control_output_fd`, `server.control_input_fd`, `server.control_exit_on_eof` | refer to "Dedicated server control" | `HALO_SERVER_STATUS_INTERVAL`, `HALO_SERVER_CONTROL`, `HALO_SERVER_CONTROL_OUTPUT_FD`, `HALO_SERVER_CONTROL_INPUT_FD`, `HALO_SERVER_CONTROL_EXIT_ON_EOF` | The control channel of the dedicated server. |
| `debug.update_answer` | `""` | `HALO_UPDATE_ANSWER` | The answer to the update question, for automatic tests: `yes`, `no` or `never`. Empty: the game asks. |
| `debug.exit_after` | `0.0` | `HALO_EXIT_AFTER` | The game stops after this number of seconds. `0`: never. |
| `debug.screenshot_directory`, `debug.screenshot_every` | `""`, `0` | `HALO_SCREENSHOT_DIR`, `HALO_SCREENSHOT_EVERY` | The game writes each Nth frame to this folder as a BMP file. |
| `debug.hidden_window`, `debug.null_renderer` | `false` | `HALO_HIDDEN_WINDOW`, `HALO_NULL_RENDERER` | `true`: no visible window, or no graphics. |
| `debug.gpu_stats`, `debug.gpu_trace_frame`, `debug.gpu_trace_constants`, `debug.gpu_dump_shaders`, `debug.texture_dump_directory`, `debug.texture_log`, `debug.gl_debug`, `debug.texture_no_cache` | off | `HALO_GPU_STATS`, `HALO_GPU_TRACE`, `HALO_GPU_TRACE_CONSTANTS`, `HALO_GPU_DUMP_SHADERS`, `HALO_TEXTURE_DUMP`, `HALO_TEXTURE_LOG`, `HALO_GL_DEBUG`, `HALO_TEXTURE_NO_CACHE` | Tools to find problems in the graphics: counts for each frame, all the GL state of one frame, the GLSL code, the textures. |
| `debug.gpu_skip_vertex_shaders`, `debug.gpu_debug_expression`, `debug.gpu_debug_flat`, `debug.gpu_debug_texture0` | off | `HALO_GPU_SKIP_VS`, `HALO_GPU_DEBUG_EXPR`, `HALO_GPU_DEBUG_FLAT`, `HALO_GPU_DEBUG_T0` | Tools to find problems in the graphics: skip the draws of a vertex shader, or replace the output of all pixel shaders with a GLSL expression (for example `t0.rgb`). |
| `web.join` | `""` | `HALO_WEB_JOIN` | `"first"`: join the first system link game found without the menus (`game/auto_join.c`), as picking it and pressing A in its lobby do. The site's `/play` sets it for the server the player picked (its gateway token shows only that server). The player is added as soon as the lobby has sent the machine its settings. If the game refuses the machine, or ends while the machine joins it, the machine joins again when the game is open (the next lobby), up to 10 joins; the web build stays on the page meanwhile. |
| `debug.network_log` | `false` | `HALO_NETWORK_LOG` | Log where every player is, and the netcode's counters, every second in a game (the `network test: tick` lines of the automated tests, without a test). |
| `debug.network_test`, `debug.network_test_start`, `debug.network_test_kill`, `debug.network_test_shoot`, `debug.network_test_vehicle`, `debug.network_test_pickup`, `debug.test_input` | off | `HALO_NETWORK_TEST`, `HALO_NETWORK_TEST_START`, `HALO_NETWORK_TEST_KILL`, `HALO_NETWORK_TEST_SHOOT`, `HALO_NETWORK_TEST_VEHICLE`, `HALO_NETWORK_TEST_PICKUP`, `HALO_TEST_INPUT` | Automatic tests of system link (`game/network_test.c`). Refer to `NETCODE.md`. |
| `debug.network_latency`, `debug.network_loss` | `0` | `HALO_NETWORK_LATENCY`, `HALO_NETWORK_LOSS` | The game holds all the data that it receives for this number of milliseconds, and ignores this percentage of the datagrams. Use these settings to test the netcode as on the internet. |
| `debug.telnet_console` | `false` | `HALO_TELNET_CONSOLE` | The game listens on 127.0.0.1, port 23 (telnet), for a script console. The console has no password, so only this computer can reach it. |

With Mesa drivers, the game sends its GL calls through the GL thread of
Mesa. To stop this, set the environment variable `mesa_glthread=false`.

## Updates

In this fork the self-updater is off in every build: it looks for the
original repository's releases, so no build gets a build number
(`tools/linux_build.py`) and none looks for updates. The rest of this
section describes the original repository's builds.

The builds from GitHub Actions (refer to the main [README](../../README.md#download))
can update themselves. At start-up, the game asks GitHub for the latest
release. The game does not wait for the answer. If the latest release is not
newer, the game does nothing.

If the latest release is newer, the game asks: "Do you want to update?"

- Select "Yes" to update. The game downloads the release for this platform,
  replaces its files and starts the new version. The old files get the
  extension `.old`. The new version deletes them.
- Select "No" to continue. The game asks again at the next start.
- Select "Do not ask again", then "Yes", to stop the questions. The game
  writes `auto = false` in the `[update]` section of `config.toml`. To get
  the questions again, set `auto = true`.

The game downloads through HTTPS. It examines the certificate of the server
against the certificate authorities of the system: on Linux, the bundle of
the distribution (`src/posix_update.c`, with Mbed TLS); on Windows, the
certificate store of Windows (WinHTTP). The folder of the executable must
let the game write to it.

Builds that you make yourself have no build number. They do not look for
updates.

## Frame rate

The game calculates its world at 30 Hz, as on the Xbox. On the Xbox, the
game showed one frame for each calculation (tick). This port shows one frame
for each refresh of the display, for example at 60, 120 or 240 Hz.

Each frame shows the world between the last two ticks
(`game/render_interpolation.c`):

- After each tick, the game keeps the camera, the position of each part of
  each object, and the first-person weapon.
- Each frame mixes the last two ticks. The mix agrees with the time since
  the last tick.
- Rotations use quaternions. Positions and scales are linear.
- A teleport, a respawn or a cut of the camera does not mix. It jumps.

Thus the frames are one tick (33 ms) after the calculation. The calculation
does not change.

To get 30 frames each second, set `display.interpolation = false`.

To see the frame rate:

1. Push \` to open the developer console.
2. Enter `display_framerate true`.

The frame rate shows at the bottom right of the screen. It is the mean over
half a second.

## System link

The Xbox game lets 16 players on 4 machines play a system link game. This
port lets up to 128 players on up to 128 machines play. Each machine can
have up to 4 players (split screen).

- `include/halo_port_limits.h` sets the limits.
- `include/halo_port_capacity.h` sets the memory for the limits. The game
  state is 16 MB at `0x81A00000` (3.3 MB on the Xbox). The pools of objects,
  effects, particles, contrails, lights and sounds are also larger.
- The byte-matching build keeps the limits of the Xbox. All the changes are
  in `#ifdef HALO_LINUX`.

Obey these rules:

- All the machines in a game must use a build with the same limits.
- The port uses protocol version 2. It does not see the Xbox game or older
  builds of the port. They do not see the port.

These are the differences from the Xbox:

- The host waits up to 60 seconds (15 seconds on the Xbox) for the other
  machines to load the map.
- If a machine does not read the messages of the host for two seconds, the
  host removes it from the game.
- The saved games contain all the game state. Thus a saved game is 16 MB.
  Saved games from older builds of the port do not operate.
- In campaign and in games of up to 16 players, the game removes garbage
  (bodies, dropped weapons) as on the Xbox. In larger games, it keeps more
  garbage, in proportion to the players.
- The lobby shows the local machine and the first three remote machines.
  The other machines are also in the game.
- In free-for-all games, each player is a team.

Linux, Windows and Android machines can play in the same game. Each machine
simulates the players from the same inputs, and the host does not correct
all of the game. Thus each machine must calculate the same floating-point
results, and all the ports:

- Compile without fused multiply-add (`-ffp-contract=off`).
- Use the math functions of musl (`port/include/halo_math.h`,
  `port/third_party/musl-math`), not the math functions of the system.

### Play on one computer

More than one copy of the game can play on one computer. Each copy must
have a different loopback address. A copy with an address gets no
broadcasts. Thus each copy must send its broadcasts to the other copies.

For a host and two clients, enter these commands in three terminals:

```sh
HALO_NET_ADDRESS=127.0.0.200 HALO_NET_BROADCAST=127.0.0.201,127.0.0.202 build/linux/halo
HALO_NET_ADDRESS=127.0.0.201 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
HALO_NET_ADDRESS=127.0.0.202 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
```

Do not give 127.0.0.1 to a copy. Each copy gets to its own address through
127.0.0.1. Linux and Windows send all of 127.0.0.0/8 to the loopback
interface.

### Test with many machines

`tools/system_link_bots.py` adds simple machines to a game. Each machine has
one player. The machines obey the system link protocol, but they do not
calculate the game or move their players.

1. Start a game on the host.
2. Enter `python tools/system_link_bots.py --host 127.0.0.200 --machines 127 --start`.

Each machine uses its own loopback address, from 127.0.0.2. The option
`--start` starts the game when all the machines are in the lobby. If the
host has no `network.address`, do not give `--host`.

## Dedicated server

The Linux build can be a dedicated server for system link: it hosts games
for other machines, without a window, sound or a player of its own. Set
`server.dedicated = true` in `config.toml`, or start the game with
`HALO_DEDICATED=1`. The computer does not need a display, a GPU or an audio
device, but it needs the game data (the `maps` folder, with `ui.map` and
the maps of the rotation).

```sh
HALO_DEDICATED=1 HALO_DATA_ROOT=/srv/halo \
HALO_SERVER_NAME="Blood Gulch 24/7" \
HALO_SERVER_ROTATION="bloodgulch:slayer,sidewinder:ctf,hangemhigh:king" \
build/linux/halo
```

The server:

1. Waits for the main menu to load, then hosts a game with the first entry
   of the rotation. The machines on the local network see the game with the
   name `server.name`.
2. Starts the game as soon as one player is in (the default,
   `server.lobby = false`): no countdown, and (with the netcode
   `"distributed"`) a team game (CTF, Team Slayer, ...) starts with players
   on one team only. The server does not
   run a game while nobody is there: the first game starts when the first
   player joins. With `server.lobby = true`, the lobby waits for
   `server.minimum_players` players, then counts down `server.countdown`
   seconds (a team game still starts with one team, with the netcode
   `"distributed"`). The machine of the
   server has no player, and is not in the count. The game takes
   `server.max_players` players at most. The teams stay balanced: a player
   who joins goes to the smaller team, and the teams are evened out between
   games.
3. Plays the game. If all the players go, the game stops after
   `server.empty_seconds` (counted only while no machine is joining it),
   and the next entry is set up at once: no end sequence and no scores,
   with nobody to show them to (the `postgame` event says
   `"postgame_seconds": 0`). The game continues when the other players go
   and one player (or one team) is left, because players can join a game
   in progress. (Halo ends the game then.)
4. Shows the scores for `server.postgame_seconds`, then sets up the next
   entry of the rotation. Without `server.lobby`, the next game starts as
   soon as the machines have loaded its map (or when the first player joins,
   if nobody is left); with it, the lobby opens again. After the last entry,
   the rotation starts again. From the end of the game until the next game
   is set up, the game is closed: the list shows it closed, and a machine
   that was still joining it is told that it is closed (`web.join` then
   joins the next game).
5. If the game stops because of a network failure, hosts a new game after
   `server.rehost_seconds`.

SIGINT (Ctrl+C) or SIGTERM tells the players in the lobby that the server
stops, and stops the server with exit code 0. A second signal stops the
server immediately. If the server is not in its main loop (for example, it
loads a map), a signal stops it immediately. SIGHUP reads the rotation and
the timings again, and SIGUSR1 writes a status event (refer to "Dedicated
server control").

If the server has no game data, it writes the folders that it examined to
the log and stops with exit code 1. If a map does not load, the server
stops with exit code 1 (`debug.txt` in the data root has the details). Use
a service manager (for example systemd with `Restart=on-failure`) to start
the server again.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `server.dedicated` | `false` | `HALO_DEDICATED` | `true`: the dedicated server. |
| `server.name` | `"Halo Dedicated"` | `HALO_SERVER_NAME` | The name of the game in the list of system link games. The list shows 15 characters. Use ASCII characters. |
| `server.rotation` | `"bloodgulch:slayer"` | `HALO_SERVER_ROTATION` | The games, in sequence: `map[:gametype]`, with commas, semicolons or spaces between them. |
| `server.lobby` | `false` | `HALO_SERVER_LOBBY` | `false`: the games follow one another without a lobby or a countdown; a game starts as soon as one player is in, and `server.countdown` and `server.minimum_players` do not apply. `true`: the lobby waits for `server.minimum_players`, then counts down `server.countdown` seconds. |
| `server.countdown` | `30` | `HALO_SERVER_COUNTDOWN` | With `server.lobby`: the seconds of the countdown in the lobby (0 to 600). `0`: the game starts immediately, as the immediate start of the host does. |
| `server.minimum_players` | `1` | `HALO_SERVER_MINIMUM_PLAYERS` | With `server.lobby`: the players that the countdown waits for (1 to `server.max_players`). |
| `server.max_players` | `16` | `HALO_SERVER_MAX_PLAYERS` | The maximum players in the game: 1 to 127 (the port's 128 machines in a session, less the server's own machine, which has no player). The default, 16, is the system link limit of the Xbox. Each player more costs the server bandwidth for every other player: refer to "Player count". The list of system link games shows it. When the game is full, the server refuses a machine that tries to join ("game is full"). |
| `server.postgame_seconds` | `10` | `HALO_SERVER_POSTGAME` | The seconds that the scores show after a game (after the 12 seconds of the end of the game), before the next game. |
| `server.empty_seconds` | `10` | `HALO_SERVER_EMPTY` | The seconds that a game without players continues. `0`: the game continues. |
| `server.rehost_seconds` | `5` | `HALO_SERVER_REHOST` | The seconds before the server hosts again after it lost the game. |
| `server.speed_hack` | `log` | `HALO_SERVER_SPEED_HACK` | What the host does about a client whose game runs faster than real time and ahead of the host (a speed hack; refer to `NETCODE.md`, "Speed hacks"). `off`: nothing. `log`: a log line, and a `speed_hack` event on the control channel. `refuse`: also ignores the predictions of the players of that machine while it is fast. `kick`: also kicks the machine (as the `kick` command does; a dedicated server only) after `server.speed_hack_seconds`, and tells the other players. No bans. |
| `server.speed_hack_rate` | `1.1` | `HALO_SERVER_SPEED_HACK_RATE` | The speed (times real time, over 2 seconds) at which a client counts as fast. At least 1.01. |
| `server.speed_hack_ahead_ticks` | `15` | `HALO_SERVER_SPEED_HACK_AHEAD` | The ticks (30 each second) that a fast client must also gain past its usual lead on the host (1 to 3000). A client that catches up after a stall only gets its usual lead back. |
| `server.speed_hack_seconds` | `10` | `HALO_SERVER_SPEED_HACK_SECONDS` | With `kick`: the seconds that a client must be fast without a break before the kick (2 to 600). |
| `server.status_interval`, `server.control`, `server.control_output_fd`, `server.control_input_fd`, `server.control_exit_on_eof` | refer to "Dedicated server control" | | The control channel for a supervising process: JSON events on stdout, commands on stdin. |

A map in the rotation is the name of a multiplayer map (`bloodgulch`,
`sidewinder`, ...; the file `maps/<name>.map`) or the full path of a
scenario with backslashes (`levels\test\bloodgulch\bloodgulch`). A gametype
is one of the built-in game variants: `slayer`, `team_slayer`, `ctf`,
`ironctf`, `king`, `team_king`, `oddball`, `team_oddball`, `race`,
`team_race`, `rally`, `elimination`, `stalker` or `accumulation`. Without a
gametype, the entry is `slayer`. At start-up, the server writes the rotation
to the log. It removes an entry with an unknown gametype, with no map
file, or with a map that is not a multiplayer map (a campaign level such as
`a10`, or `ui`). If no entry remains, the rotation is `bloodgulch:slayer`. Team
games start only when each team has a player.

**Teams.** A player who joins goes on the team with fewer players. On a tie,
the player goes to the team that is behind on score (in a game in progress),
otherwise the teams alternate. A player can ask for a team: the join request's
`team_index` is `0` (red) or `1` (blue), and the original clients send none.
The web build sends `HALO_WEB_PLAYER_TEAM`. The server honours the request
unless it would make the teams differ by more than one player. Players who
leave can make the teams uneven. When they differ by more than one player,
the server moves players from the bigger team to the smaller one, in the lobby
and between games. It moves players who asked for the smaller team first, then those who did not
ask for the bigger one, the last in the player list first. So a team game such as CTF starts once two
players are in, even after others left. The original's side swap between
games (red ↔ blue) stays. The log's `team balance:` lines say what happened.

The dedicated server always uses these settings, and ignores the file and
the environment for them:

| Setting | Value | Why |
| --- | --- | --- |
| `debug.null_renderer`, `debug.hidden_window` | `true` | No window and no graphics. |
| `display.interpolation`, `display.vsync`, `display.fullscreen` | `false` | The server makes 30 frames each second, with a sleep between them. |
| `audio.enabled` | `false` | No audio device. The sounds continue without output and are not mixed. |
| `network.online`, `network.allow_upnp`, `network.join_from_clipboard` | `false` | System link only: no internet play, invites or UPnP. |
| `discord.application_id` | `""` | No Discord. |
| `update.auto` | `false` | No updates. |
| `debug.network_test`, `debug.test_input` | `""` | No automatic tests. |

The dedicated server also:

- Starts only the events of SDL (no video, audio or gamepad subsystems), so
  it operates without `DISPLAY`, Wayland or PulseAudio.
- Does not open the telnet console of the game.
- Draws nothing: the main loop does not render or present frames.

The other settings, for example `network.address` and `network.broadcast`,
apply as usual. To run more than one server on one
computer, give each server a different `network.address` (refer to "Play on
one computer").

The code is in `game/dedicated_server.c` (the control
channel: `src/dedicated_control.c`). The changes to the game are in
`#ifdef HALO_LINUX` (refer to "Game source changes").

#### Rotation entry rules

A rotation entry can change the rules of its game type:
`map:gametype+key=value+key=value`, for example
`bloodgulch:slayer+score=25+lives=3+no_shields=1` or
`ratrace:ctf+score=5+flag_time=60`. The changes apply to the built-in game
type the entry names; the game type keeps its name. Values are whole
numbers, or `true`/`false` (also `1`/`0`, `on`/`off`, `yes`/`no`) for the
switches. An entry with an unknown key, a key of another game type, or a
value out of range is left out of the rotation and reported as for a bad
map (`rejected`). The events that describe an entry (`rotation`, `lobby`,
`game_started`, `status`) have its changes in `rules` (`null` for none).

| Keys | Game types | Values |
| --- | --- | --- |
| `score` | all | 1 to 999: the score to win |
| `lives` | all | 0 to 99 (0: unlimited) |
| `respawn`, `respawn_growth`, `suicide_penalty` | all | seconds: 0 to 300, 0 to 60, 0 to 60 |
| `health` | all | 25 to 400: percent of the normal health |
| `weapons` | all | 0 to 10: default, pistols, rifles, plasma, sniping, no sniping, rockets, shotguns, short range, human, no grenades |
| `vehicles` | all | 0 to 4: default, none, warthogs, ghosts, tanks |
| `goal_radar` | all | 0 motion tracker, 1 navpoints, 2 none |
| `odd_man_out`, `radar`, `friend_indicators`, `infinite_grenades`, `no_shields`, `invisible`, `generic_equipment` | all | switches |
| `assault`, `flag_must_reset`, `flag_at_home`, `flag_time` | ctf | switches; `flag_time` seconds, 0 to 600 |
| `death_bonus`, `kill_penalty`, `kill_in_order` | slayer | switches |
| `moving_hill` | king | switch |
| `random_start`, `ball_speed`, `trait_with_ball`, `trait_without_ball`, `ball_type`, `balls` | oddball | switch; 0 slow, 1 normal, 2 fast; 0 none, 1 invisible, 2 extra damage, 3 damage resistant (both traits); 0 normal, 1 magic, 2 terminator; 1 to 16 |
| `race_type`, `team_scoring` | race | 0 normal, 1 any order, 2 rally; 0 minimum, 1 maximum, 2 sum |

The code is in `game/variant_overrides.c`; `tools/variant_overrides_test.py`
tests it without game data.

#### Player count

Measured on Blood Gulch Slayer (distributed netcode, native headless
clients on one 8-core machine, 2026-10). The server keeps 30 ticks each
second with 127 players. What grows is the bandwidth: each client receives
about 8 kbit/s for each other player.

| Players | Server CPU (one core) | Server memory | To each client | Server upload (all clients) |
| --- | --- | --- | --- | --- |
| 16 | 3% | 76 MB | 0.25 Mbit/s | 4 Mbit/s |
| 32 | 6% | 77 MB | 0.39 Mbit/s | 12.6 Mbit/s |
| 64 | 11% | 78 MB | 0.66 Mbit/s | 42 Mbit/s |
| 126 | 19% | 80 MB | 0.98 Mbit/s | 123 Mbit/s |

`server.max_players` is at most 127: with 128 clients and the limit at 128,
the 128th was refused, as the server's own machine takes one of the 128
machine slots. A gateway between web clients and the server
(`port/gateway`) relays all of these bytes as well.

### Dedicated server control

A process that starts the server (for example a PHP supervisor that uses
Symfony Process) can read the status of the server and send it commands
through the pipes of the server. No network port is opened (the telnet
console stays off), so only the process that holds the pipes can control
the server.

- **Events** go to stdout, one JSON object on each line (JSON Lines). The
  human log (`halo-linux: ...` lines) goes to stderr. When the events use
  stdout, the server keeps stdout for the events only: other output that
  the game writes to stdout goes to stderr. Thus each stdout line is an
  event.
- **Commands** come from stdin, one on each line: a JSON object, or a line
  of text.
- `server.control_output_fd` and `server.control_input_fd` select other
  descriptors (for example 3 and 4, as `proc_open` can give). Then stdout
  is not changed. `-1` disables one direction.
  `server.control = false` disables the channel.

The server never waits for the supervisor. It reads stdin between frames
on the main thread, and only when `poll()` reports data, so an idle or
closed stdin costs nothing. If the events are a pipe or a socket, the
server writes them without blocking. If the supervisor does not read,
events wait in a queue of 1 MiB. After that, the server drops events, and
a `dropped` event gives the count when the queue has room again. If the
supervisor closes its end, the events stop and the server continues. When
stdin ends, the server writes `input_closed` and continues. With
`server.control_exit_on_eof = true`, it stops as for SIGTERM.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `server.control` | `true` | `HALO_SERVER_CONTROL` | `false`: no control channel. |
| `server.control_output_fd` | `1` | `HALO_SERVER_CONTROL_OUTPUT_FD` | The descriptor for events. `1` is stdout. `-1`: no events. |
| `server.control_input_fd` | `0` | `HALO_SERVER_CONTROL_INPUT_FD` | The descriptor for commands. `0` is stdin. `-1`: no commands. |
| `server.control_exit_on_eof` | `false` | `HALO_SERVER_CONTROL_EXIT_ON_EOF` | `true`: stop the server (as SIGTERM does) when the command input ends. |
| `server.status_interval` | `2` | `HALO_SERVER_STATUS_INTERVAL` | The seconds between `status` events (0 to 3600). `0`: `status` only when requested. |

#### Signals

| Signal | Effect |
| --- | --- |
| SIGINT, SIGTERM | Stop the server (refer to "Dedicated server"). Event: `shutdown`. A second signal, or a signal while the server loads a map, stops the server immediately. The server then writes `{"event":"shutdown","reason":"signal","signal":15,"immediate":true}` from the signal handler, without `seq` or `time`, and possibly after an empty line. |
| SIGHUP | Reads the settings again (as the `reload` command does, without an `id`). Event: `reloaded`. |
| SIGUSR1 | Writes a `status` event. |

#### Events

Each event is one JSON object on one line. Each event has these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `event` | string | The event name (as follows). |
| `seq` | integer | Starts at 0 and increases by 1 for each event. A gap means dropped events. |
| `time` | number | Unix time in seconds, with milliseconds. |

A reply to a command also has `id` (the id of the command, the same JSON
type, or `null`) and `command` (the name of the command, or `null`).
Ignore empty lines and fields that you do not know: new fields and new
events are not a change of the protocol version. Other changes increase
`protocol`.

Common objects:

- **entry**: `{"map": "bloodgulch", "map_path": "levels\\test\\bloodgulch\\bloodgulch", "gametype": "slayer"}`.
  `gametype` is the name from the rotation.
- **variant fields** (in `lobby`, `game_started`, status `lobby`/`game`,
  `game_ended`): `variant` (the name that the game shows, for example
  `"Slayer"`), `engine` (`ctf`, `slayer`, `oddball`, `king`, `race`,
  `terminator`, `stub`, or `none`), `teams` (boolean), `score_limit` (integer).
- **player**:

  | Field | Type | Meaning |
  | --- | --- | --- |
  | `player` | integer or null | The player slot of the server (0 to 15). Use it with `kick`. `null` for a player who left during the game (in game player lists). |
  | `name` | string | The player name. |
  | `machine` | integer | The machine index. All players of one machine (split screen) have the same value. |
  | `controller` | integer | The local player of the machine (0 to 3). |
  | `color` | string or null | The player's colour: `white`, `black`, `red`, `blue`, `gray`, `yellow`, `green`, `pink`, `purple`, `cyan`, `cobalt`, `orange`, `teal`, `sage`, `brown`, `tan`, `maroon` or `salmon` (the one the player asked for, else the one the server picked). Team games draw players in their team's colour instead. |
  | `team`, `team_name` | integer, string, or null | `0`/`"red"`, `1`/`"blue"`. `null` in games without teams. |
  | `connected` | boolean | `false`: the player left this game (the statistics remain). |
  | `machine_name` | string or null | The name of the machine (in the lobby list). |
  | `address`, `port` | string, integer, or null | The IPv4 address and UDP port of the machine. `null` for the machine of the server. |
  | `ping_ms` | integer or null | The round trip in milliseconds. `null` until the round trip has been measured. |
  | `kills`, `deaths`, `assists`, `suicides`, `team_kills`, `score` | integer | In a game only (not in the lobby). `score` is the score of the gametype (kills, flag captures, seconds with the ball or on the hill, laps). |
  | `score_text` | string | The score as the scoreboard shows it (for example `"1:05"` for time scores). In a game only. |
  | `won` | boolean or null | In `game_ended` only. `null`: a tie. |

- **team_scores**: `[{"team": 0, "name": "red", "score": 3}, ...]` (teams
  that have players), or `null` (no team game, or not in a game).

| Event | When | Fields |
| --- | --- | --- |
| `starting` | First, when the process starts. | `protocol` (1), `pid`, `name`, `commands` (boolean: the server reads commands). |
| `server_started` | Once, when the first lobby opens. | `name`, `protocol`, `network_version` (the game's network version, `HALO_PORT_NETWORK_VERSION`: clients of another refuse the server), `distributed` (boolean: always `true` now that the lockstep netcode is gone), `rotation` (array of entries), `settings` `{lobby, countdown, minimum_players, max_players, postgame_seconds, empty_seconds, rehost_seconds, status_interval, exit_on_eof}`. |
| `lobby` | The lobby opens, the player count changes, the countdown starts or stops, or the lobby map changes. | `reason` (`opened`, `players`, `countdown_started`, `countdown_stopped`, `map_changed`), entry fields, variant fields, `player_count`, `minimum_players`, `max_players`, `countdown` (the seconds that remain, or `null`), `players` (array). |
| `game_started` | The map loaded and the game can score. | Entry fields, variant fields, `players` (array). |
| `player_joined` | A player joins (lobby or game). | `player`, `name`, `machine`, `controller`, `color`, `team`, `team_name`, `machine_name`, `address`, `port`, `in_game` (boolean). |
| `player_left` | A player leaves or is kicked. | The same fields as `player_joined`, without `in_game`, and `reason` (`left` or `kicked`). |
| `speed_hack` | A client's game runs too fast (`server.speed_hack`, not `off`): the first window of 2 seconds, each 10 seconds after that while it continues, and when it is kicked. | `machine`, `machine_name`, `address`, `port`, `players` `[{player, name}]`, `rate` (times real time, in the last window of 2 seconds), `ahead_ticks` (the ticks that it gained past its usual lead on the host), `seconds` (how long it has been fast), `action` (`logged`, `predictions_refused` or `kicked`; a kick also writes `player_left` with reason `kicked`). A web supervisor can map `address` (the gateway session) to the account, and ban the account itself: the server keeps no bans. |
| `score` | During a game, when a statistic or a score changes. At most once each second. | Entry fields, `time_elapsed` (seconds), `team_scores`, `players`. |
| `status` | Each `server.status_interval` seconds, on SIGUSR1, and as the reply to `status` (then with `id`/`command`). | `state` (`starting`, `hosting`, `lobby`, `game`, `postgame` (from `game_ended` on, the game's end screen included), `waiting`), `name`, `network_version`, `uptime` (seconds), `games_hosted`, `rotation`, `rotation_index` (the current game position in the rotation, or `null` if a command set the game), `next` (entry), `lobby` (object or `null`), `game` (object or `null`). |
| `game_ended` | The game ends (before the scores show). | `reason` (`game_over`, `empty`, `end_game`, `next_map`, `change_map`), entry fields, variant fields, `time_elapsed`, `time_remaining` (always `null`), `team_scores`, `players` (with `won`), `next` (entry). |
| `postgame` | The scores show. | Entry fields (the game that ended), `postgame_seconds` (`0` when the scores are skipped: a game that ended empty, or `next_map` with `skip_postgame`), `next` (entry). |
| `reloaded` | After SIGHUP. | `rotation_changed`, `rotation`, `rejected` `[{entry, reason}]`, `settings`. |
| `input_closed` | The command input ended. | `exiting` (boolean). |
| `shutdown` | The server stops. | `reason` (`signal`, `command`, `input_closed`), `signal` (integer or `null`), `immediate` (boolean), `games_hosted`. |
| `dropped` | Events were dropped because the supervisor did not read. | `count`. |
| `ack` | A command succeeded. | `id`, `command`, and fields for each command (refer to "Commands"). |
| `error` | A command failed, or the server has a problem. | `id`, `command` (`null` if not a reply), `code`, `message`, `fatal`. If `fatal` is `true`, the server stops with exit code 1. |

The status `lobby` object has entry fields, variant fields,
`player_count`, `minimum_players`, `max_players`, `countdown`, and `players` (array). The status `game` object has
`ended` (boolean), entry fields, variant fields, `time_elapsed` (or `null`
while the map loads), `time_remaining` (`null`), `team_scores`, and
`players` (array).

The Xbox gametypes have no time limit: a game ends at its score limit, so
`time_remaining` is always `null`.

Error codes of the server (with `id: null`):

| Code | Fatal | Meaning |
| --- | --- | --- |
| `no_game_data` | yes | No `maps` folder. Exit code 1. |
| `map_load_failed` | yes | A map did not load (`debug.txt` has the details). Exit code 1. |
| `startup_failed` | yes | SDL did not start. |
| `halt` | yes | The game halted (a failed assertion or a fatal error; `message` has the file, the line and the condition, and `debug.txt` has the details). Exit code 1. |
| `no_playable_rotation` | no | No usable entry in `server.rotation`: the server plays `bloodgulch:slayer`. |
| `rotation_entry_rejected` | no | The server removed an entry from the rotation (unknown gametype, or no map file). |
| `game_lost` | no | The game stopped because of a network failure or an abort. The server hosts again after `server.rehost_seconds`. |
| `cannot_host` | no | The server could not host. It tries again. |
| `line_too_long` | no | A command line was longer than 64 KiB and was ignored. |
| `event_too_large` | no | An event was too large and was not written. |

#### Commands

A command is one line: a JSON object, or text.

```
{"cmd":"kick","id":17,"player":3}
kick 3
```

JSON: `cmd` (or `command`) is the name. `id` (optional, string or
number) is copied to the reply. The other keys are arguments. Their values
are strings, numbers, booleans, `null`, or arrays of those (the server
joins array values with commas). Nested objects are an error. Text: the
first word is the name. `key=value` words are arguments. The other words
are the text of the command, for example `change_map bloodgulch:ctf now`.
The server ignores empty lines and lines that start with `#`. It reads up
to 32 commands each frame (30 frames each second) and runs them in
sequence.

Each command gets one reply: `ack` or `error`, with the `id` of the
command. Some commands then cause other events (for example `status`, or
`player_left`). A text command has no id, so its replies have `"id":
null`.

| Command | Arguments | Result |
| --- | --- | --- |
| `status` | none | `ack`, then a `status` event with the same `id`. |
| `next_map` | `skip_postgame` (boolean, optional) | Ends the current game and plays the next game (the next rotation entry, or the game that `change_map` set). `ack` `{effect, next}`. `effect`: `ending_game` (a game was in progress; `game_ended` follows with reason `next_map`), `skipping_postgame` (the scores show: the lobby opens now), `already_advancing`, or `lobby` (the lobby changes to the next game now). |
| `end_game` | none | Ends the current game as the score limit does. The scores show, then the rotation continues. `ack` `{effect: "ending_game"}`. `error` `not_in_game` if no game is in progress. |
| `change_map` | `map`, `gametype` (optional, default `slayer`), `now` (boolean); or text `map[:gametype] [now]` | Sets the next game. In the lobby: the lobby changes now (`effect: "lobby"`). In a game: the game is next (`next_game`); with `now`, the current game ends and the scores do not show (`ending_game`). Before the server hosts: `next_lobby`. The rotation continues after that game (at the position where it stopped). `ack` `{effect, next}`. `error` `invalid_map` (unknown gametype, or no map file). |
| `set_rotation` | `rotation` (string `"a:b,c"` or array `["a:b","c"]`), or the text | Replaces the rotation (in memory only, not in `config.toml`). The next game is the first entry of the new rotation (a `change_map` that is waiting goes first). In the lobby, the lobby changes now. `ack` `{effect, rotation, rejected}`. `error` `invalid_rotation` (no usable entry; the reply has `rejected`), with no change. |
| `kick` | `player` (slot), `machine`, or `name`; or text `kick <player>` | Removes the machine of the player from the game, as a lost connection does. All players of that machine go. It is not a ban: the machine can join again. `ack` `{machine, machine_name, players}`, then a `player_left` event (reason `kicked`) for each player. Errors: `not_hosting`, `no_such_player`, `ambiguous_name`, `no_such_machine`, `cannot_kick_host`, `kick_failed`. |
| `reload` | none | Reads `server.rotation`, `server.lobby`, `server.countdown`, `server.minimum_players`, `server.max_players`, `server.postgame_seconds`, `server.empty_seconds`, `server.rehost_seconds`, `server.status_interval`, and `server.control_exit_on_eof` again from `config.toml` and from the environment of the process. The environment has priority, and the environment of a running process does not change. If the rotation text changed, the rotation starts again as for `set_rotation`. `ack` `{rotation_changed, rotation, rejected, settings}`. `error` `reload_failed` (errors in `config.toml`; no change). If the new rotation has no usable entry, the server writes `error` `invalid_rotation` and then the `ack` with `rotation_changed: false`. |
| `quit` | none | `ack`, then the server stops as for SIGTERM (`shutdown` with reason `command`, exit code 0). |
| `help` | none | `ack` `{protocol, commands}`. |
| `say`, `broadcast` | any | `error` `not_supported`. System link has no chat, and the host has no message that shows text on the machines of the players. |

Errors for all commands: `bad_json` (the line is not a JSON object, a
value is a nested object, or the line has other text after the object),
`bad_request` (no `cmd`, a bad argument, more than 16 arguments, or
arguments longer than 16 KiB), and `unknown_command`.

Example session (`>` is stdin, `<` is stdout):

```
< {"event":"starting","seq":0,"time":1790662000.101,"protocol":1,"pid":4242,"name":"Halo Dedicated","commands":true}
< {"event":"server_started","seq":1,"time":1790662004.512,"name":"Halo Dedicated","protocol":1,"distributed":false,"rotation":[{"map":"bloodgulch","map_path":"levels\\test\\bloodgulch\\bloodgulch","gametype":"slayer"},{"map":"sidewinder","map_path":"levels\\test\\sidewinder\\sidewinder","gametype":"ctf"}],"settings":{"lobby":true,"countdown":30,"minimum_players":1,"max_players":16,"postgame_seconds":15,"empty_seconds":10,"rehost_seconds":5,"status_interval":2,"exit_on_eof":false}}
< {"event":"lobby","seq":2,"time":1790662004.513,"reason":"opened","map":"bloodgulch","map_path":"levels\\test\\bloodgulch\\bloodgulch","gametype":"slayer","variant":"Slayer","engine":"slayer","teams":false,"score_limit":25,"player_count":0,"minimum_players":1,"max_players":16,"countdown":null,"players":[]}
< {"event":"player_joined","seq":3,"time":1790662010.020,"player":0,"name":"Chief","machine":1,"controller":0,"color":"cobalt","team":null,"team_name":null,"machine_name":"Xbox","address":"192.168.1.20","port":2302,"in_game":false}
> {"cmd":"change_map","id":"a1","map":"sidewinder","gametype":"ctf"}
< {"event":"ack","seq":5,"time":1790662011.300,"id":"a1","command":"change_map","effect":"lobby","next":{"map":"sidewinder","map_path":"levels\\test\\sidewinder\\sidewinder","gametype":"ctf"}}
< {"event":"lobby","seq":6,"time":1790662011.301,"reason":"map_changed","map":"sidewinder",...}
> {"cmd":"kick","id":2,"player":0}
< {"event":"ack","seq":9,"time":1790662020.000,"id":2,"command":"kick","machine":1,"machine_name":"Xbox","players":1}
< {"event":"player_left","seq":10,"time":1790662020.000,"player":0,"name":"Chief","machine":1,"controller":0,"color":"cobalt","team":0,"team_name":"red","machine_name":"Xbox","address":"192.168.1.20","port":2302,"reason":"kicked"}
> status
< {"event":"ack","seq":11,"time":1790662021.000,"id":null,"command":"status"}
< {"event":"status","seq":12,"time":1790662021.000,"id":null,"command":"status","state":"lobby",...}
```

A game `status` (abbreviated):

```json
{"event":"status","seq":40,"time":1790662100.0,"state":"game","name":"Halo Dedicated","uptime":100.2,"games_hosted":1,
 "rotation":[...],"rotation_index":0,"next":{"map":"sidewinder","map_path":"levels\\test\\sidewinder\\sidewinder","gametype":"ctf"},
 "lobby":null,
 "game":{"ended":false,"map":"bloodgulch","map_path":"levels\\test\\bloodgulch\\bloodgulch","gametype":"team_slayer",
   "variant":"Team Slayer","engine":"slayer","teams":true,"score_limit":50,"time_elapsed":83.4,"time_remaining":null,
   "team_scores":[{"team":0,"name":"red","score":7},{"team":1,"name":"blue","score":5}],
   "players":[{"player":0,"name":"Chief","machine":1,"controller":0,"color":"cobalt","team":0,"team_name":"red","connected":true,
     "machine_name":"Xbox","address":"192.168.1.20","port":2302,"ping_ms":null,"kills":7,"deaths":2,"assists":1,
     "suicides":0,"team_kills":0,"score":7,"score_text":"7"}]}}
```

`tools/dedicated_control_test.py` tests the channel without game data
(`python3 tools/dedicated_control_test.py --binary build/linux/halo`).
`tools/auto_join_test.py` runs a real dedicated server and `HALO_WEB_JOIN`
clients (it needs the game data and `xvfb-run`, and skips without them:
`python3 tools/auto_join_test.py --binary build/linux/halo --data-root <root>`).
It checks the add-player delay, a lobby join, a late join, a machine refused
as a game ends (it joins again), an empty game going straight to the lobby,
a client joining as the empty countdown runs out, and the CTF teams.

## Internet play

Machines with an invite link can play system link on the internet. This
project has no server.

When a copy of the game starts to host a system link game, it makes an
invite link: `halo://join/<44 hexadecimal digits>`. The game writes the link
to the standard error and puts it on the clipboard.

To join a game, do one of these steps:

- Open the link. The game is the handler of `halo://` links. If the game
  already operates, the new copy gives the link to it and stops. A key in a
  file that only the user can read (`halo-ce-universal.key` in
  `$XDG_RUNTIME_DIR`, else `~/.halo-ce-universal.key`; on Windows in
  `%LOCALAPPDATA%`) encrypts the link, so the programs of other users cannot
  read it.
- Copy the link (or the 44 digits) and go to the game.
- Enter `halo <link>`.
- Accept a Discord invite. Refer to "Discord".

When the machines connect, the game of the host shows in Multiplayer,
System Link. Join the game as on a local network. System link on a local
network does not need an invite.

### Security

Only machines with the invite can find the game:

- Each copy of the game makes an X25519 key pair when it starts. Its
  identifier is from the hash of its public key.
- The link contains the identifier of the host and a random 16-byte token.
- The machines exchange their public keys and addresses through public MQTT
  brokers (`network.signalling_brokers`). The topics are HMACs of the token.
  A key from the token encrypts and authenticates the messages
  (`src/p2p_signal.c`, `src/p2p_crypto.c`). The host authenticates its answer
  with a key that only it and the player can calculate. Its public key must
  agree with the identifier in the link.
- Each two machines get the keys of their packets from their key pairs and
  a random number from each. The keys do not go through the brokers. Thus
  other machines with the invite cannot read or change the packets.
- Each packet is encrypted and authenticated, with a different key in each
  direction. A machine ignores a packet that it already received.
- A machine can send only to the ports of the game on the other machine.
- An invite operates while the copy of the game that made it operates.

### Connection

Each machine gets its public address from public STUN servers. Then the two
machines send packets to each other until the packets get through (UDP hole
punching). There is no relay.

Some networks give a different port for each destination (for example some
mobile and company networks). Two machines behind such networks cannot
connect. To connect, forward `network.tunnel_port` on the router of one of
the machines.

The game can ask the router to forward the port (UPnP,
`src/posix_upnp.c`, with `port/third_party/miniupnpc`):

- The host asks its router when a player uses its invite.
- A player that joins asks its router when it does not reach the host in
  5 seconds.
- The forwarded port is one more address that the machine gives to the
  other machine.
- The forward has a duration of one hour. The game makes it longer while
  it operates, and removes it when the game stops.
- UPnP does not help behind a second NAT, for example the NAT of a mobile
  network provider. Then the router has a private address, and the game
  does not ask.

To stop all UPnP requests, set `network.allow_upnp` to `false`.

In the game, each machine has an address in 100.64.0.0/10:

- `src/xnet.c` sends the traffic of the game to such an address through
  local sockets on 127.0.0.1 (or `network.address`).
- `src/p2p.c` sends that traffic through one UDP socket. UDP datagrams go
  as they are. TCP connections go as KCP streams (`port/third_party/kcp`).
- The broadcasts of the game go to all the machines. Thus the game of the
  host shows on the other machines.

### Discord

If the Discord desktop client operates, the game of the host shows in
Discord (through the application of `discord.application_id`). The activity
has a private party with the invite as its join secret. The host can send
the invite with the invite button of Discord. When a person accepts it, that
person joins the game. If the game does not operate, Discord starts it.

## What operates

| Area | Status |
| --- | --- |
| Game code | All 466 C files of the game. The changes are in "Game source changes". |
| Graphics | Direct3D 8 on OpenGL 4.5 core through SDL3 (`src/d3d8_gl.c`). The port translates the NV2A vertex shaders and register combiners to GLSL. It decodes all the Xbox texture formats. The vertex and index buffers come from a GL copy of the Xbox memory. |
| Sound | Xbox DirectSound on SDL3 audio (`src/dsound_sdl.c`): PCM and Xbox ADPCM, mixed at 48 kHz, with volume, pitch, mix bins, distance, stereo pan, occlusion and obstruction. There is no Doppler effect, no cones and no reverb. |
| Input | XInput on SDL3 (`src/xinput_sdl.c`): keyboard, mouse, gamepads with rumble, and the debug keyboard for the console. |
| Files | The Win32 file functions and the MSVC file functions on POSIX, with the translation of Xbox paths. |
| Threads | Threads, events, mutexes, critical sections, interlocked operations and alertable waits. |
| Memory | The port reserves the Xbox memory at `0x80000000`. Thus the game gets the fixed addresses that it expects. |
| Saved games | The Xbox `UDATA` layout, with SHA-1 signatures. |
| Networking | Winsock on BSD sockets. System link on a local network and on the internet. |
| Bink video | Not available. The game skips the movies. |

## How the port operates

### The compiler

`tools/linux_build.py` compiles the game with clang and these options, which
give the ABI of the MSVC compiler:

- `--target=i686-linux-gnu`: 32-bit x86.
- `-fms-extensions`: the MSVC extensions.
- `-fshort-wchar`: 16-bit `wchar_t`.
- `-malign-double`: 8-byte alignment of 64-bit members.
- `-fcommon`: tentative definitions, as in C89.

glibc gives only ISO C (`__STRICT_ANSI__`). Thus POSIX names, for example
`random`, do not conflict with the names of the game.

These files supply the MSVC functions that clang does not have:

| File | Contents |
| --- | --- |
| `include/halo_linux_prefix.h` | The first header of each file: the architecture macros of the SDK, MSVC `__inline`, SEH keywords, `__declspec(selectany)`. |
| `include/` | Headers that add MSVC names to the C runtime headers. |
| `port/include/xdk` | The Xbox SDK declarations. The compiler reads this folder after all the other folders. |
| `tools/linux_msvc_semantics.py` | Makes a header that declares each struct tag at file scope, as MSVC does. It also makes the header inline functions weak, as the COMDAT functions of MSVC. `game/msvc_comdat.c` gives one external copy of each. |
| `include/halo_linux_winsock_names.h` | Gives new names to the Winsock functions of the SDK. Thus they do not link to the glibc functions with the same names. |
| `include/halo_linux_source_fixups.h` | Repairs one declaration conflict (`rasterizer_debug_drawing_begin`). |

`tools/linux_link_check.py` stops the link if a weak reference has no
definition. Without this check, the linker gives the reference the address
0.

### The platform layer (`src/`)

- The files `posix_*.c` use glibc. The compiler uses the ABI of the host
  for these files, because some glibc structures have a different layout
  with `-malign-double`.
- The other files include the SDK declarations through `platform.h`. Thus
  the compiler examines each definition against the SDK prototype.
- `src/halo_linker_common.c` gives weak storage for some globals of the
  January link, and for `fast_ftol_C` and `main_crash`.
- `main/d3d_intimacy.cpp` reads a private structure of the Xbox Direct3D.
  The Linux build does not use this file. `src/d3d8_gl.c` gives
  `d3d_find_flipcount`.
- The build returns small structures and unions in registers
  (`-freg-struct-return`), as on Win32.

### Game source changes

Five files of the game have changes for clang. These changes do not change
the MSVC objects: a comparison of all 612 C objects showed no difference in
code or data.

| File | Change |
| --- | --- |
| `cseries/cseries.c` | The naked function `stristr` uses `[ebp+8]` and `[ebp+12]` for its parameters. |
| `bitmaps/bitmap_drawing.c` | `*((word *)p)++` is now `*(*(word **)&p)++`. |
| `rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.c` | `&(T *)x` is now `(T **)&x`. |
| `hs/hs.c` | Local prototypes that did not agree with `ai_script.h` are removed. |
| `units/vehicles.c` | The local prototype of `unit_update_animation` uses the type of `units.h`. |

`math/real_math.h` had a copy of `plane2d_from_points` that did not agree
with the function in `effects/decals.c`. clang used the copy, and parts of
levels were not visible. The copy now agrees with the function.

Other changes are in `#ifdef HALO_LINUX`. All the native ports define
`HALO_LINUX`. The byte-matching build does not define it.

| File | Change |
| --- | --- |
| `scenario/scenario.c` | The BSP connection tables have names, not MSVC offsets. |
| `rasterizer/xbox/rasterizer_xbox_environment_fog.c` | A local pointer gets its value from the file-scope array with the same name. |
| `game/player_control.c` | The mouse aims the player on controller 1 directly. |
| `sound/game_sound.c` | The game calculates the obstruction of each sound one time for each tick, not for each frame. |
| `cseries/errors.c` | `debug.txt` stays open between lines. |
| `networking/`, `game/`, `interface/`, `bungie_net/network/` and the pools of objects, effects and sounds | The system link limits and the memory for them. |
| `game/`, `objects/`, `units/`, `networking/` | The distributed netcode. Refer to `NETCODE.md`. |
| `interface/hud.c`, `rasterizer/rasterizer_text.c` | In multiplayer, players' names are drawn above their heads (`display.player_names`, `display.player_name_scale`); text can be drawn scaled about a point (`rasterizer_text_set_scale`). |
| `game/players.c`, `networking/network_server_message_handler.c`, `networking/network_server_manager.c`, `interface/virtual_keyboard.c` | Players' names are kept to text that can be typed and told apart: the host trims the spaces around a name and removes characters that draw as nothing; a name with nothing left to type (in ASCII, a letter with a mark as its plain letter: "jose" for "José") becomes "Player"; a name that reads the same as another player's (in either case) gets a number ("Player2"). The profile name keyboard refuses such a name, and (not in the web build, which names the player from the site) a multiplayer game refuses a profile whose name was made before this check. |
| `main/main.c`, `shell/shell_xbox.c`, `game/game_engine.c`, `interface/ui_widget.c`, `networking/network_server_manager.c`, `networking/network_client_manager.c`, `networking/network_game_manager.c`, `networking/telnet_console.c` | The dedicated server: no rendering, a 30 Hz sleep, no host player in the checks, the automatic countdown, the automatic return to the lobby, the server name, no telnet console, the fatal error event of a map that does not load, and the lobby, player and machine information and the kick for the control channel. Refer to "Dedicated server" and "Dedicated server control". |

The x86 inline assembly of the game has C replacements in
`#ifdef HALO_LINUX`. Thus the compiler can optimize that code for each
processor:

| File | Assembly | Replacement |
| --- | --- | --- |
| `cseries/cseries.h` | x87 `fistp` (`fast_ftol`) | `__builtin_rint` |
| `bitmaps/bitmaps_inlines.h` | x87 conversions | C conversions |
| `math/matrix_math.c` | SSE `matrix4x3_multiply` | a C loop |
| `effects/decals.c` | an x87 conversion | a C conversion |
| `cseries/profile.c` | `rdtsc` | `QueryPerformanceCounter` |
| `cseries/cseries.c` | naked `stristr` | a C `stristr` |
| `cseries/stack_walk_windows.c` | a read of EBP | `__builtin_frame_address` |
| `interface/hud_draw.c` | a read of `[ebp+4]` | `__builtin_return_address(1)` |
| `bink/bink_playback.c` | `int 3` | `__builtin_trap` |

The x87 control and status words (`_control87`, `_statusfp`, `_clearfp` in
`src/msvc_crt.c`) use `fenv.h`. On Android, they use the FPCR and FPSR.
