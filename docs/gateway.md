# The web client's gateway, and the multiplayer-only web build

The web build (`ninja web`, [wasm-spike.md](wasm-spike.md)) has no UDP or TCP. Its sockets
(`port/web/src/posix_web_net.c`) are framed onto one WebSocket to **`halo-gateway`** (`port/gateway`, Go),
which runs next to the dedicated servers and owns the real sockets. To the servers, each browser is an ordinary
system link machine on a loopback address.

It lives in this repository because the frame format is defined by `posix_web_net.c`: the two change
together, are tested together, and the daemon image builds the gateway beside the game binary
(`go build ./port/gateway`, a static binary; dependencies: `github.com/coder/websocket`, and Pion
(`github.com/pion/webrtc/v4`) for the data channels, below).

## Addressing

| who | address | notes |
|---|---|---|
| dedicated server N | `127.0.1.N:5150/5151` | `HALO_NET_ADDRESS=127.0.1.N`, `HALO_NET_BROADCAST=127.64.0.1` |
| the gateway's hub | `127.64.0.1:5150/5151` | where servers' broadcasts go; fanned out to the sessions allowed to see that server |
| browser session | `127.64.a.b` (from `HALO_GATEWAY_CLIENT_NET`, 127.64.0.0/16) | the token's `adr` claim, or the next free one; announced in HELLO |

All of 127/8 routes to `lo` on Linux, so the gateway binds these addresses without `NET_ADMIN`, and nothing
in the game's packets needs rewriting: a server sees packets from `127.64.a.b` whose `XNADDR` says `127.64.a.b`.

- A browser's broadcast (the system link search, to 255.255.255.255 or the hub) becomes unicast to port
  5150/5151 of every registered server the token allows. Answers come back to the session's socket.
- A server's broadcast to the hub goes down every session allowed to see that server.
- TCP (the game's reliable endpoint): OPEN dials from `127.64.a.b` to the server; DATA and CLOSE are relayed.
- Only ports 5150 and 5151 of registered servers are reachable: the token is not a relay into the container.

## Frames

One WebSocket message is one frame; numbers are big-endian, addresses as in `sockaddr_in`:

```
1 UDP     dst/src ip4, port2, port2, payload
2 OPEN    stream4, ip4, port2, port2
3 OPENED  stream4, ok1
4 DATA    stream4, payload
5 CLOSE   stream4
6 HELLO   ip4                 (gateway -> client: the session's address)
7 PING    token (<= 16 bytes) (echoed the way it came: WebSocket or data channel)
8 RTC     JSON                (WebRTC signaling, WebSocket only; below)
```

## WebRTC

A WebSocket is TCP: a lost packet holds up everything behind it until it is resent (the minimum
retransmission timeout is 200 ms), so the game's per-tick datagrams arrive late and in bursts rather
than not at all. Once the WebSocket is open, the page offers a **data channel** (label `halo`,
`ordered: false`, `maxRetransmits: 0`: UDP's behaviour) and, when it opens, sends and receives the
**UDP frames and PING** over it. Everything else (HELLO, the TCP streams, signaling) stays on the
WebSocket, which also keeps accepting datagrams, so a switch either way loses only what is in flight.
The join token, the addressing and the token URL are unchanged: the channel belongs to the WebSocket's
session.

Signaling is frame 8, a JSON object:

```
client -> gateway   {"type":"offer","sdp":...}          (the page makes the channel)
                    {"type":"candidate","candidate":{}} (optional; not needed by an ICE-lite gateway)
                    {"type":"bye","reason":...}         the page gave up on the channel
gateway -> client   {"type":"answer","sdp":...}
                    {"type":"bye","reason":...}         "unavailable" (no WebRTC here), or it failed
```

- **One UDP port for all sessions.** The gateway is an **ICE-lite** agent on `HALO_GATEWAY_RTC_LISTEN`
  (Pion's UDP mux: sessions are told apart by ICE user name), and its answer carries one **host
  candidate** per address in `HALO_GATEWAY_RTC_PUBLIC_IPS`, on `HALO_GATEWAY_RTC_PORT`. The browser
  connects to that; nothing else needs to be open, and no STUN or TURN server is involved. Inside a
  container the public addresses replace the container's (the port is published with DNAT, so the
  gateway still sees the browsers' own addresses).
- **The port: 3478/udp.** 443/udp is Caddy's HTTP/3 on the production host, 7780/7781 are the gateway's
  WebSocket and control API (TCP, but one number each is less confusing), and 3478 is the IANA
  STUN/TURN port: networks that let video calls through tend to allow it. A TURN server added later
  can take 5349 or 443/tcp elsewhere.
- **Worker and page.** Browsers have no `RTCPeerConnection` in workers, and the WebSocket lives in the
  game's worker (`web_library.js`). The peer connection is made on the page's main thread
  (`webrtc_main_start`, proxied) and the frames cross between it and the worker over a
  `BroadcastChannel` named for the attempt.
- **Fallback.** The page gives up when the channel is not open 4 s after the WebSocket opened (UDP
  blocked, an old gateway, `HALO_WEB_RTC=0`) or when it fails or hears nothing for 2.5 s later: it
  tells the gateway (`bye`) and sends the datagrams over the WebSocket again, and the game carries on.
  The page's main thread (not the game's worker, which can be busy loading a map) sends a keepalive
  PING (`7 'k'`) every half second, which the gateway echoes. The gateway gives up on a channel it
  hears nothing from for 3 s, on a failed connection, or on a closed channel, and says `bye`.
  A channel that did not open in time or was lost is tried again on the same WebSocket 8 s later (then
  16 s), 3 attempts in all: a lossy moment can outlast the 4 s (at 5% loss the DTLS and SCTP handshakes
  lose packets and wait out their 1 s retransmissions). Not when the gateway has no WebRTC or refused
  the offer for the limits; after the last attempt the next is on the next WebSocket connection.
- **Offers are limited.** Each offer makes a peer connection (DTLS keys, an SCTP association), so a
  session takes at most 5, one at a time and at least 2 s apart; the page makes one per WebSocket
  connection. Others are answered `bye` (`"too many offers"`, `"offer too soon"`) and the datagrams stay
  on (or go back to) the WebSocket; a new offer that is taken replaces the session's peer.
- **Behind Docker (1:1 NAT).** Publish the one port on the host's public addresses
  (`-p <public IPv4>:3478:3478/udp`, and the IPv6 one) and put those addresses in
  `HALO_GATEWAY_RTC_PUBLIC_IPS`: the answer then carries them, not the container's address, and
  Docker's DNAT delivers the browser's packets to the gateway unchanged (tested end to end: the gateway
  and a server in a container on its own bridge network, a browser on the host connecting to the host's
  address; `/workspace/rtc-e2e/docker-nat.sh`). With a different host port, set
  `HALO_GATEWAY_RTC_PORT` to it.
- **Congestion.** A frame waiting behind more than 256 KiB in a channel's send buffer is dropped,
  as a router would.
- **Observability.** `webstats_publish` adds `net.transport` (`"rtc"` or `"ws"`) for the page's
  overlay; the page's console logs `[webnet] datagrams over WebRTC (N ms)` and
  `[webnet] WebRTC lost (reason); datagrams over the WebSocket`. The gateway logs `rtc open`
  (with `setup_ms`) and `rtc closed` (reason, frames), `GET /sessions` gives each session's
  `Transport`, and `GET /metrics` `rtc_sessions`, `rtc_opened`, `rtc_fallbacks` and
  `rtc_offers_refused`.

## Join tokens

`base64url(JSON claims) "." base64url(HMAC-SHA256(secret, first part))`, claims:

| claim | meaning |
|---|---|
| `sub` | user id (logged) |
| `sid` | random session id; single use (replay cache until `exp`) |
| `exp` | unix time, at most 15 minutes ahead (60–120 s is plenty: it is only checked at connect) |
| `srv` | server ids the session may see and join, or `["*"]` |
| `adr` | optional: the session's `127.64.a.b` (lets the page pass the same `HALO_WEB_ADDRESS`) |

The browser sends it as a WebSocket subprotocol, `Sec-WebSocket-Protocol: halo.v1, t.<token>` (never in the
query string, so it stays out of access logs). A bad, expired or replayed token gets HTTP 401; a full gateway
503. On a dropped connection the client asks `HALO_WEB_TOKEN_URL` (POST, same origin, answered with
`{"token": "..."}`) for a new token and reconnects (at most 5 attempts).

Development tokens: `HALO_GATEWAY_SECRET=... halo-gateway -mint web1 -mint-address 127.64.0.2 -mint-servers gulch`.

## Running it

Environment (defaults in brackets):

| variable | |
|---|---|
| `HALO_GATEWAY_SECRET` | HMAC key, at least 16 bytes (required) |
| `HALO_GATEWAY_LISTEN` [127.0.0.1:7780] | WebSocket at `/gateway`, plus `/healthz` |
| `HALO_GATEWAY_CONTROL` [127.0.0.1:7781] | the daemon's API (below) |
| `HALO_GATEWAY_SERVERS` | initial registry, `id=127.0.1.1,id2=127.0.1.2` |
| `HALO_GATEWAY_CLIENT_NET` [127.64.0.0/16], `HALO_GATEWAY_HUB` [127.64.0.1] | addressing |
| `HALO_GATEWAY_MAX_CLIENTS` [256], `HALO_GATEWAY_MAX_FRAME` [65536] | limits (a larger frame closes with 1009) |
| `HALO_GATEWAY_FRAME_RATE` [600/s], `HALO_GATEWAY_BYTE_RATE` [2 MiB/s] | per-session token buckets (burst 2x); excess frames are dropped and counted |
| `HALO_GATEWAY_LOG_LEVEL` [info] | JSON logs (slog): session opened/closed with sub, sid, address, frame counts, drops, reason |
| `HALO_GATEWAY_RTC_LISTEN` [empty: off] | the WebRTC UDP address, e.g. `0.0.0.0:3478` |
| `HALO_GATEWAY_RTC_PUBLIC_IPS` | comma-separated addresses announced as host candidates (IPv4 and/or IPv6); empty: the interfaces' |
| `HALO_GATEWAY_RTC_PORT` [the bound port] | the port announced, when a port mapping changes it |

Also per session: 16 streams, 8 UDP source ports, 60 s idle timeout, 5 s dial timeout.
SIGINT/SIGTERM closes every session with 1001 and exits; a second signal exits at once.

Control API (`HALO_GATEWAY_CONTROL`, loopback only):

```
GET    /healthz
GET    /servers                       {"gulch": "127.0.1.1", ...}
PUT    /servers/{id}  {"address": "127.0.1.N"}
DELETE /servers/{id}
GET    /sessions                      address, sub, servers, frames, drops per session
GET    /metrics                       counters
```

Tests: `cd port/gateway && go test -race ./...` (frames, tokens, registry, buckets, bad tokens, address
allocation and the client limit, discovery fan-out and hub broadcasts over real UDP, streams, limits, shutdown;
WebRTC with a Pion client: the answer's candidates, datagrams and PING both ways over the channel, streams
refused on it, and the fallbacks: a closed channel, a silent one, the page's `bye`, no WebRTC, a bad offer;
the offer limits).

## In the Laravel daemon container

1. **Gateway**: the daemon starts `halo-gateway` once (supervised, restart on exit) with
   `HALO_GATEWAY_SECRET` (shared with Laravel through the container environment) and
   `HALO_GATEWAY_LISTEN=0.0.0.0:7780` on the internal network only (or 127.0.0.1 if nginx runs in the same
   container). It is not published to the host.
2. **Servers**: each dedicated server starts with `HALO_NET_ADDRESS=127.0.1.N` and
   `HALO_NET_BROADCAST=127.64.0.1`; when it is up the daemon calls `PUT /servers/{id} {"address":"127.0.1.N"}`,
   and `DELETE /servers/{id}` when it stops. (`HALO_GATEWAY_SERVERS` seeds the registry at start.)
3. **Token endpoint** in Laravel, for a signed-in user (for example `POST /play/token`, CSRF-protected):

   ```php
   $claims = ['sub' => (string) $user->id, 'sid' => Str::random(20), 'exp' => time() + 90,
              'srv' => [$server->gateway_id], 'adr' => $address];   // $address: 127.64.a.b, unique per live session
   $body = rtrim(strtr(base64_encode(json_encode($claims)), '+/', '-_'), '=');
   $sig  = rtrim(strtr(base64_encode(hash_hmac('sha256', $body, config('halo.gateway_secret'), true)), '+/', '-_'), '=');
   return ['token' => "$body.$sig", 'address' => $address];
   ```

   (Omit `adr` to let the gateway pick; then leave `HALO_WEB_ADDRESS` unset and the client takes the HELLO
   address, which is slightly racy at start-up, so pinning is preferred.)
4. **/play** serves `port/web/shell/play.html` + `build/web/halo.{js,wasm}` with
   `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp` (and CORP on the
   maps), and passes the settings in the query string or by writing them into the page:
   `?env=HALO_WEB_GATEWAY=wss://<host>/gateway,HALO_WEB_TOKEN=<token>,HALO_WEB_ADDRESS=<address>,HALO_WEB_TOKEN_URL=/play/token`.
   Add `HALO_WEB_PLAYER_NAME=<name>` (URL-encoded UTF-8, up to 11 characters, no commas: they separate the
   settings) to name the player and its
   machine; without it the profile's name or a random one is used. Add `HALO_WEB_JOIN=first` to join the
   first game found by itself (`port/linux/game/auto_join.c`): with a token limited to one server, that server.
   Add `HALO_WEB_PLAYER_COLOR=<0-17 or name>` (white, black, red, blue, gray/grey, yellow, green, pink,
   purple, cyan, cobalt, orange, teal, sage, brown, tan, maroon, salmon: `profile_color_table`'s order) for the
   player's armour colour in free-for-all games (`web_host.c web_player_color`, applied in
   `player_ui_get_active_player_profile`; team games still colour by team). Add `HALO_WEB_PLAYER_TEAM=red|blue` (else auto) to ask for a team in team games (the server honours it while the teams stay within one player: `port/linux/README.md`, "Teams"). Add `HALO_WEB_EXIT_URL=<url>` for
   where the page goes when the player leaves the game (below); `HALO_WEB_MENUS=1` brings the menus back.
   The loader sets `window.haloFeatures` (`webJoin`, `playerColor`, `playerTeam`, `leave`) so the page can tell builds that
   know the settings.
   `/maps/index.json` lists `[{name, size}]`, and `/maps/<name>.map` serves the files with Range support. For the
   multiplayer-only build it needs only `ui.map` and the multiplayer maps.
5. **nginx**:

   ```nginx
   location = /gateway {
       proxy_pass http://daemon:7780;
       proxy_http_version 1.1;
       proxy_set_header Upgrade $http_upgrade;
       proxy_set_header Connection "upgrade";
       proxy_set_header Host $host;
       proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
       proxy_read_timeout 3600s;
       proxy_send_timeout 3600s;
       proxy_buffering off;
   }
   ```

   The gateway accepts any Origin; restrict it in nginx (`if ($http_origin != "https://<host>") { return 403; }`)
   if the site wants that.

## The multiplayer-only web build

`ninja web` defines `HALO_MULTIPLAYER_ONLY` (`tools/web_build.py`; `python3 configure.py --web-campaign`
builds with the campaign). Everything is under `#ifdef`, so the byte-matching build and the native ports are
unchanged. The web multiplayer-only build is **multiplayer only, with no menus**: the site picks the name,
colour and server, and the game only joins and plays.

- **No main menu.** `main_screen_shell_load` (`ui_widget.c`) only initialises the file system on the first
  load (no intro movie, no main menu, no menu music), and the auto-join (`auto_join.c`, forced to
  `HALO_WEB_JOIN=first` with a 0.5 s settle) joins the game its token allows as soon as the lobby browser
  sees it: the page goes from loading straight to the server's lobby.
- **Leaving goes back to the site.** Every later return to the main menu (Quit in the pause menu, B in the
  lobby, a lost connection, a refused join, an error) calls `web_leave` instead, as does finding no game
  within 45 s. JS gets a cancelable `halo:leave` event on `window` with `detail = {reason: "left" |
  "no_game", errorCode}` (`web_library.js web_leave_game`, once per page); without a handler that calls
  `preventDefault()`, the loader goes to `HALO_WEB_EXIT_URL` (or shows "You left the game").
- **No split screen, profiles or settings.** Only the first controller joins the lobby
  (`netgame_join_player`) and only gamepad port 0 is reported (`xinput_sdl.c`); the profile and settings
  screens are unreachable without the main menu. The pause menu has only Resume and Quit (its tag). The
  developer console (`` ` ``) is left in.
- The main menu's Campaign item and the multiplayer menu's Co-op item are not made
  (`ui_widget.c ui_widget_multiplayer_only_hidden`), for `HALO_WEB_MENUS=1`.
- `main_set_map_name` (the one way into a local game: the campaign menus, the `map_name` console command,
  command-line levels) refuses any map whose cache header is not a multiplayer scenario, as the dedicated
  server's rotation does, and logs `'levels\a10\a10' is not a multiplayer map: this build is multiplayer only`.
  System link games load their map through `network_game_create_game_objects`, unaffected.
- So the page needs only `ui.map` plus the multiplayer maps.
- The dedicated server reports each player's colour (`"color": "orange"`, or `null`) in its player rows and
  `player_joined` / `player_left` events (`port/linux/README.md`).

## Maps on demand

The loader (`port/web/shell/halo-loader.js`) fetches only `ui.map` before the start (`?preload=a,b` or
`?preload=all` to change that). Any other map is fetched when the game first wants it
(`Module.haloFetchMap`, called from `port/web/src/web_host.c`): a system link client starts downloading the
host's map as soon as the pregame lobby names it (`network_game_globals.c`) or the join's non-blocking precache
asks for it, and the blocking precache at game start waits for the download (`cache_files_windows.c`).

## Status (end to end on the box)

Dedicated server (native, `127.0.1.1`, Blood Gulch slayer, minimum 1 player) + `halo-gateway` + the web build
in headless Chrome through `port/web/serve.py`: the browser finds the server in the system link search via the
fan-out, joins over the relayed TCP stream, the server logs `player_joined` (address `127.64.0.2`) and
`game_started`, the client downloads bloodgulch.map on demand and plays (HUD, motion tracker, the netcode's
per-second ticks). Two browser clients (`127.64.0.2`, `127.64.0.3`) in the same game see each other move.
Screenshots and logs: `/workspace/halo-data/shots/gateway_*`.

Fixed on the way:

- The in-game hang at map load was a WebAssembly trap: `weapon_preprocess_node_orientations(long)` sits in the
  object type table as a `(long, real_orientation *)` callback, and wasm traps an indirect call through a
  mismatched signature (x86 does not care). A web-only adapter (`weapons.c`, `object_types.c`) fixes it. The
  trap killed the game worker silently, which looked like a stall.
- `select()` with a timeout now waits: on the WebSocket's thread with `emscripten_sleep` (JSPI) so frames
  arrive (the game's blocking TCP connect needs that), on other threads with `nanosleep` (no busy-wait).
- Audio is mixed on a game thread and pushed to SDL, instead of in a callback on the page's thread.

Remaining gaps:

- In-game rendering in the browser now matches the native client (pairs in
  `/workspace/halo-data/shots/render_native_*.png` and `render_web_*.png`; causes and fixes in wasm-spike.md,
  items 1 and 5). WebGL's refusal to mix constant colour and constant alpha blend factors (the plasma weapons'
  meters) is worked around in `apply_raster_state`.
- Two clients in one tab set are memory heavy (~0.8 GB each in headless Chrome with SwiftShader).
- The canvas-goes-black-on-click report did not reproduce in headless Chrome (no pointer lock there); it needs a
  real browser to look at.
- WebRTC data channels carry the datagrams when UDP 3478 reaches the gateway (above, "WebRTC"); there is no
  TURN yet, so networks that block it stay on the WebSocket.
