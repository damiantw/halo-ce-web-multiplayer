# Upstream "Remove the lockstep netcode" (cybersecurity/halo-ce-universal a3ed50c5) — review and plan

Written 2026-09-30 for damiantw/halo-ce-universal (our fork) + laravel-halo (dedicated servers, Go gateway, WebRTC).
**Status (2026-09-30, the PR that adds this document):** option B, steps 1-3 in part:
- a3ed50c5 (remove lockstep): cherry-picked (`-x`) onto our `#ifdef HALO_LINUX` tree. Upstream's changes
  go in the HALO_LINUX branches, and the Xbox (non-HALO_LINUX) branches keep the original code.
  `network_game_server_close_game` stays: our `network_game_server_close_ended_game` uses it.
- aad8719f (CTF flags in the second game): cherry-picked, without the network test harness parts.
- e7d1ed3a: the host's hit-report validation only (plus the effect tag structs it needs and the
  `damage.c` no-player kill guard). Not taken yet: reconciliation, event counters, batching,
  and the join/silence timeouts.
- 4adc3a87 (HALO_LINUX undefinition): **not taken**. `cseries.h` now refuses to compile without
  HALO_LINUX, and `tools/test_linux_port.py` (CI) checks the prefixes still define it.
- laravel-halo: `lockstep` removed from `GameServer::NETCODES`, with a migration to `distributed`.

The review below was written before that work.

## 1. What the commit does (and what replaces lockstep)

**Short answer:** nothing new replaces lockstep. The *distributed* netcode that upstream shipped earlier
already did, and it's what our servers have always run (`network.netcode = "distributed"`, the
default; every server in our API reports `"netcode": "distributed"`). a3ed50c5 deletes the old,
opt-in Xbox lockstep path (`network.netcode = "lockstep"` / `HALO_NETCODE`) and makes distributed
the only mode.

- **Lockstep (removed).**
  - This is the original Xbox system link model. Clients send input to the host. The host sends every
    machine every player's input for each 30 Hz tick, in a reliable, ordered `_message_server_game_update`.
  - Each machine simulates the same ticks and must stay bit-identical: out-of-sync checks compare the
    random seed each tick.
  - The host throttles (`game_time_update`) so it's never more than 128 ticks ahead of its slowest
    client, and stalls on a lagging client (`network_game_server_stalled_on_client`, 2 s stall timeout).
  - A client sees its own movement a full round trip late.
  - Games close when they start (`network_game_server_close_game`), because a machine must simulate
    from tick 0.
- **Distributed (kept, the only mode now).** Described in upstream `port/linux/NETCODE.md`:
  - **Clocks:** every machine ticks on its own 30 Hz clock and the host waits for nobody.
  - **Prediction:** a client drives its own player (and the vehicle it drives) from local input
    immediately. Remote players are driven by inputs the host relays every tick, sent unreliably with
    the recent ticks' buttons repeated.
  - **Authority:** the host decides damage, deaths, spawns, pickups, item spawns, scoring and the
    game type's state, and sends them (reliably where they must not be lost). Clients skip those
    systems and apply the host's state (`port/linux/game/network_distributed.c`, `network_objects.c`,
    `network_damage.c`).
  - **Corrections:** clients' copies are corrected toward the host's positions, and their own unit
    only past a tolerance.
  - **Shooter's hits:** a client reports its hits. The host validates them (the right player and
    weapon, target within tolerance of where the host had it one RTT ago, no more reports than the
    rate of fire) and deals the damage.
  - **Late joins:** games stay open once started (`network_game_server_accepts_late_joins`).
- **What a3ed50c5 changes in code** (24 files, +113/−465):
  - `game_time.c`: the host/client tick throttle is gone and every network connection ticks at
    `TICKS_PER_SECOND`.
  - `network_server_manager.c`: removes `close_game`, `get_oldest_client_update_received` and
    `stalled_on_client`. Late joins and late-joiner keep-alives no longer depend on
    `network_game_distributed()`.
  - The host's per-tick game update always carries `player_count = 0` (no actions). It only keeps
    the clients' count of host ticks.
  - `network_client_manager.c`: removes the out-of-sync / missed-update / random-seed checks. The
    client just adopts the host's update number.
  - Players always get the host-chosen slot (`player_list_index = NONE`).
  - `port_config.c` / README: the `network.netcode` setting is gone. The game advertisement says it
    plays distributed, and a client refuses a host that doesn't (an old v4 host set to lockstep),
    telling the player why.
  - `system_link_bots.py`: bots no longer send input.
- **Related upstream commits (since our merge base 0ef2ed7d; upstream +42 non-merge commits, ours +61):**
  - **4adc3a87 "Remove matching tooling and HALO_LINUX ifdefs"** (5,436 files, −1.6 M lines).
    a3ed50c5 is written on top of it, so it doesn't cherry-pick alone: 9 conflicting files.
  - **e7d1ed3a "Harden and fix the netcode"** (network protocol **version 5**):
    - A client's clock starts from the host's first game update.
    - Own-player reconciliation against the tick the host has them at (fewer rubber-bands).
    - Much stricter validation of hit reports (finite numbers, real nodes/regions/materials, the host's
      own damage flags/multiplier/team, grenade/vehicle ownership windows, a 0.5 s rewind cap).
    - Game-type event counters, so clients announce CTF/oddball/race events they missed.
    - One datagram per tick per machine (`_distributed_message_batch`).
    - Join/timeout rules:
      - A machine must join within 10 s of connecting.
      - The host drops a machine silent for 15 s.
      - The host drops a late joiner that hasn't loaded within 2 min.
      - The game is closed to joins while loading and after it ends.
    - Also large changes to p2p/xnet (their internet play), CTF/king/oddball engines, effects.
  - **aad8719f "Fix client flags in the second game"** (CTF flag state on the second game of a
    session, a bug class we'd hit because our servers run games back to back).
  - Already ported by us earlier today: 30709dbf / fd8fc726 / c26f2e84 (late joiners, respawn
    countdown, dirty-disc join) went in via our #27/#28.

## 2. How it interacts with our stack

**Our topology:**
- Browser (WASM client) ⇄ WebSocket or WebRTC data channel ⇄ Go gateway (`game/port/gateway`) ⇄ UDP
  ⇄ our headless dedicated server.
- The laravel daemon drives the dedicated server over its control channel (lobby, rotation,
  next_map, time limits).

**Net: removing lockstep is essentially neutral-to-positive for us, but the surrounding upstream
changes are not free.**

- **Transport / Go gateway / WebRTC: no impact.**
  - The gateway is datagram-transparent. The page's data channel is `{ordered: false,
    maxRetransmits: 0}` (`web_library.js`, `rtc.go`), i.e. UDP semantics, which is exactly what
    distributed wants: unreliable per-tick messages with redundancy, and reliable messages via the
    game's own ack layer.
  - Lockstep was the mode that suffered from loss (a reliable, ordered per-tick update is held up by
    any lost datagram), and we never run it.
  - e7d1ed3a's one-datagram-per-tick batching *reduces* datagrams/s through the gateway (fewer
    WebSocket frames, fewer SCTP messages). Datagram sizes stay far under the data-channel limits.
  - Their p2p/xnet changes (invite-link tunnel, Discord signalling) are native-only and bypassed by
    the web build's `posix_web_net.c` → gateway path. The dedicated server binds plain UDP behind
    the gateway, so those changes don't matter to us, apart from merge conflicts.
- **Dedicated server: small, mostly beneficial.**
  - We already rely on distributed features: games stay open for late joiners, back-to-back games
    with no lobby (#24–#28 era work), host-authoritative scoring (the tie-break tests below show the
    host's result on every client).
  - Our `#ifdef HALO_LINUX` code calls `network_game_server_close_game()` only when
    `!network_game_distributed()`, and upstream deletes that function. After adoption, that branch
    and the laravel `GameServer::NETCODES = ['distributed','lockstep']` option (admin can set
    `HALO_NETCODE=lockstep`) must go. Today that option is a foot-gun: a lockstep server would close
    games at start, break our late-join flow, and stall on slow web clients.
  - The host no longer stalling on a slow client is important for web clients: the WASM loop can
    block during a map fetch (we measured 0.3 fps for ~3 s). Under lockstep that would freeze every
    player. Under distributed only that client hitches.
- **New timeouts from e7d1ed3a (v5) are the real risk for web clients:**
  - *Drop after 15 s of silence.* A web client whose game loop is blocked (big map download without
    prefetch, a tab in the background, a GC pause on a phone) could be dropped. Mitigations:
    - Our map prefetch during postgame (laravel #33, deployed).
    - `web_fetch_map` yields to the network thread while waiting (JSPI).
    - Verify keep-alives are *sent* during a long download, not just received. Late joiners
      downloading a 40 MB CE+ map over a slow link are the case to test.
  - *Join within 10 s of connecting.* Our `auto_join` waits 3 s in the lobby before adding the
    player (`joined_seconds >= 3.0f`). That's fine, but it leaves ~7 s of headroom on slow phones;
    this timer could be relaxed for gateway connections.
  - *Late joiner must load within 2 min.* This covers slow downloads of CE+ maps (20–40 MB) on bad
    mobile links; acceptable, but it should surface as a clear "took too long to load" on the page
    rather than a silent drop.
- **Protocol versioning:** v5 isn't compatible with v4.
  - We always ship the dedicated server and the web client from the same commit in one image, so web
    players are unaffected.
  - Native desktop/Android clients built from older commits (if anyone uses them against our
    servers) would be refused, with a message.
- **Our fork's code:** 61 own commits, ~16 k lines over upstream, much of it `#ifdef HALO_LINUX`:
  - dedicated server hooks
  - auto_join / web lobby hiding
  - the late-joiner guards
  - the network_test harness
  - render changes
  - **Upstream no longer defines `HALO_LINUX`** (the build defines only `HALO_LINUX_PLATFORM_LAYER`;
    0 occurrences left in `source/`). After a naive merge, every one of our `#ifdef HALO_LINUX` blocks
    (≈500 occurrences across source/port) would **silently compile out**. Everything would still
    build, but dedicated-server behaviour would quietly revert. This is the single biggest risk.

## 3. Options

### A. Adopt upstream wholesale: merge upstream/main (4adc3a87 + a3ed50c5 + e7d1ed3a + aad8719f)
- **Trial merge (no push):** 20 conflicting files, including:
  - network_server/client/message handlers, network_game_manager.c
  - game.c, game_engine.c/h, players.c, main.c, render.c
  - network_test.c, network_distributed.h
  - configure.py, the CI workflow, READMEs
  - The 1.6 M-line deletion itself merges cleanly.
- **Work:**
  - Resolve the conflicts.
  - Mechanically unwrap or replace every `#ifdef HALO_LINUX` in our code (a script, then review
    each). Alternatively, re-define `HALO_LINUX` in our build as a stop-gap, but then upstream code
    that *assumes* the old ifdefs are gone needs checking.
  - Drop `close_game` uses and the laravel `lockstep` option.
  - Re-run the full e2e suite:
    - lone start
    - back-to-back games and transitions
    - late join mid-match and during postgame
    - CTF/oddball/king scoring
    - tie-break, time limits
    - WebRTC and WebSocket fallback
    - 16-player bot soak
- **Effort:** ~3–5 focused days, plus a soak period on a staging server before prod.
- **Pros:**
  - Keeps us on upstream's train: their hardening (hit-report validation is a real anti-cheat
    improvement for public web servers), fixes (flags in the second game), future work.
  - Smaller long-term maintenance.
- **Risks:**
  - Silent `HALO_LINUX` compile-outs.
  - v5 timeouts vs. blocked web loops (drops).
  - Their CTF/oddball engine rewrites interacting with our dedicated-server rotation and
    `game_ended` event hooks.
  - The loss of the matching tooling removes a correctness safety net we don't use anyway.
  - Big-bang change: hard to bisect.

### B. Port parts (recommended now)
Cherry-pick the behaviour, not the history, in small PRs, each with CI + e2e:
1. **Remove lockstep in our tree** (a hand-port of a3ed50c5, ~0.5–1 day):
   - Delete the lockstep branches in game_time.c, the server/client managers and the message
     handler; hard-wire distributed.
   - Remove `network.netcode`, and refuse lockstep hosts.
   - laravel: remove `lockstep` from `GameServer::NETCODES` and the admin form; a migration makes
     any server with `netcode = lockstep` distributed.
   - Low risk: we never run lockstep. Mostly deletions of code that's dead for us.
2. **aad8719f "client flags in the second game"** (~0.5 day). High value for our back-to-back CTF
   servers.
3. **e7d1ed3a in slices** (~3–4 days total), in order of value/risk:
   - (a) Hit-report validation hardening (anti-cheat; server-side only, no protocol change).
   - (b) Own-player reconciliation at the host's tick (fewer rubber-bands on high-ping web/mobile
     players; protocol change: v5).
   - (c) Game-type event counters (clients announce missed captures).
   - (d) One datagram per tick (fewer gateway frames).
   - (e) Join/timeout rules. Land these last, with web-specific relaxations (e.g. a longer silence
     timeout for gateway machines, or keep-alives sent while blocked on a map fetch).
   - Skip their p2p/xnet/Discord/UPnP changes: we don't use them.
4. **Keep our `HALO_LINUX` world for now.** Plan a separate, mechanical "unwrap HALO_LINUX" PR later
   if we ever want to rejoin upstream's history.
- **Effort:** ~5 days spread over several PRs.
- **Pros:** each step testable and revertible; no silent compile-outs; lets us keep web-specific
  tuning.
- **Risks:** a growing divergence from upstream history (future upstream fixes get harder to take);
  hand-ports can miss a hunk.

### C. Own design
Keep distributed as the base but evolve our own server-authoritative model for the web: snapshot
interpolation with entity-level deltas, a server-side input buffer and a gateway-aware
tick/backpressure.
- **Effort:** weeks.
- **Verdict:** only justified if we hit limits distributed can't meet (e.g. >16 players, spectators
  at scale, strict anti-cheat). Not recommended now: upstream is actively improving exactly this model.

## 4. Recommendation
**B now, A later if upstream keeps moving fast.** Order:
1. **lockstep removal + laravel option removal:** 1 PR per repo, ~1 day, low risk.
2. **aad8719f:** ~0.5 day.
3. **e7d1ed3a slices (a)→(e):** ~3–4 days, each with the full local e2e plus a staging soak. Give
   (e) the timeout tuning for web.
4. **Re-evaluate a full merge** once upstream stabilises (their last three commits landed within
   hours). That merge needs the `HALO_LINUX` unwrap as its own reviewed PR first.

**Test gates for every step:**
- `ninja linux web` and the game CI.
- The local compose stack e2e:
  - lone CTF start
  - back-to-back transitions with no lobby screen
  - late join mid-match and during postgame (the 3-client script)
  - T chat
  - Team Slayer/CTF tie and non-tie at the time limit
- A 16-bot soak (`HALO_TEST_INPUT=bot`) for 30 min with WebRTC and the WebSocket fallback.
- Then prod with the previous image kept for rollback.
