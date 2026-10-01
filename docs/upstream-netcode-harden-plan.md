# Upstream netcode hardening: review and porting plan

Repo: `damiantw/halo-ce-web-multiplayer` (our main `349953a3`, `HALO_PORT_NETWORK_VERSION 4`).
Upstream: `cybersecurity/halo-ce-universal` main `c55e4e2b` (merge-base `0ef2ed7d`).
Scope reviewed: `e7d1ed3a` .. `71ae1f47` (net version 4 to 8), `c5fcfbd4` (v9), `083eef0c`, `8fb1647e`.
Analysis only. No code has been changed for this.

## What we already have
- Lockstep removal (distributed netcode) ported HALO_LINUX-side (old repo #31/#32, `e7adc0cf`).
- `aad8719f` client flags in the second game (`343e7dbc`).
- **Hit-report validation from e7d1ed3a only** (`34fe0f5f`). The rest of e7d1ed3a is not ported.
- Our own pieces that upstream lacks and that these commits touch:
  - late-joiner roster ordering and the profile.c guard
  - `9325cb33` input reset
  - duplicate-name numbering `030b8c0b` and `HALO_WEB_PLAYER_NAME`
  - the dedicated control channel (laravel), auto_join, variant overrides
  - the Go gateway with WebSocket/WebRTC (`port/gateway`)
- Hard rules: never take `4adc3a87`, `a05253c1` (lock to known map builds), or upstream's join/silence timeouts. Keep the `cseries.h` HALO_LINUX guard.

## The commits

| commit | net ver | what it does | for us |
|---|---|---|---|
| `e7d1ed3a` (56 files, +4871/-1915) | 5 | The client clock starts from the host's first update. Own-player reconciliation against the host tick. Strict hit validation. Game-type event counters, so clients re-announce missed CTF/oddball/race/king events. One datagram per tick (`_distributed_message_batch`). Join timeout 10 s, client silence 15 s, late joiner 120 s, playerless machine 15 s. Closed to joins while loading. Large p2p/xnet/UPnP/Discord/crypto work. CTF/king/oddball/race/slayer engine rewrites. Telnet changes. | High value: clock start, reconciliation, event counters, batching. Hit validation is done. **Timeouts: exclude.** p2p/xnet/Discord/UPnP: skip. |
| `0d548798` (49 files, +4689) | 6 | Host anchor/tolerance checks on client moves. Default-stripped unit state (35 vs 64 B) and delta input. Fills datagrams. Per-relevance object correction rates (25/60/120 units). Damage and pickups go only to the machines concerned. Hits stamped with the host tick, with 1 s of position history; reports older than 3 s refused; flood cost. Vehicle prediction correction. Statistics/gametype state sent on change. Also p2p, telnet port 2323, cache_files/players/ui_widget/Android. | High value: bandwidth drops a lot (helps WebRTC/WS relays and many-player servers), plus move validation. Large and invasive. |
| `4f3ce00a` | (6) | Anchor refinements. Gametype state at most 2.5/s, sent reliably. p2p proof of key. | Take with 0d548798, minus p2p. |
| `671cc9ed` | (6) | Foot speed cap (2 world units/tick). Melee/collision shape checks. Damage after a client leaves. Actor kills. p2p rate limits. | Take with 0d548798, minus p2p. |
| `e233e1c1` | 7 | Stricter vertical/fall/jump bounds (anti-hover). The killing blow carries the killer's score; body state includes "dead". Telefrag messages. Projectile reach history 13 s. Explosion direction/scale checks. Unreliable killing blow. 64-hex invite links. Oddball pass event. | Anti-cheat and correctness: take. Invite links: skip (p2p). |
| `a6ca914b` | (7) | In-air height caps for players and non-flying vehicles. p2p broker rate limits. `sdl_platform.c`. | Take the caps; skip p2p. |
| `71ae1f47` | 8 | Clients play by the host's rules: `cheats_network_client_enforce` limits console/telnet/cheat commands to cosmetic ones, resets game speed/cheats/autoaim and rasterizer debug, and keeps the camera the player's own. Touches cheats.c, hs.c, game_time.c, director.c, xbox_sound_cache.c, network_connection.c, server_message_handler. | High value for web, where the console is reachable from devtools/keybinds. Mostly client-side. Check it doesn't break our dedicated-server console or control channel (those run as host). |
| `c5fcfbd4` | 9 | Speed-hack detection: checked every 2 s; >10% faster and >0.5 s ahead means predictions refused; 10 s of that means kick plus ban. `_distributed_message_notice` to all machines. `cheaters.txt`, `bans.txt` (ip=/hwid=), console `ban <name>`. Discord user. Hardware ID (HMAC of machine-id) in the join request. | Detection is valuable. **The ban storage does not fit us** (see risks). Must be adapted. |
| `083eef0c` | none | `network_game_server_clean_name()` strips control, bidi, zero-width, surrogate and `\|` characters, trims spaces, and defaults to "Player"/"Machine". Applied to queue_client_player, the join request machine name, add_player_request_pregame, settings and player_settings requests. | Take early. Low risk, no version bump. It sits near hardware_id code from c5fcfbd4 in the join handler, so it is a hand port, not a cherry-pick. |
| `8fb1647e` | none | Weapon swap crash and log spam: first_person_weapons.c, data.c, sound_definitions.c, client_message_handler, memory_watch.c crash report, plus a test pickup_weapon config. | Take early. Independent, no version bump. Skip the test config if it needs game data. |

## Overlaps and conflicts with our tree
- **network_damage / hit validation:** our `34fe0f5f` is the e7d1ed3a version. 0d548798 then reworks it (host-tick stamps, 1 s history, 3 s refusal, flood cost) and e233e1c1 again (13 s projectile reach). So port those on top of ours and resolve against `34fe0f5f`. Don't cherry-pick blindly.
- **network_server_manager.c:** this is the hot spot. Our dedicated-server code lives here: control channel hooks, auto_join, late-joiner ordering, duplicate names, variant overrides. Upstream's e7d1ed3a, 0d548798 and c5fcfbd4 add hundreds of lines here, mixed with p2p, the hardware ID and the timeouts. Expect manual hunk-by-hunk ports.
- **game_engine (ctf/king/oddball/race/slayer):** e7d1ed3a rewrites these for the event counters. Our variant overrides touch game-type settings, so test every game type.
- **network_connection.c:** ours writes the reliable stream in a blocking loop and drops a peer after `NETWORK_CONNECTION_WRITE_TIMEOUT = 2000` ms. Upstream replaced that with an outgoing queue and 15000 ms ("as long as the host waits for a silent machine"). Upstream's queue is actually **kinder to stalled web tabs** than our 2 s. Take the queue and set the write timeout to our own long value (below). There is also a dead-looking `NETWORK_GAME_CLIENT_STALL_TIMEOUT = 2000` lockstep path in our network_server_manager.c (~l.3520). Confirm it is unreachable after the lockstep removal.
- **players.c / unit update:** our `9325cb33` input reset overlaps with 0d548798's delta input. The reset must also clear the delta baseline.
- **Names:** with 083eef0c, run `clean_name` *before* our duplicate-name numbering. Check that the gateway- or laravel-provided `HALO_WEB_PLAYER_NAME` survives. `|` is stripped, and non-BMP characters (emoji) are dropped.
- **cheats/hs (71ae1f47):** the dedicated server is the host, so enforcement must never apply on the host side. Our control channel issues hs/console commands on the server; verify this. The web client's own debug keys and console get limited, which is what we want.
- **Native-only parts to skip everywhere:** p2p broker/sessions, xnet crypto, UPnP, Discord, invite links, Windows/Android bits, telnet port moves (unless we want telnet).

## Value for headless Linux servers + web clients
1. **Bandwidth (0d548798, batching from e7d1ed3a).** Fewer, fuller datagrams, stripped unit state, delta input, and relevance-based correction rates. Every web datagram crosses the gateway (WebRTC data channel or WS), so this is the biggest practical win for latency and server egress.
2. **Smoothness (clock start, own-player reconciliation, vehicle correction).** Web clients have more jitter (tab scheduling, WS fallback). Reconciliation against the host tick means less rubber-banding.
3. **Correctness (event counters, killing-blow score, dead body state, oddball pass).** Lost unreliable events stop desyncing scores and flags. This matters more on lossy WebRTC unreliable channels.
4. **Anti-cheat (anchors, speed/height caps, 71ae1f47 rules, speed-hack detection).** Web clients are trivially modifiable (devtools, patched wasm). Server-side validation is the only real defence.
5. **Name sanitising and the weapon-swap crash.** Cheap hygiene. Bidi and zero-width names are an abuse vector on public servers.

## Risks
- **Protocol bump 4 to 9 (or our own numbering).** Server and web client must ship together, in the same image or the same deploy. A cached old `halo.wasm` will be refused. So:
  - cache-bust the wasm/js URLs (versioned paths or hashes) and have the site reload on version mismatch
  - make the gateway/laravel server list show only same-version servers
  - native clients built from older trees are refused (expected)
  - recommendation: **one bump per merged slice**, but **deploy only at milestones**, or keep our own counter and not match upstream's numbers, since we will never interoperate with upstream's p2p builds anyway. Matching upstream's numbers would only matter for native interop, and that also needs all of their p2p/protocol pieces.
- **Speed-hack false positives on web.** Upstream reasons that an honest client's clock only jumps forward to the host when behind, so it is never both fast and ahead. Browser realities to test:
  - rAF/timer throttling in background tabs, then catch-up bursts
  - `performance.now()` clamping
  - a client resuming after a stall
  - a dedicated-server hitch (map load, GC-like pauses, a container CPU limit) that makes every client look "ahead"

  Start with **log-only / refuse-predictions only**, no kick and no ban, and put the thresholds in config.
- **Ban identity behind the gateway.** Every browser appears as `127.64.a.b`, from the token's `adr` claim or the next free address. An `ip=` ban therefore bans a **reusable virtual address**, and could hit a different user later. The web build has no machine-id, so `hwid` is empty or meaningless, and there is no Discord. So `bans.txt` and `cheaters.txt` are wrong for us. Instead:
  - report a speed-hack or kick event over the **dedicated control channel** with the session address
  - laravel maps the address to the token `sub` (or the gateway's `GET /sessions`) and bans the **account**
  - the gateway or laravel refuse future tokens
  - the server keeps only an in-memory per-match kick list
- **Strict move/height/speed validation vs our custom content.** Our variant overrides (e.g. gravity, speed, vehicles) can trip upstream's fixed constants (2 wu/tick foot speed, height caps). Scale the limits from the variant's settings, or gate them on a config flag.
- **Merge risk.** These are big commits (thousands of lines each) in our most customised files. Expect hand ports and long review. Keep each slice reviewable and separately testable.
- **Timeouts sneaking in.** Several commits fold timeout logic into otherwise-wanted hunks (see below).

## Keeping upstream's timeouts out
Upstream timeout code to **not** port (upstream main):
- `network_server_manager.c`:
  - `NETWORK_GAME_SERVER_JOIN_TIMEOUT` 10 s
  - `NETWORK_GAME_SERVER_CLIENT_TIMEOUT` 15 s
  - `NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT` 120 s
  - `network_game_server_client_machine_timed_out()` and its caller
  - `NETWORK_GAME_SERVER_PLAYERLESS_MACHINE_TIMEOUT` 15 s, used to refuse a joined machine with no player
- `network_game_server_network_lost()` (host interface-down grace): native and wifi oriented. Not needed in a container. Skip it.
- `network_connection.c`: `NETWORK_CONNECTION_WRITE_TIMEOUT` 15000 with the outgoing queue.

How:
1. Port the surrounding hunks, but leave out the `*_timed_out()` checks and the constants. Where code needs a constant (the write queue), define `HALO_PORT_*` limits in `halo_port_limits.h` with our own values:
   - write stall: 60 s or more
   - a "dead" peer is detected only by a TCP/WebRTC close from the gateway
2. If any silence timeout is ever needed (zombie slots on public servers), make it **opt-in config** that defaults off, and long (minutes). Count only after the gateway reports the session closed. The gateway already knows when a browser's WS or data channel is gone, and can CLOSE the TCP relay so the server drops the machine through the normal path.
3. **Join-while-loading closure** (e7d1ed3a closes joins during load): fine to take. It is not a timeout, but check it doesn't fight our auto_join retry.
4. Add a regression test to `dedicated_control_test` or `test_linux_port`: a machine silent for 30 s+ in game, and a late joiner loading for 3+ min, both stay connected.
5. In each slice PR, grep the diff for `TIMEOUT`, `last_heard_time` and `timed_out`, and list any hits in the PR body.

## Recommended PR slices (in order)
1. **`8fb1647e` weapon-swap crash/log spam.** No protocol change. Cherry-pick with fixups. Skip test data that needs maps.
2. **`083eef0c` name cleaning.** No protocol change. Hand port onto our join/settings handlers, ordered before duplicate-name numbering. Unit-test bidi, zero-width, `|` and empty names.
3. **`71ae1f47` client plays by host rules.** Host and dedicated paths untouched. Verify the control channel's hs commands and the web debug UI. (It bumps upstream to v8 because of a server_message_handler change. If we take it alone, bump to our v5.)
4. **e7d1ed3a remainder (v+1):** clock start from the host's first update, own-player reconciliation, game-type event counters, one datagram per tick, closed-while-loading. **No timeouts, no p2p.** Test every game type and late join.
   - 4a, optional and separable: upstream's reliable outgoing queue in network_connection.c, with our long write timeout replacing the 2 s blocking loop.
5. **`0d548798` + `4f3ce00a` + `671cc9ed` (v+1):** stripped unit state, delta input, datagram filling, relevance corrections, concerned-machine damage/pickups, host-tick hit history, anchors, foot speed cap. Limits scaled by variant or configurable. Measure gateway bandwidth before and after.
6. **`e233e1c1` + `a6ca914b` (v+1):** vertical/height caps, killing-blow score, dead body state, telefrag, projectile/explosion checks, oddball pass. No invite links or p2p.
7. **`c5fcfbd4` adapted (v+1):** speed-hack detection and `_distributed_message_notice`.
   - Phase 1: log plus refuse predictions only.
   - Phase 2: kick, then report over the control channel. The ban lives in laravel/gateway by account `sub`.
   - No `bans.txt`, hwid or Discord. Thresholds in config.

Versioning and deploys: bump `HALO_PORT_NETWORK_VERSION` in each protocol slice (4 to 5 to 6 ...). Merge freely, but **deploy server image + web client together**, ideally after slices 4 through 6 as one release, then 7. Add wasm cache-busting and a version-mismatch reload before the first protocol deploy.
