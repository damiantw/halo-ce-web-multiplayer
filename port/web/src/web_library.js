// JavaScript side of the web build's platform layer (tools/web_build.py).
//
// webnet_*: the gateway WebSocket of posix_web_net.c. It lives in the
// thread that uses it (the game's), so its messages are taken when that
// thread returns to its event loop, which it does at every frame.
addToLibrary({
	$WEBNET: { socket: null, inbox: [], outbox: [], open: false },

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
			socket.onmessage = (event) => {
				if (WEBNET.inbox.length < 4096) WEBNET.inbox.push(new Uint8Array(event.data));
			};
			socket.onclose = (event) => {
				WEBNET.open = false;
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
		if (WEBNET.open) WEBNET.socket.send(copy);
		else if (WEBNET.socket && WEBNET.outbox.length < 256) WEBNET.outbox.push(copy);
		else return 0;
		return 1;
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
