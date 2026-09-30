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
		framesIn: 0, framesOut: 0, bytesIn: 0, bytesOut: 0, dropped: 0, closes: 0, ping: NaN, pingAt: 0,
		rtc: null },

	// WebRTC (docs/gateway.md, "WebRTC"): once the WebSocket is open, the
	// datagrams (UDP frames and PING) move to a data channel, unordered and
	// never retransmitted, as UDP is; streams, HELLO and the signaling stay
	// on the WebSocket. The browser has no RTCPeerConnection in workers, so
	// the peer lives on the page's main thread (webrtc_main_start) and the
	// frames cross over a BroadcastChannel named for this attempt. Should
	// the channel not open within WEBRTC_OPEN_MS, or fail or go quiet later,
	// the datagrams go back to the WebSocket (which the gateway keeps
	// accepting all along), and the game carries on; the next attempt is on
	// the next WebSocket connection. HALO_WEB_RTC=0 turns it off.
	$WEBRTC_OPEN_MS: 4000,
	// webrtc_start: a new attempt for the WebSocket just opened
	$webrtc_start__deps: ["$WEBNET", "$WEBRTC_OPEN_MS", "$webrtc_signal", "$webrtc_stop", "$webnet_deliver", "webrtc_main_start"],
	$webrtc_start: (socket) => {
		if (typeof BroadcastChannel !== "function") return;
		const id = (Math.random() * 0x7fffffff) | 0;
		const channel = new BroadcastChannel(`halo-rtc-${id}`);
		const rtc = { id, channel, socket, state: "connecting", started: performance.now(), timer: 0 };
		WEBNET.rtc = rtc;
		rtc.timer = setTimeout(() => {
			if (rtc.state === "connecting") webrtc_stop(rtc, "timeout", true);
		}, WEBRTC_OPEN_MS);
		channel.onmessage = (event) => {
			const m = event.data;
			if (WEBNET.rtc !== rtc || rtc.state === "down") return;
			if (m.t === "offer") webrtc_signal(rtc, { type: "offer", sdp: m.sdp });
			else if (m.t === "open") {
				rtc.state = "open";
				clearTimeout(rtc.timer);
				err(`[webnet] datagrams over WebRTC (${Math.round(performance.now() - rtc.started)} ms)`);
			} else if (m.t === "frame") webnet_deliver(new Uint8Array(m.data));
			else if (m.t === "down") webrtc_stop(rtc, m.reason, true);
		};
		_webrtc_main_start(id);
	},
	$webrtc_signal__deps: ["$WEBNET"],
	$webrtc_signal: (rtc, message) => {
		if (rtc.socket.readyState !== 1) return;
		const body = new TextEncoder().encode(JSON.stringify(message));
		const frame = new Uint8Array(1 + body.length);
		frame[0] = 8;
		frame.set(body, 1);
		rtc.socket.send(frame);
	},
	// webrtc_stop: back to the WebSocket; tell: say so to the gateway
	$webrtc_stop__deps: ["$WEBNET", "$webrtc_signal"],
	$webrtc_stop: (rtc, reason, tell) => {
		if (!rtc || rtc.state === "down") return;
		const was = rtc.state;
		rtc.state = "down";
		clearTimeout(rtc.timer);
		rtc.channel.postMessage({ t: "close" });
		setTimeout(() => rtc.channel.close(), 1000);
		if (tell) webrtc_signal(rtc, { type: "bye", reason: String(reason) });
		err(`[webnet] WebRTC ${was === "open" ? "lost" : "not used"} (${reason}); datagrams over the WebSocket`);
	},
	// webrtc_frame: frame 8 from the gateway
	$webrtc_frame__deps: ["$WEBNET", "$webrtc_stop"],
	$webrtc_frame: (frame) => {
		const rtc = WEBNET.rtc;
		let message;
		try { message = JSON.parse(new TextDecoder().decode(frame.subarray(1))); } catch (e) { return; }
		if (!rtc || rtc.state === "down") return;
		if (message.type === "answer") rtc.channel.postMessage({ t: "answer", sdp: message.sdp });
		else if (message.type === "bye") webrtc_stop(rtc, "gateway: " + message.reason, false);
	},

	// webrtc_main_start: the peer connection, on the page's main thread. It
	// makes the offer and its data channel, and relays frames between the
	// channel and the game's worker. It sends a keepalive PING (7 'k') every
	// half second, which the gateway echoes: hearing nothing for 2.5 s (the
	// gateway: 3 s) gives the channel up (the game's worker may be busy loading a
	// map, so the main thread keeps this time, not the game).
	webrtc_main_start__proxy: "async",
	webrtc_main_start: (id) => {
		const channel = new BroadcastChannel(`halo-rtc-${id}`);
		const env = (typeof Module !== "undefined" && Module["ENV"]) || {};
		const status = (globalThis.haloWebRTC = globalThis.haloWebRTC || { transport: "ws" });
		if (typeof RTCPeerConnection !== "function" || env.HALO_WEB_RTC === "0") {
			channel.postMessage({ t: "down", reason: typeof RTCPeerConnection !== "function" ? "unsupported" : "disabled" });
			setTimeout(() => channel.close(), 1000);
			return;
		}
		const pc = new RTCPeerConnection({ iceServers: [] });
		const dc = pc.createDataChannel("halo", { ordered: false, maxRetransmits: 0 });
		dc.binaryType = "arraybuffer";
		let lastRx = 0, done = false, keepalive = 0;
		const keepaliveFrame = new Uint8Array([7, 0x6b]);
		const finish = () => {
			done = true;
			clearInterval(keepalive);
			if (status.id === id) status.transport = "ws";
			try { dc.close(); } catch (e) {}
			try { pc.close(); } catch (e) {}
			setTimeout(() => channel.close(), 1000);
		};
		const down = (reason) => {
			if (done) return;
			channel.postMessage({ t: "down", reason });
			finish();
		};
		dc.onopen = () => {
			if (done) return;
			lastRx = performance.now();
			status.id = id;
			status.transport = "rtc";
			channel.postMessage({ t: "open" });
			keepalive = setInterval(() => {
				if (performance.now() - lastRx > 2500) down("no traffic");
				else if (dc.readyState === "open") dc.send(keepaliveFrame);
			}, 500);
		};
		dc.onclose = () => down("channel closed");
		dc.onmessage = (event) => {
			lastRx = performance.now();
			const data = event.data;
			if (data.byteLength === 2 && new Uint8Array(data)[0] === 7) return; // the keepalive's echo
			channel.postMessage({ t: "frame", data });
		};
		pc.onconnectionstatechange = () => {
			if (pc.connectionState === "failed" || pc.connectionState === "closed") down("connection " + pc.connectionState);
		};
		channel.onmessage = (event) => {
			const m = event.data;
			if (done) return;
			if (m.t === "send") {
				// a congested channel drops, as a router would
				if (dc.readyState === "open" && dc.bufferedAmount < 262144) dc.send(m.data);
			} else if (m.t === "answer") {
				pc.setRemoteDescription({ type: "answer", sdp: m.sdp }).catch((e) => down("answer: " + e.message));
			} else if (m.t === "close") finish();
		};
		pc.createOffer()
			.then((offer) => pc.setLocalDescription(offer))
			.then(() => channel.postMessage({ t: "offer", sdp: pc.localDescription.sdp }))
			.catch((e) => down("offer: " + e.message));
	},

	// webnet_deliver: a frame from the gateway (WebSocket or data channel)
	// for the game
	$webnet_deliver__deps: ["$WEBNET"],
	$webnet_deliver: (frame) => {
		WEBNET.framesIn++;
		WEBNET.bytesIn += frame.length;
		// 7 PING: the gateway's echo of webnet_stats' ping. Taken
		// here, between the game's frames, so it includes the time a
		// frame waits for the game: the latency the game sees.
		if (frame[0] === 7) {
			if (frame.length === 9) {
				const sent = new DataView(frame.buffer, frame.byteOffset).getFloat64(1);
				if (sent === WEBNET.pingAt) WEBNET.ping = performance.now() - sent;
			}
			return;
		}
		if (frame[0] === 1 && WEBNET.inbox.length >= 4096) return;
		if (WEBNET.inbox.length >= 65536) {
			err(`[webnet] ${WEBNET.inbox.length} frames waiting; closing the session`);
			WEBNET.socket.close(4000, "client backlog");
			return;
		}
		WEBNET.inbox.push(frame);
	},

	webnet_connect__deps: ["$WEBNET", "$UTF8ToString", "$webrtc_start", "$webrtc_stop", "$webrtc_frame", "$webnet_deliver"],
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
				webrtc_start(socket);
			};
			// Frames are taken between the game's frames. Datagrams (type 1)
			// may be dropped when too many are waiting, as the network would;
			// stream frames never are: a hole in a stream puts the game out
			// of step with the host. Should even those pile up, the session
			// is closed (the game sees its streams closed) rather than
			// silently corrupted.
			socket.onmessage = (event) => {
				const frame = new Uint8Array(event.data);
				// 8: WebRTC signaling (webrtc_frame)
				if (frame[0] === 8) return webrtc_frame(frame);
				webnet_deliver(frame);
			};
			socket.onclose = (event) => {
				WEBNET.open = false;
				// the gateway's session, and with it the peer, is gone
				webrtc_stop(WEBNET.rtc, "WebSocket closed", false);
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
		const rtc = WEBNET.rtc;
		if (WEBNET.open && rtc && rtc.state === "open" && copy[0] === 1) {
			// a datagram: over the data channel (webrtc_main_start)
			rtc.channel.postMessage({ t: "send", data: copy.buffer });
			WEBNET.framesOut++;
			WEBNET.bytesOut += length;
		} else if (WEBNET.open) {
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
			// the way the datagrams go: the ping is theirs
			const rtc = WEBNET.rtc;
			if (rtc && rtc.state === "open") rtc.channel.postMessage({ t: "send", data: ping.buffer });
			else WEBNET.socket.send(ping);
		} else {
			WEBNET.ping = NaN;
		}
	},

	// webstats_publish: the game's once-a-second statistics (sdl_platform.c,
	// web_frame_statistics: frame timing, webnet_stats' values, send errors),
	// run on the page's main thread (the game's is a worker). They become
	// Module.haloStats and a "halo:stats" event on the page, for the site's
	// wrapper to show. Counters are totals: the page takes differences.
	webstats_publish__proxy: "async",
	webstats_publish: (values, count) => {
		values >>>= 0;
		const v = HEAPF64.slice(values >> 3, (values >> 3) + count);
		const number = (x) => (Number.isFinite(x) ? x : null);
		const stats = {
			fps: v[0], frameMs: v[1], low1Fps: v[2], maxFrameMs: v[3], frames: v[4],
			net: { open: !!v[5], pingMs: number(v[6]), buffered: v[7], framesIn: v[8], framesOut: v[9],
				bytesIn: v[10], bytesOut: v[11], dropped: v[12], waiting: v[13], closes: v[14],
				sendErrors: v[15] },
		};
		// what carries the datagrams (webrtc_main_start, on this thread)
		stats.net.transport = (globalThis.haloWebRTC && globalThis.haloWebRTC.transport) || "ws";
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
