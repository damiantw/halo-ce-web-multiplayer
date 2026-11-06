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
| [PR #474](https://github.com/OpenCommunityEdition/OpenCE/pull/474) (`933cad12`) | The host refuses a client's reported point hit with no line through the level's structure from the shooter to the target | `port/linux/game/network_damage.c`, `port/linux/NETCODE.md` |
| [PR #466](https://github.com/OpenCommunityEdition/OpenCE/pull/466) (`667c2f29`, `07c8430b`, `32604c8e`) | A client keeps an inventory whose unit or weapons have not come yet, and applies it once they have | `port/linux/game/network_objects.c`, `port/linux/NETCODE.md`, `tools/test_retried_inventories.py`, `tools/fixtures/retried_inventories.c`, `.github/workflows/build.yml` |
| [PR #490](https://github.com/OpenCommunityEdition/OpenCE/pull/490) (`1fc3a350` only) | A failed `SDL_Init` is logged once, and again only when its reason changes, rather than on every frame that asks for SDL | `port/linux/src/sdl_platform.c` |
| [PR #487](https://github.com/OpenCommunityEdition/OpenCE/pull/487) (`b3641162`) | A light the window has no room for (past `MAXIMUM_LIGHTS_PER_WINDOW`) no longer zeroes the submitted-light count or lands in an object's lights, and "too many lights" is logged once in a while instead of for each light | `source/objects/object_lights.c`, `source/rasterizer/rasterizer_lights.c` |
| [PR #542](https://github.com/OpenCommunityEdition/OpenCE/pull/542) (`0e7233f3`, merge `85350eab`) | An object's shadow map is centered where the frame draws the object between ticks. The move is `render_interpolation_object_point`, which reads the object interpolation this fork already had | `source/render/render_objects.c`, `port/linux/game/render_interpolation.c`, `port/linux/include/halo_linux_source_fixups.h` |
| [PR #540](https://github.com/OpenCommunityEdition/OpenCE/pull/540) (`8468b4c3`, merge `c3ddd20c`), the slope only | A z bias's slope is taken across the game's pixels, multiplied by the bound target's scale. The depth-unit term upstream adds in the vertex program for `display.reversed_depth` is not taken: this fork has no reversed depth | `port/linux/src/d3d8_gl.c` |
| [PR #541](https://github.com/OpenCommunityEdition/OpenCE/pull/541) (`6f1a31d6`, `cdd43c65`, merge `13908e4d`) | Each lens flare's occlusion test has a slot of its own and is read only once a count of that flare's own test has come. Numbered on this fork's visibility paths (the web query, the desktop query buffer, the CPU query read, and the Android counter) | `source/rasterizer/rasterizer_lights.c`, `port/linux/src/d3d8_gl.c` |
| [PR #552](https://github.com/OpenCommunityEdition/OpenCE/pull/552) (`688eeb8a`, merge `07728f92`) | A lightning bolt keeps the shape of the first frame of a tick until the next tick | `source/objects/widgets/lightning.c` |
| [PR #546](https://github.com/OpenCommunityEdition/OpenCE/pull/546) (`08efb40e`, `cf3f709f`, merge `97fd9de7`; `cf3f709f` landed on upstream main with [PR #562](https://github.com/OpenCommunityEdition/OpenCE/pull/562)) | Contrail points are made and aged by the ticks' own time, a 30th of a second each, half of that while the players are double speed | `source/effects/contrails.c` |
| [PR #536](https://github.com/OpenCommunityEdition/OpenCE/pull/536) (`5cd9bc79`, merge `0c70ca6a`) | A scope's zoom blur is drawn to the left and top edges of the screen | `source/rasterizer/xbox/rasterizer_xbox_screen_effect.c` |

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
