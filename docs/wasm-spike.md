# WebAssembly spike: the game client in the browser, and the gateway

Status: feasibility spike on local branch `wasm-spike` (made from `dedicated-control` at ea1f9cf5), 29 Sep 2026.
Nothing here has been pushed.

**Result: yes. We can build the client ourselves from this source with Emscripten.** In headless Chrome
(SwiftShader software WebGL2) the build loads `ui.map` from an HTTP server, plays the main menu music,
draws the animated main menu with its widgets at 20–70 fps, and takes keyboard input
(Main menu → Multiplayer → Select Profile). **Recommendation: go.** The remaining work is ordinary
porting work. None of it needs a new approach.

Screenshots (from the user's own disc data; not committed):

| file | what it shows |
|---|---|
| `shots/wasm_final_01.png` | main menu with widgets (HALO logo, Campaign/Multiplayer/Settings) |
| `shots/wasm_mainmenu_01.png` | same, GL-error checking on |
| `shots/wasm_input_02.png` | after ArrowDown+Enter: the Select Profile screen (small text garbled: see "texture cache") |
| `shots/wasm_profile_text_nocache.png` | same screen with `HALO_TEXTURE_NO_CACHE=1`: the text is correct, which confirms the cause |
| `shots/wasm_menu_00.png`, `wasm_run7_02.png` | earlier milestones: the menu background only |

## How to build and run

```sh
source <emsdk>/emsdk_env.sh                    # emsdk "latest" = emcc 6.0.10, SDL3 port 3.4.2
python3 configure.py && ninja web              # -> build/web/halo.{js,wasm} (3.0 MB wasm, 0.3 MB js), ~10 s on 8 cores
python3 port/web/serve.py --maps /path/to/maps --port 8000   # COOP/COEP/CORP, Range, /maps/index.json
# open http://127.0.0.1:8000/   (only ui.map is fetched first, other maps on demand; options: ?preload=all  ?env=A=1,B=2  ?init=<console commands>)
# the gateway, the multiplayer-only build and system link from the browser: docs/gateway.md
```

## What compiled

Everything the Linux build compiles. That means all of `source/` plus the platform layer in `port/linux/src`,
apart from 5 desktop-only files (`posix_update.c`, `posix_upnp.c`, `updater.c`, `memory_watch.c`, `p2p_discord.c`).
Web replacements live in `port/web/src`. The build uses the same flags as the Android build
(`-fms-extensions -fshort-wchar -fcommon -fwrapv ...`, `-DHALO_ANDROID=1 -DHALO_WEB=1`), so the web build reuses
the OpenGL ES 3.0 renderer path and the Android fixes for variadic prototypes.

- `tools/web_build.py`: the `ninja web` target, called from `tools/project_x86.py`. Link flags:
  `--use-port=port/web/ports/sdl3_pinned.py -sMIN/MAX_WEBGL_VERSION=2 -sFULL_ES3 -sPROXY_TO_PTHREAD -sJSPI -sOFFSCREENCANVAS_SUPPORT
  -sOFFSCREENCANVASES_TO_PTHREAD=#canvas -sPTHREAD_POOL_SIZE=8 -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB
  -sINITIAL_MEMORY=256MB -sSTACK_SIZE=4MB -lwebsocket.js`. SDL 3 is an external Emscripten port pinned to the
  Android and Windows builds' release (3.4.16, checked by SHA512) rather than the SDK's `-sUSE_SDL=3` (3.4.2,
  whose gamepad code fails on the game's worker thread).
- `tools/web_abi_shims.py`: wasm, unlike x86, traps when a call's signature differs from the callee's.
  The script reads wasm-ld's "function signature mismatch" warnings and generates 32 adapters
  (`port/web/src/web_abi_shims.c`) plus 38 per-file `-D` renames (`port/web/web_abi_shims.json`).
  Most of them are implicit-`int` callers of `void` functions. Two special cases: `time` (32-bit `time_t` callers)
  and `error` (a variadic function called without a prototype). You run link → regenerate until there are no
  warnings (4 rounds).
- Source changes, all behind `#ifdef HALO_WEB`:
  - `source/interface/terminal.c:323`: builds a `va_list` instead of treating a `char *` as one.
  - `source/cache/cache_files_windows.c:1074,1155`: a thread-start adapter. `CreateThread` received
    `cache_file_windows_thread_proc` through a function-pointer cast, and wasm checks indirect-call signatures.
  - `source/cseries/stack_walk_windows.c:763`: no frame walking (it read out of bounds).
  - `port/linux/src/xbox_memory.c:23–246`: the Xbox's physical window at 0x80000000–0x88000000 is reserved by
    moving the `sbrk` break into it. Commit is a memset. There is no mmap/mprotect. `MAXIMUM_MEMORY=4GB` makes the
    upper half addressable.
  - `port/linux/src/xbox_kernel.c:649`: `GetTickCount` counts from start-up. Emscripten's monotonic clock is
    epoch-based, so as a `DWORD` it went negative in a `long`. The sound manager's gain then blew up and hit an
    assertion at `sound_dsound_xbox.c:980`.
  - `port/linux/src/msvc_crt.c:298`: `_control87` without the ARM FPCR builtins.
  - `port/linux/src/gl_functions.c:21`: `glCopyImageSubData` and `glDrawElementsBaseVertex` are optional.
  - `port/linux/src/d3d8_gl.c:1452`: see "blockers" 1.
  - `port/linux/src/d3d8_gl.c:3419`: see "blockers" 2.
  - `port/linux/src/xbox_textures.c:592,630`: WebGL has no `GL_TEXTURE_SWIZZLE_*`, so red and blue are swapped
    on the CPU before `GL_RGBA` uploads.
  - `port/linux/src/sdl_platform.c:572,641`: requests an opaque canvas (the idea comes from upstream PR #12), logs
    the frame rate every 5 s, and hooks a stall watchdog (`HALO_WEB_WATCHDOG=1`).
  - `port/linux/src/posix_net.c:75–417`: the real socket layer is excluded, and `port/web/src/posix_web_net.c`
    replaces it (see "Networking").
- New web platform files: `web_host.c` (host_gl_* for WebGL2, extension aliases, `glBufferSubData` in place of
  mapping, no-op fences, watchdog), `posix_web_host.c` (getrandom, UPnP stubs), `web_memory_watch.c`,
  `posix_web_net.c` + `web_library.js` (sockets over a WebSocket), and `shell/play.html` + `shell/halo-loader.js`
  (Module config, map preload into MEMFS, env/init from the URL).

## What blocked, and how each was resolved or remains

Resolved during the spike (these are the traps anyone repeating this will hit):

1. **Frames stopped after about 10.** `D3DDevice_GetVisibilityTestResult` (`d3d8_gl.c:1415`) returns
   `D3DERR_TESTINCOMPLETE` until `GL_QUERY_RESULT_AVAILABLE`. The game spins on it
   (`rasterizer_xbox_widgets.c:233`, `rasterizer_xbox_transparent_geometry.c:845`). WebGL only makes query results
   available after control returns to the event loop, so the spin never ends. Fix: answer with the slot's last
   result, and report "visible" until the first result arrives. The game reads each test in the frame that made
   it, so the result that can be there is an earlier frame's: the slot keeps its earlier query until it is read
   (`D3DDevice_EndVisibilityTest`). Before that, no result ever arrived and every lens flare and glow was drawn,
   even behind the first-person weapon. Occlusion is a frame or two late on web.
2. **No menus/text.** Immediate-mode draws (`D3DDevice_End`, `d3d8_gl.c:3409`) interleave 16 vec4 attributes, a
   256-byte stride. WebGL caps `vertexAttribPointer` strides at 255, so the draws failed with `INVALID_VALUE` and
   `INVALID_OPERATION`. Fix: upload each attribute in its own block (stride 16).
3. **Nothing presented without JSPI.** An OffscreenCanvas on a pthread only presents when the thread yields.
   `SDL_GL_SwapWindow` yields via `emscripten_sleep(0)` when JSPI or ASYNCIFY is on. JSPI is used: no code-size
   cost, and it works in Chrome and Firefox. Safari still needs an `-sASYNCIFY` build (PR #12 ships both).
4. **Indirect-call signature traps** (see the shims above). This risk class stays open: any remaining
   function-pointer cast only traps when its path runs. The fallback is `-sEMULATE_FUNCTION_POINTER_CASTS`
   (slower), or extending `web_abi_shims.py` to scan the IR for cast function pointers the way
   `tools/android_abi_check.py` does.

Still open (with the evidence):

5. **CPU writes the renderer cached** (`port/web/src/web_memory_watch.c`), fixed. Native builds find guest memory
   the CPU rewrote through page protection (`memory_watch.c`, mprotect/SIGSEGV); wasm has neither, and the web
   watch only saw file reads. The vertex mirror (`d3d8_gl.c`, `mirror_range`) uploads vertex data once and
   reuses it while no write is seen, so in the browser it drew stale vertices: the first-person weapon's
   triangles across the screen (the "nearly black world"; with `rasterizer_first_person_weapon_far_clip_distance
   0.001` the world drew fine) and garbled small text (text quads come from the dynamic vertex rings). The world
   itself (BSP, lightmaps, sky, fog) matched the native client on a fixed camera. Fix: the mirror is off in the
   browser (draws stream their vertices; no slower in SwiftShader), and every Direct3D lock announces a write to
   what it covers (`d3d8_resources.c`), so textures the game writes through locks are uploaded again.
6. The `HALO_TEXTURE_NO_CACHE=1` path shows wrong colours on some DXT textures (the green ring in
   `wasm_profile_text_nocache.png`). It is a debug path, but it points at the upload path for re-created
   compressed textures.
7. Clicking the canvas (focus / pointer lock) resized it to the page and went black. `SDL_SetWindowRelativeMouseMode`
   and the canvas CSS size need handling: resize the OffscreenCanvas on the game thread, keep 4:3 letterboxing.
   Pointer lock itself: SDL's Emscripten relative mode cannot lock from the game's worker (a browser locks only
   during a click or key handler), and the web build, which also defines `HALO_ANDROID`, never asked for it. The
   page locks the canvas on a click while the game wants the mouse (`halo-loader.js`, "Pointer lock"; the game
   says so through `web_mouse_capture`); SDL then reports `movementX/Y` as relative motion.
8. Audio: SDL3's Emscripten backend uses the deprecated ScriptProcessorNode on the page thread. It works (the menu
   music plays), but it glitches under load and needs a user gesture to start. Move to an AudioWorklet reading a
   SharedArrayBuffer ring.
9. Bink movies are unsupported (the same is true on Linux). The intro skips.
10. File IO: maps are fetched whole into MEMFS before `main` (14–24 MB each; all of `root-mp/maps` is about
    250 MB of RAM). `createLazyFile` cannot be used (synchronous binary XHR is forbidden on the page thread).
    Plan: WasmFS with a fetch/OPFS backend, reading on demand with Range from `/maps`. PR #3's import tool fills
    OPFS from the user's disc image. Saves and profiles (`/home/web_user/.local/share/halo-linux`) need
    OPFS/IDBFS persistence.
11. `p2p_thread` busy-polls `GetTickCount` even with internet play off. That burns a core. Skip the thread on web.
12. There are no GPU fences on WebGL, so the triple-buffered stream ring relies on `glBufferSubData` implicit sync.
    Adopt PR #12's batching of stream `bufferSubData` calls (they measured 6–8× fps).

## Estimated remaining work (one engineer familiar with the port)

| area | state | estimate |
|---|---|---|
| Build | `ninja web` works; the ABI shim generator exists | 1–2 d: CI job, release/`-O3`/`-flto` flags, ASYNCIFY (Safari) variant, wasm-opt, cast audit |
| GL → WebGL2 | ES 3.0 path works: menus render; no copy_image/border clamp/base vertex/atomic counters | 4–6 d: CPU-write texture invalidation (5), no-cache DXT path (6), audit other >255 strides & BGRA paths, stream batching, in-game maps (b30, bloodgulch) visual pass, context loss |
| Audio | SDL3 ScriptProcessor, works | 2–3 d: AudioWorklet backend, user-gesture resume, latency tuning |
| Input | keyboard works via SDL3 | 2–3 d: pointer lock + relative mouse, canvas resize (7), gamepad API through SDL3, key capture (Esc/Tab) |
| File IO | MEMFS preload | 3–5 d: WasmFS fetch/OPFS backend with Range, PR #3 importer integration, persistent saves, cache headers |
| Threads / main loop | PROXY_TO_PTHREAD + JSPI; game loop unchanged; 8 workers | 1–2 d: drop p2p thread, size pool, verify no thread blocks the page thread, Safari ASYNCIFY build |
| Networking | client socket layer over WebSocket written (`posix_web_net.c`), untested | 4–6 d: Go gateway + Laravel tokens/server list + end-to-end test with the dedicated server; +5 d WebRTC later |
| Performance | 20–70 fps on software GL for the menu | 3–5 d: stream batching (PR #12), -O3/LTO, profile an in-game map on real GPUs; memory budget (maps out of RAM) |

Total: roughly 4–6 engineer-weeks to a playable multiplayer client in Chrome/Firefox, and 1–2 more for Safari
and polish.

## Prior art

- **Upstream PR #3** (`pr-3` locally): `WEB_PORT_PLAN.md` lays out 8 lanes (build, memory at 0x80000000 with
  4 GB wasm memory, GLES "300 es", SDL, OPFS, threads, assets, release). `port/web/` has an in-browser XISO
  importer (`xiso.js`) that copies a disc image into OPFS `halo-data/`, and a dev server that checks cache headers.
  This spike follows the plan's memory and GL choices. The importer is the right front end for step 10.
- **Upstream PR #12** wraps bnunu's prebuilt "Apollo" build (a SHA-pinned halo.js/wasm; JSPI by default,
  `?build=asyncify` fallback). It is not from source, but it proves the same codebase runs under Emscripten +
  JSPI + WebGL2. Two of its patches are worth porting: `opaque-canvas.js` (alpha:false, done here through SDL) and
  `stream-batch.js` (batch `bufferSubData` into 16/2 MiB ring buffers: 3.5 → 21–29 fps on the b30 intro on an
  M-series Mac).

## Networking in the client

The port's socket layer is `posix_socket_*` in `port/linux/src/posix_net.c` (under `xnet.c`, the Xbox Winsock/XNet
emulation the game calls). XNet addresses are plain IPv4: a host's `XNADDR` carries its real address
(`xnet.c:10`). System link uses UDP 5150 (server) and 5151 (client) (`NETWORK_GAME_SERVER_PORT/CLIENT_PORT` =
0x141E/0x141F), UDP broadcast for the game search, and the reliable connection endpoint.

`port/web/src/posix_web_net.c` replaces the socket block on web. Every socket is virtual:

- Traffic between the page's own sockets (127.0.0.1 or its own address) is delivered locally, so a player can
  host a local game.
- Everything else goes as frames over **one WebSocket** to the gateway (`web_library.js` owns the WebSocket on
  the game's worker and keeps inbox/outbox queues; `select` never blocks).
- Frames (one per WebSocket binary message, big-endian, addresses in network order):

  | type | client → gateway | gateway → client |
  |---|---|---|
  | 1 UDP | dst ip4, dst port2, src port2, payload | src ip4, src port2, dst port2, payload |
  | 2 OPEN | stream4, dst ip4, dst port2, src port2 | inbound: stream4, src ip4, src port2, dst port2 |
  | 3 OPENED | stream4, ok1 | stream4, ok1 |
  | 4 DATA | stream4, payload | stream4, payload |
  | 5 CLOSE | stream4 | stream4 |
  | 6 HELLO | – | address ip4 (the client's assigned address) |
  | 7 LISTEN (to add) | proto1, port2 | – |

- Configuration: `HALO_WEB_GATEWAY` (the `wss://…/gateway` URL) and `HALO_WEB_ADDRESS`. The page passes both
  from the Laravel `/play` view through `Module.ENV`. The loader turns off internet play (p2p/UPnP/brokers), the
  updater and clipboard invites on web.

## Gateway design

### Where it runs, and addressing

The gateway runs **inside the daemon container**, next to the game servers, because the servers listen on
per-server loopback addresses (for example `127.0.1.N:5150/5151`) that only exist in that network namespace.

Addressing uses real loopback addresses, so that nothing in the game's packets needs rewriting:

- Each server keeps its loopback address as its identity. Its `network.address` is `127.0.1.N`, so the `XNADDR`
  in its discovery answers says `127.0.1.N`. The browser sends to that address unchanged: the web socket layer
  treats only 127.0.0.1 and its own address as local, so `127.0.1.N` goes to the gateway.
- Each browser session gets its own loopback address from `127.64.0.0/16` (up to 65 k sessions), announced in
  HELLO. The whole of 127/8 routes to `lo` on Linux, so the gateway can `bind()` any of these without
  `NET_ADMIN`. For that session the gateway opens a real UDP socket on `127.64.a.b:5151` (plus whatever source
  ports the client uses) and makes TCP connections from `127.64.a.b`.
  Servers therefore see an ordinary LAN client whose packet source address matches the `XNADDR` it sent, and
  their replies to `127.64.a.b:5151` come back to the gateway's socket and go down the right WebSocket.
  (The `100.64.x.y` scheme from the brief works too, but only if those addresses are added to `lo`, which needs
  NET_ADMIN, or if the gateway rewrites `XNADDR`s inside game packets. The loopback scheme avoids both.)
- Discovery: a broadcast frame (dst 255.255.255.255:5150) from a browser is **fanned out as unicast** to the
  5150 port of every server the session's token allows. Answers come back to the session socket like any reply.
  The browser UI lists servers from the Laravel API (`GET /api/servers`: name, map, players, address). The
  in-game system-link list still works through the fan-out. A "join" from the web UI can launch with
  `?init=connect 127.0.1.N`, or filter the list.
- TCP (reliable endpoint): OPEN → `connect()` from `127.64.a.b:<src>` to the server; DATA/CLOSE are relayed.
  Inbound connections (a server connecting back) are accepted on listeners the gateway opens for the session
  after LISTEN (frame 7), or lazily on 5150/5151.

### Process

**Recommended: a small native Go service (`halo-gateway`)**, one static binary started and supervised by the
daemon (or as a sidecar process in the daemon container).

- Go gives one goroutine per session and per socket, easy `net.ListenUDP`/`DialTCP` with explicit local
  addresses, a mature WebSocket library (`nhooyr.io/websocket` or `gorilla/websocket`), and **pion/webrtc** for
  the later WebRTC step. It adds no runtime dependency to the container.
- Rust (tokio + webrtc-rs) would work equally well. A C gateway would repeat the game's own socket code, but
  WebSocket, TLS-less HTTP and WebRTC in C add a lot of surface.
- PHP/ReactPHP run by the daemon is possible for WebSocket↔UDP, but binding hundreds of per-session sockets,
  per-packet work at 30–60 pps per player, and WebRTC (no maintained PHP stack) argue against it. Keep PHP for
  control only (tokens, the server list, starting/stopping servers).

State: `sessions[token-sub] = {addr 127.64.a.b, ws, udp sockets by local port, tcp streams by id, allowed servers}`.
It exposes a local control endpoint (unix socket or 127.0.0.1 HTTP) where the daemon registers and unregisters
servers (`id → 127.0.1.N`) and reads metrics (sessions, pps, bytes).

### Auth

- Laravel issues a **signed, short-lived join token** when the user opens `/play` or clicks Join:
  `base64url(payload).base64url(HMAC-SHA256(secret, payload))`, with payload
  `{sub: user id, sid: random session id, srv: [server ids or "*"], exp: now+60s, nonce}`. The HMAC secret is
  shared with the gateway through the daemon container's environment. Ed25519 is an option if the gateway
  should not be able to mint tokens.
- The token goes in the WebSocket subprotocol header (`Sec-WebSocket-Protocol: halo.v1, <token>`) or in the
  first frame, **not in the query string**, so it never lands in nginx access logs. The gateway checks the
  signature, `exp`, and single use of `nonce`/`sid` within the expiry window, then sends HELLO.
- The gateway only fans out and connects to servers in `srv` and in its registry, and only to ports 5150/5151.
  The token is therefore not a general relay into the container's loopback. Limits: frames per second and bytes
  per session, one session per `sid`, idle timeout.

### TLS and routing

The web container's nginx terminates TLS and proxies `wss://host/gateway` over the internal Docker network to
the gateway's plain HTTP port. That port is never published on the host.

```nginx
location = /gateway {
    proxy_pass http://daemon:7780;           # the gateway's WebSocket listener
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_read_timeout 3600s;  proxy_send_timeout 3600s;
    proxy_buffering off;
}
```

The `/play` page keeps its COOP/COEP headers (`same-origin` / `require-corp`). `/maps` needs
`Cross-Origin-Resource-Policy: same-origin`, Range support and long-lived caching. The WebSocket is same-origin,
so COEP does not affect it.

### WebRTC later

WebSocket over TCP brings head-of-line blocking under loss. On LAN-quality links the game copes (its own
reliability runs on top), but on lossy mobile links unreliable transport is better. Phase 2:

- Signalling runs over the same authenticated WebSocket (SDP offer/answer + ICE).
- There are two DataChannels: `unreliable` (`ordered:false, maxRetransmits:0`) for frame type 1 (UDP), and
  `reliable` for types 2–5. The frame format stays the same, so `posix_web_net.c` does not change and only
  `web_library.js` picks the channel.
- pion runs in the gateway with a single UDP port muxed for all peers (e.g. 3478/udp or 443/udp, published on
  the host), plus a TURN fallback (coturn or pion/turn) for restrictive networks. The WebSocket remains the
  fallback transport.

### Suggested next steps for networking

1. Write the Go gateway (about 600 lines): WebSocket accept + token check, session loopback allocation, UDP
   relay, broadcast fan-out, TCP streams, and the daemon registration endpoint.
2. Laravel: a `/api/servers` list and a join-token endpoint; pass `HALO_WEB_GATEWAY`/token to `/play`.
3. End-to-end test in the daemon container: the dedicated server on `127.0.1.1`, headless Chrome client →
   gateway → join bloodgulch. The native `debug.network_test` hooks can drive the client.

## Go / no-go

**Go.** The hardest unknowns are now answered positively:

- The 32-bit decomp compiles to wasm32 with only a handful of source edits and generated ABI adapters.
- The Xbox memory window at 0x80000000 works in a 4 GB wasm heap.
- The existing threads and blocking main loop run unchanged under PROXY_TO_PTHREAD + JSPI.
- The Android GLES 3.0 renderer maps onto WebGL2 with small, local fixes.
- System link can be carried over a WebSocket without changing the game's network code, because the port's
  socket layer is a clean seam.

The risks that remain are known and bounded: CPU-written texture invalidation, undiscovered function-pointer
casts, audio latency, memory use of whole maps (fixed by on-demand FS), and Safari (an ASYNCIFY build).
