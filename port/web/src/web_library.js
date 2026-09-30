// JavaScript side of the web build's platform layer (tools/web_build.py).
//
// webnet_*: the gateway WebSocket of posix_web_net.c. It lives in the
// thread that uses it (the game's), so its messages are taken when that
// thread returns to its event loop, which it does at every frame.
addToLibrary({
	// generation: how many times the WebSocket has closed. The gateway's
	// streams die with it (a new session has none), and posix_web_net.c
	// closes its streams when it sees the count change.
	// framesIn/Out, bytesIn/Out, dropped (frames refused while the socket is
	// down), closes and ping (ms, the last PING's round trip; NaN: none
	// answered) are for the page's overlay (webnet_stats)
	$WEBNET: { socket: null, inbox: [], outbox: [], open: false, generation: 0,
		framesIn: 0, framesOut: 0, bytesIn: 0, bytesOut: 0, dropped: 0, closes: 0, ping: NaN, pingAt: 0 },

	webnet_connect__deps: ["$WEBNET", "$UTF8ToString"],
	webnet_connect: (url, token, tokenUrl) => {
		url = UTF8ToString(url);
		token = UTF8ToString(token);
		tokenUrl = UTF8ToString(tokenUrl);
		let attempts = 0;
		// A join token opens one session (the gateway refuses it again): a
		// reconnect asks tokenUrl (HALO_WEB_TOKEN_URL, the site's endpoint;
		// same origin, so the page's session cookie goes along) for a new
		// one, answered as {"token": ...} or plain text.
		const renew = async () => {
			if (!tokenUrl) return token;
			const response = await fetch(tokenUrl, { method: "POST", credentials: "same-origin",
				headers: { "Accept": "application/json" } });
			if (!response.ok) throw new Error(`HTTP ${response.status}`);
			const text = (await response.text()).trim();
			return text.startsWith("{") ? JSON.parse(text).token : text;
		};
		const connect = () => {
			// the token rides in the subprotocol list (port/gateway): a
			// query string would end up in access logs
			const socket = new WebSocket(url, token ? ["halo.v1", "t." + token] : ["halo.v1"]);
			socket.binaryType = "arraybuffer";
			socket.onopen = () => {
				WEBNET.open = true;
				attempts = 0;
				for (const frame of WEBNET.outbox) socket.send(frame);
				WEBNET.outbox = [];
				err(`[webnet] connected to ${url}`);
			};
			// Frames are taken between the game's frames. Datagrams (type 1)
			// may be dropped when too many are waiting, as the network would;
			// stream frames never are: a hole in a stream puts the game out
			// of step with the host. Should even those pile up, the session
			// is closed (the game sees its streams closed) rather than
			// silently corrupted.
			socket.onmessage = (event) => {
				const frame = new Uint8Array(event.data);
				WEBNET.framesIn++;
				WEBNET.bytesIn += frame.length;
				// 7 PING: the gateway's echo of webnet_stats' ping. Taken
				// here, between the game's frames, so it includes the time a
				// frame waits for the game: the latency the game sees.
				if (frame[0] === 7) {
					if (frame.length === 9) {
						const sent = new DataView(frame.buffer).getFloat64(1);
						if (sent === WEBNET.pingAt) WEBNET.ping = performance.now() - sent;
					}
					return;
				}
				if (frame[0] === 1 && WEBNET.inbox.length >= 4096) return;
				if (WEBNET.inbox.length >= 65536) {
					err(`[webnet] ${WEBNET.inbox.length} frames waiting; closing the session`);
					socket.close(4000, "client backlog");
					return;
				}
				WEBNET.inbox.push(frame);
			};
			socket.onclose = (event) => {
				WEBNET.open = false;
				WEBNET.generation++;
				WEBNET.closes++;
				// what was queued for the dead session's streams means nothing
				// to the next one
				WEBNET.outbox = WEBNET.outbox.filter((frame) => frame[0] === 1);
				// (1008: the gateway's policy refusal, a token for other servers)
				if (++attempts > 5 || event.code === 1008) {
					err(`[webnet] disconnected from ${url} (${event.code}); giving up`);
					return;
				}
				err(`[webnet] disconnected from ${url} (${event.code}); retrying`);
				setTimeout(() => renew().then((next) => { token = next; connect(); }, (error) => {
					err(`[webnet] no new join token from ${tokenUrl}: ${error}`);
					connect();
				}), 2000);
			};
			WEBNET.socket = socket;
		};
		connect();
	},

	webnet_send__deps: ["$WEBNET"],
	webnet_send: (frame, length) => {
		frame >>>= 0;
		// a copy: WebSocket.send takes no views of shared memory
		const copy = HEAPU8.slice(frame, frame + length);
		if (WEBNET.open) {
			WEBNET.socket.send(copy);
			WEBNET.framesOut++;
			WEBNET.bytesOut += length;
		} else if (WEBNET.socket && WEBNET.outbox.length < 256) WEBNET.outbox.push(copy);
		else {
			WEBNET.dropped++;
			return 0;
		}
		return 1;
	},

	webnet_generation__deps: ["$WEBNET"],
	webnet_generation: () => WEBNET.generation,

	// webnet_stats: the socket's counters into out (10 doubles): open, ping
	// ms, bufferedAmount, frames in, frames out, bytes in, bytes out, frames
	// dropped, frames waiting for the game, closes. Then a new PING (the game
	// calls this once a second).
	webnet_stats__deps: ["$WEBNET"],
	webnet_stats: (out) => {
		out >>>= 0;
		const socket = WEBNET.socket;
		const values = [WEBNET.open ? 1 : 0, WEBNET.ping, socket ? socket.bufferedAmount : 0,
			WEBNET.framesIn, WEBNET.framesOut, WEBNET.bytesIn, WEBNET.bytesOut, WEBNET.dropped,
			WEBNET.inbox.length, WEBNET.closes];
		values.forEach((value, index) => { HEAPF64[(out >> 3) + index] = value; });
		if (WEBNET.open) {
			const ping = new Uint8Array(9);
			ping[0] = 7;
			WEBNET.pingAt = performance.now();
			new DataView(ping.buffer).setFloat64(1, WEBNET.pingAt);
			WEBNET.socket.send(ping);
		} else {
			WEBNET.ping = NaN;
		}
	},

	// webstats_publish: the game's once-a-second statistics (sdl_platform.c,
	// web_frame_statistics: frame timing, webnet_stats' values, send errors),
	// run on the page's main thread (the game's is a worker). They become
	// Module.haloStats and a "halo:stats" event on the page, for the site's
	// wrapper to show. Counters are totals: the page takes differences. The
	// 1% low is null right after a map loads (too few frames yet).
	webstats_publish__proxy: "async",
	webstats_publish: (values, count) => {
		values >>>= 0;
		const v = HEAPF64.slice(values >> 3, (values >> 3) + count);
		const number = (x) => (Number.isFinite(x) ? x : null);
		const stats = {
			fps: v[0], frameMs: v[1], low1Fps: number(v[2]), maxFrameMs: v[3], frames: v[4],
			net: { open: !!v[5], pingMs: number(v[6]), buffered: v[7], framesIn: v[8], framesOut: v[9],
				bytesIn: v[10], bytesOut: v[11], dropped: v[12], waiting: v[13], closes: v[14],
				sendErrors: v[15] },
		};
		Module["haloStats"] = stats;
		if (typeof dispatchEvent === "function" && typeof CustomEvent === "function")
			dispatchEvent(new CustomEvent("halo:stats", { detail: stats }));
	},

	// web_mouse_capture: the game wants the mouse for aiming (1) or lets go
	// of it (0; menus, F12) (sdl_platform.c, platform_mouse_capture). On the
	// page's main thread: a "halo:mouse-capture" event for halo-loader.js,
	// which locks the pointer on the next click on the canvas.
	web_mouse_capture__proxy: "async",
	web_mouse_capture: (capture) => {
		Module["haloMouseWanted"] = !!capture;
		if (typeof dispatchEvent === "function" && typeof CustomEvent === "function")
			dispatchEvent(new CustomEvent("halo:mouse-capture", { detail: { capture: !!capture } }));
	},

	// web_leave_game: the multiplayer-only build is done (auto_join.c,
	// web_leave): the player left the game (the pause menu's Quit, B in the
	// lobby), the connection was lost or the join refused (reason 0, "left";
	// error_code: the game's error, -1 for none), or no game was found to
	// join (1, "no_game"). On the page's main thread: a "halo:leave" event
	// ({reason, errorCode}) for the site's page, which goes back to its home
	// page; with nobody to take it (preventDefault), halo-loader.js does.
	web_leave_game__proxy: "async",
	web_leave_game: (reason, errorCode) => {
		const detail = { reason: reason === 1 ? "no_game" : "left", errorCode: errorCode < 0 ? null : errorCode };
		Module["haloLeft"] = detail;
		if (typeof dispatchEvent === "function" && typeof CustomEvent === "function")
			dispatchEvent(new CustomEvent("halo:leave", { detail, cancelable: true }));
	},

	webnet_receive__deps: ["$WEBNET"],
	webnet_receive: (frame, capacity) => {
		frame >>>= 0;
		const next = WEBNET.inbox.shift();
		if (!next) return 0;
		const length = Math.min(next.length, capacity);
		HEAPU8.set(next.subarray(0, length), frame);
		return length;
	},
});
