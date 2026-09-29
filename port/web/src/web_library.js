// JavaScript side of the web build's platform layer (tools/web_build.py).
//
// webnet_*: the gateway WebSocket of posix_web_net.c. It lives in the
// thread that uses it (the game's), so its messages are taken when that
// thread returns to its event loop, which it does at every frame.
addToLibrary({
	$WEBNET: { socket: null, inbox: [], outbox: [], open: false },

	webnet_connect__deps: ["$WEBNET", "$UTF8ToString"],
	webnet_connect: (url) => {
		url = UTF8ToString(url);
		const connect = () => {
			const socket = new WebSocket(url);
			socket.binaryType = "arraybuffer";
			socket.onopen = () => {
				WEBNET.open = true;
				for (const frame of WEBNET.outbox) socket.send(frame);
				WEBNET.outbox = [];
				err(`[webnet] connected to ${url}`);
			};
			socket.onmessage = (event) => {
				if (WEBNET.inbox.length < 4096) WEBNET.inbox.push(new Uint8Array(event.data));
			};
			socket.onclose = () => {
				WEBNET.open = false;
				err(`[webnet] disconnected from ${url}; retrying`);
				setTimeout(connect, 2000);
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
