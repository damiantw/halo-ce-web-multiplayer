# License notice

This repository is a fork of
[OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE)
(formerly cybersecurity/halo-ce-universal). This note says which license
covers which code. It describes where code came from; it is not legal advice.

## Before upstream 4b09168e: CC0 1.0

Upstream OpenCE was released under CC0 1.0 until its commit
[`4b09168e`](https://github.com/OpenCommunityEdition/OpenCE/commit/4b09168e7fad3b0c2ac4e2aef72d7ab5cb5e6f85)
(19 October 2026), which switched it to the GNU Affero General Public License,
version 3 or later (AGPL-3.0-or-later). Upstream states that its source at
every earlier commit stays under CC0 1.0.

Everything in this repository up to and including main at `2d7c2077` (upstream
code taken from before that switch, and this fork's own changes) was released
under CC0 1.0 and stays under CC0 1.0. Its text is in
[LICENSES/CC0-1.0.txt](LICENSES/CC0-1.0.txt).

## Ported from upstream after 4b09168e: AGPL-3.0-or-later

These upstream changes reached upstream main after `4b09168e` (with upstream
PR #416), and this repository takes them under AGPL-3.0-or-later. Some were
first written on upstream branches before the switch; they are treated here as
AGPL-3.0-or-later all the same.

| Upstream | What it changes | Files here |
| --- | --- | --- |
| [PR #373](https://github.com/OpenCommunityEdition/OpenCE/pull/373) (`2482087e`) | Web index cache: a buffer for each frame, so WebGL on Metal does not stall | `port/linux/src/d3d8_gl.c` |
| `e6fdb07c` | Scope zoom blur: a fixed number of passes at any resolution | `source/rasterizer/xbox/rasterizer_xbox_screen_effect.c` |
| `2223f945` | No Running Riot for a killer with no kills in a row | `source/game/game_engine.c` |
| `8453be8d` | Team Oddball's built-in variant starts zeroed | `source/game/game_engine.c` |
| [PR #375](https://github.com/OpenCommunityEdition/OpenCE/pull/375) (`40c1db8a`) | A client leaves the host's bipeds that fell out of the map to the host | `source/units/bipeds.c` |
| [PR #349](https://github.com/OpenCommunityEdition/OpenCE/pull/349) (`956387d8`) | A client deletes a CTF flag or oddball on the host's word without halting, and clears its goal | `port/linux/game/network_objects.c`, `port/linux/NETCODE.md`, `source/game/game_engine.c`, `source/game/game_engine.h`, `source/items/weapons.c`, `source/objects/objects.c` |
| [PR #361](https://github.com/OpenCommunityEdition/OpenCE/pull/361) (`c94e7053`) | A looping sound's track stops when its looping sound is gone | `source/sound/sound_manager.c`, `port/linux/README.md` |
| `313b6c21`, `da22f9b4` | The Linux crash handler is installed at start, put back over a library's, and also reports SIGBUS, SIGILL, SIGFPE and SIGABRT | `port/linux/src/memory_watch.c`, `port/linux/src/platform.h`, `port/linux/src/d3d8_gl.c`, `port/web/src/web_memory_watch.c`, `port/windows/src/win32_memory_watch.c`, `port/android/guest/runtime/guest_memory_watch.c`, `tools/test_memory_watch.py` |
| [PR #456](https://github.com/OpenCommunityEdition/OpenCE/pull/456) (`99c20307`) | A GPU without S3TC gets DXT1 textures as ASTC 4x4, their bits moved (here: WebGL 2's `WEBGL_compressed_texture_astc`, only where there is no S3TC; `HALO_WEB_NO_ASTC=1` turns it off) | `port/linux/src/dxt1_astc.h`, `port/linux/src/xbox_textures.c`, `port/linux/src/d3d8_gl.c`, `port/linux/src/xgpu.h`, `port/linux/src/port_config.c`, `port/linux/README.md`, `port/web/shell/halo-loader.js`, `tools/test_dxt1_astc.py`, `tools/fixtures/dxt1_astc.c` |
| [PR #450](https://github.com/OpenCommunityEdition/OpenCE/pull/450) (`5744d2d1`, `ef0e3be4`, `962c3eca`) | Scenery and device shields break, and tripmines go off, for clients as for the host (here network version 11; on the static-target damage of upstream `7a1ffca2`, which is CC0 1.0 from before the switch) | `port/linux/game/network_damage.c`, `source/objects/damage.c`, `port/linux/NETCODE.md`, `port/linux/include/halo_port_limits.h`, `tools/test_static_target.py`, `tools/fixtures/static_target.c` |

The text of the AGPL 3.0 is in [LICENSE](LICENSE).

## The combined work: AGPL-3.0-or-later

Because it includes the code above, the game as built and distributed from
this repository (the Linux dedicated server and the web client) is under
AGPL-3.0-or-later as a whole. The CC0 parts can still be used on their own
under CC0.

## Source

The source is this repository:
<https://github.com/damiantw/halo-ce-web-multiplayer>. The AGPL (section 13)
asks that people who play a changed version over a network, as on
halo.damian.lol, be offered its source; the site links here.

## Not covered

- Third-party code, fonts and art keep their own licenses, which are next to
  them (for example `port/third_party`).
- The license covers the work of the project's authors. It gives no rights in
  Halo, its maps or anything else Microsoft owns. No game data is in this
  repository.
