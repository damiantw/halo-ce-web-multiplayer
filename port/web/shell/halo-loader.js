// The web build's page glue (docs/wasm-spike.md, docs/gateway.md): puts the
// maps from an HTTP URL in d:\maps (each fetched before the start or when the
// game first wants it), picks the save root, and forwards the game's log to
// the page.
//
// Query parameters:
//   maps=<url>    where the maps are (default: maps/ next to the page); the
//                 server must answer maps/index.json with [{name, size}]
//   init=<text>   init.txt's contents (console commands at start-up)
//   env=A=1,B=2   environment variables (the HALO_* settings, port_config.c)
//                 (and window.haloFiles, if the page sets it: {name: text}
//                 written to d:\ before the start)
//   preload=<a,b> the maps fetched before start (default: ui; "all" for
//                 every map in the index). Any other map is fetched when the
//                 game first wants it (Module.haloFetchMap, called from
//                 port/web/src/web_host.c: a system link client starts on the
//                 host's map in the pregame lobby and waits for it as the map
//                 loads). Emscripten's lazy files would need synchronous XHR
//                 on the page's main thread, which browsers forbid for binary
//                 data; see docs/wasm-spike.md.
//
// Diagnostics, given with env= (the site's /play passes env= through):
//   HALO_WEB_DPR=<n>   the device pixel ratio the game sees (1: a drawing
//                      buffer of the canvas's CSS size on a Retina screen)
// Player settings, given with env= or by the site's game bridge:
//   HALO_WEB_PLAYER_NAME=<name>    the player's name in network games
//   HALO_WEB_PLAYER_COLOR=<color>  the player's colour: 0-17 or its name
//                                  (white, black, red, blue, gray, yellow,
//                                  green, pink, purple, cyan, cobalt, orange,
//                                  teal, sage, brown, tan, maroon, salmon)
//   HALO_WEB_PLAYER_TEAM=<team>    the team asked for in team games: red or
//                                  blue (else auto); a port's server honours
//                                  it while the teams stay within one player
//   HALO_WEB_LOOK_SENSITIVITY=<n>  the controller's look speed, 1-10 (3)
//   HALO_WEB_INVERT_LOOK=<0|1>     1: the vertical look (sticks and mouse)
//                                  is inverted
//   HALO_WEB_STICK_DEADZONE=<n>    the sticks' dead zone, 0-90 percent (27)
//   HALO_WEB_VIBRATION=<0|1>       0: the controller does not rumble
//   HALO_WEB_MOUSE_SENSITIVITY=<x> the mouse aim's multiplier, 0.1-10 (1)
//                                  (port/web/src/web_host.c)
//   HALO_WEB_EXIT_URL=<url>        where the page goes when the player
//                                  leaves the game (default: stay)
//   HALO_WEB_MENUS=1               the multiplayer-only build's menus back
//                                  (development)
// What this build understands, for the site's game bridge (which runs before
// the game starts, after this script): webJoin, the web.join setting
// (HALO_WEB_JOIN=first, port/linux/game/auto_join.c); playerColor,
// HALO_WEB_PLAYER_COLOR; playerTeam, HALO_WEB_PLAYER_TEAM; leave, the "halo:leave" event: the build has no
// main menu, it boots into the join and, when the player leaves the game,
// asks the page to leave (web_library.js, web_leave_game); inputSettings,
// HALO_WEB_LOOK_SENSITIVITY, HALO_WEB_INVERT_LOOK, HALO_WEB_STICK_DEADZONE,
// HALO_WEB_VIBRATION and HALO_WEB_MOUSE_SENSITIVITY; gamepadEvents, the
// "halo:gamepad" event (below).
window.haloFeatures = Object.assign(window.haloFeatures || {}, { webJoin: true, playerColor: true, playerTeam: true, leave: true,
	inputSettings: true, gamepadEvents: true });
(() => {
	const params = new URLSearchParams(location.search);
	const envParam = (name) => {
		for (const pair of (params.get("env") || "").split(",")) {
			const [key, ...value] = pair.split("=");
			if (key === name) return value.join("=");
		}
		return null;
	};
	// HALO_WEB_DPR: SDL sizes the drawing buffer by window.devicePixelRatio
	// (SDL_WINDOW_HIGH_PIXEL_DENSITY), read on this thread
	const dpr = Number(envParam("HALO_WEB_DPR"));
	if (dpr > 0) {
		try {
			Object.defineProperty(window, "devicePixelRatio", { configurable: true, get: () => dpr });
		} catch (error) {
			console.warn("HALO_WEB_DPR:", error);
		}
	}
	const mapsUrl = (params.get("maps") || "maps/").replace(/\/?$/, "/");
	const logElement = document.getElementById("log");
	const statusElement = document.getElementById("status");
	const lines = [];
	const print = (text) => {
		console.log(text);
		lines.push(text);
		if (lines.length > 400) lines.shift();
		if (logElement) logElement.textContent = lines.join("\n");
	};
	window.haloLog = lines;

	var Module = window.Module = {
		canvas: document.getElementById("canvas"),
		print,
		printErr: print,
		setStatus: (text) => { if (statusElement) statusElement.textContent = text; },
		preRun: [function () {
			const FS = Module.FS;
			FS.mkdirTree("/data/maps");
			FS.mkdirTree("/home/web_user");
			Module.ENV.HOME = "/home/web_user";
			/* the web build's defaults: no internet play (p2p hosting and
			brokers need raw sockets; system link goes through the gateway),
			no updater, no clipboard invites */
			Module.ENV.HALO_NET_ONLINE = "0";
			Module.ENV.HALO_UPDATE_AUTO = "0";
			Module.ENV.HALO_NET_JOIN_FROM_CLIPBOARD = "0";
			for (const pair of (params.get("env") || "").split(",").filter(Boolean)) {
				const [name, ...value] = pair.split("=");
				Module.ENV[name] = value.join("=");
			}
			Module.addRunDependency("maps-index");
			const preload = (params.get("preload") || "ui").toLowerCase();
			const fetching = {};
			let maps = [];
			// maps/<name>.map into the file system once: true when it is there
			const fetchMap = (map) => fetching[map.name] || (fetching[map.name] = (async () => {
				const response = await fetch(mapsUrl + map.name);
				if (!response.ok) throw new Error(`${map.name}: HTTP ${response.status}`);
				const data = new Uint8Array(await response.arrayBuffer());
				FS.writeFile("/data/maps/" + map.name, data);
				print(`[web] fetched ${map.name} (${(data.length / 1048576).toFixed(0)} MB)`);
				return data.length;
			})());
			Module.haloFetchMap = async (name) => {
				const map = maps.find((entry) => entry.name.toLowerCase() === name.toLowerCase() + ".map");
				if (!map) return false;
				Module.setStatus(`fetching ${map.name}`);
				try {
					await fetchMap(map);
					return true;
				} catch (error) {
					delete fetching[map.name];
					print(`[web] cannot fetch ${map.name}: ${error}`);
					return false;
				} finally {
					Module.setStatus("");
				}
			};
			fetch(mapsUrl + "index.json").then((response) => response.json()).then(async (index) => {
				maps = index;
				const wanted = preload === "all" ? maps : maps.filter((map) => preload.split(",").some((name) =>
					map.name.toLowerCase() === name + ".map"));
				let bytes = 0;
				await Promise.all(wanted.map(async (map) => {
					bytes += await fetchMap(map);
					Module.setStatus(`maps: ${(bytes / 1048576).toFixed(0)} MB`);
				}));
				print(`[web] ${wanted.length} of ${maps.length} maps (${(bytes / 1048576).toFixed(0)} MB) from ${mapsUrl} in /data/maps; the others when the game wants them`);
				if (params.get("init")) FS.writeFile("/data/init.txt", params.get("init"));
				// files the page puts in d:\ (window.haloFiles = {"camera.txt": "..."})
				for (const [name, text] of Object.entries(window.haloFiles || {}))
					FS.writeFile("/data/" + name.replace(/[\/\\]/g, "_"), text);
				FS.chdir("/data");
				Module.removeRunDependency("maps-index");
			}).catch((error) => {
				print(`[web] cannot list the maps at ${mapsUrl}: ${error}`);
			});
		}],
		onAbort: (what) => print(`[web] abort: ${what}`),
	};
})();

// Gamepads. SDL (its Emscripten joystick driver, on this thread) reads the
// browser's Gamepad API; a pad the browser maps to the "standard" layout
// needs nothing more. The original Xbox controllers (the Duke, 045e:0202,
// and the Controller S, 045e:0285/0287/0288/0289) do: their buttons are A,
// B, black, X, Y, white, back, start and the stick clicks, with no
// shoulders. This puts them in the standard layout the game expects
// (xinput_sdl.c: white and black on the shoulders):
//   Chrome on Linux (xpad) reports them "standard" by the Xbox 360's order,
//     which is wrong for a Duke: 2 is black, 3 X, 4 Y, 5 white, 10 the
//     right stick click, 16 the left one;
//   unmapped (Firefox on Linux, DirectInput drivers on Windows): the raw
//     buttons above and the axes left X/Y, left trigger, right X/Y, right
//     trigger (-1 at rest) and the D-pad as a hat (X, Y), or as buttons
//     10-13 (left, right, up, down) after them.
// Face buttons stay digital (xpad reports them so). Browsers show a pad only
// after one of its buttons is pressed; the connection and disconnection
// become a "halo:gamepad" event ({connected, index, id, name, layout:
// "standard", "duke" or "unmapped"}; the site's game bridge passes it on)
// and a short notice over the game.
(() => {
	const nativeGetGamepads = Navigator.prototype.getGamepads;
	if (typeof nativeGetGamepads !== "function") return;
	const DUKE_PRODUCTS = ["0202", "0285", "0287", "0288", "0289"];
	// Chrome: "... (STANDARD GAMEPAD Vendor: 045e Product: 0289)" or
	// "... (Vendor: 045e Product: 0289)"; Firefox: "45e-289-..." or "045e-0289-..."
	const ids = (id) => {
		let match = /Vendor: ([0-9a-f]{1,4}) Product: ([0-9a-f]{1,4})/i.exec(id);
		if (!match) match = /^([0-9a-f]{1,4})-([0-9a-f]{1,4})-/i.exec(id);
		return match ? [match[1].toLowerCase().padStart(4, "0"), match[2].toLowerCase().padStart(4, "0")] : [null, null];
	};
	const isDuke = (pad) => {
		const [vendor, product] = ids(pad.id);
		return vendor === "045e" && DUKE_PRODUCTS.includes(product);
	};
	const padName = (pad) => (pad.id || "Gamepad")
		.replace(/\s*\((?:[^()]*STANDARD GAMEPAD[^()]*|Vendor: [0-9a-f]+ Product: [0-9a-f]+)\)\s*$/i, "")
		.replace(/^[0-9a-f]{1,4}-[0-9a-f]{1,4}-/i, "").trim() || "Gamepad";
	const button = (value, pressed) => ({ pressed: !!pressed, touched: !!pressed || value > 0, value });
	const copy = (b) => (b ? button(typeof b === "object" ? b.value : b, typeof b === "object" ? b.pressed : b > 0.5) : button(0, false));
	// a trigger axis rests at -1, but a browser reports 0 for an axis that has
	// not moved yet: until one is seen at rest, only a push past 0 counts
	const triggerRest = {};
	const trigger = (pad, axis) => {
		const value = pad.axes[axis] || 0;
		const key = pad.index + ":" + axis;
		if (value < -0.5) triggerRest[key] = true;
		const pressure = triggerRest[key] ? (value + 1) / 2 : Math.max(0, value);
		return button(pressure, pressure > 0.12);
	};
	const standardPad = (pad, buttons, axes) => ({
		id: pad.id, index: pad.index, connected: pad.connected, timestamp: pad.timestamp,
		mapping: "standard", axes, buttons, vibrationActuator: pad.vibrationActuator || null,
		hapticActuators: pad.hapticActuators,
	});
	const remap = (pad) => {
		if (!pad || !isDuke(pad)) return pad;
		const b = pad.buttons || [];
		const a = pad.axes || [];
		if (pad.mapping === "standard") {
			// the Xbox 360 order put on a Duke (Chrome, xpad)
			const out = Array.from({ length: 17 }, (_, index) => copy(b[index]));
			out[2] = copy(b[3]); out[3] = copy(b[4]); out[4] = copy(b[5]); out[5] = copy(b[2]);
			out[10] = copy(b[16]); out[11] = copy(b[10]); out[16] = button(0, false);
			return standardPad(pad, out, a.slice(0, 4));
		}
		if (b.length < 10 || a.length < 6) return pad;
		const hatButtons = a.length < 8 && b.length >= 14;
		const hatX = hatButtons ? 0 : a[6] || 0;
		const hatY = hatButtons ? 0 : a[7] || 0;
		const dpad = (index, down) => (hatButtons ? copy(b[index]) : button(down ? 1 : 0, down));
		const out = [
			copy(b[0]), copy(b[1]), copy(b[3]), copy(b[4]), copy(b[5]), copy(b[2]),
			trigger(pad, 2), trigger(pad, 5), copy(b[6]), copy(b[7]), copy(b[8]), copy(b[9]),
			dpad(12, hatY < -0.5), dpad(13, hatY > 0.5), dpad(10, hatX < -0.5), dpad(11, hatX > 0.5),
			button(0, false),
		];
		return standardPad(pad, out, [a[0] || 0, a[1] || 0, a[3] || 0, a[4] || 0]);
	};
	Object.defineProperty(Navigator.prototype, "getGamepads", {
		configurable: true, writable: true,
		value: function getGamepads() {
			const pads = nativeGetGamepads.call(this);
			return pads ? Array.prototype.map.call(pads, remap) : pads;
		},
	});

	const notice = document.createElement("div");
	notice.id = "halo-gamepad-notice";
	notice.hidden = true;
	notice.setAttribute("role", "status");
	// top centre, clear of the page's own corner buttons and stats
	notice.style.cssText = "position: fixed; left: 50%; top: 56px; transform: translateX(-50%); z-index: 10; pointer-events: none; padding: 6px 12px;"
		+ " border-radius: 4px; background: rgba(0, 0, 0, 0.75); color: #fff; font: 13px/1.3 system-ui, sans-serif; max-width: 60vw;";
	let noticeTimer = 0;
	const show = (text) => {
		if (!notice.isConnected && document.body) document.body.appendChild(notice);
		notice.textContent = text;
		notice.hidden = false;
		clearTimeout(noticeTimer);
		noticeTimer = setTimeout(() => { notice.hidden = true; }, 5000);
	};
	window.haloGamepads = {};
	const announce = (raw, connected) => {
		if (!raw) return;
		const layout = isDuke(raw) ? "duke" : raw.mapping === "standard" ? "standard" : "unmapped";
		const detail = { connected, index: raw.index, id: raw.id, name: padName(raw), layout };
		if (connected) window.haloGamepads[raw.index] = detail;
		else delete window.haloGamepads[raw.index];
		show(connected
			? `🎮 ${detail.name} connected` + (layout === "duke" ? " (original Xbox layout)" : layout === "unmapped" ? " — no standard layout in this browser: some buttons may be wrong" : "")
			: `🎮 ${detail.name} disconnected`);
		window.dispatchEvent(new CustomEvent("halo:gamepad", { detail }));
	};
	// (capture phase on the window, before this script's halo.js: SDL takes
	// the button and axis counts from the event's pad)
	window.addEventListener("gamepadconnected", (event) => {
		const raw = event.gamepad;
		const mapped = remap(raw);
		if (mapped !== raw) {
			try { Object.defineProperty(event, "gamepad", { configurable: true, value: mapped }); } catch (error) { /* (kept) */ }
		}
		announce(raw, true);
	}, true);
	window.addEventListener("gamepaddisconnected", (event) => announce(event.gamepad, false), true);
})();

// Leaving: the game says the player left (web_library.js, web_leave_game).
// The site's game bridge takes the event (preventDefault) and the site goes
// back to its home page; standing alone, the page goes to HALO_WEB_EXIT_URL
// if given and otherwise says so.
window.addEventListener("halo:leave", (event) => {
	setTimeout(() => {
		if (event.defaultPrevented) return;
		const exitUrl = (new URLSearchParams(location.search).get("env") || "").split(",")
			.map((pair) => pair.split("=")).filter(([key]) => key === "HALO_WEB_EXIT_URL").map(([, ...value]) => value.join("="))[0];
		if (exitUrl) {
			location.assign(decodeURIComponent(exitUrl));
		} else if (window.Module && window.Module.setStatus) {
			window.Module.setStatus(event.detail && event.detail.reason === "no_game" ? "No game to join" : "You left the game");
		}
	}, 0);
});

// Pointer lock: the mouse aims. A browser locks the pointer only for a
// request made while it handles a click or a key (user activation), and the
// game runs on a worker, outside those handlers; so the game says when it
// wants the mouse (sdl_platform.c, platform_mouse_capture; web_library.js,
// web_mouse_capture: a "halo:mouse-capture" event) and a click on the canvas
// locks it here. That click only takes the mouse: the game does not see it
// (no shot). Once the canvas is locked, SDL's pointerlockchange handler sees
// it and its motion events carry movementX/Y. Esc (the browser's) releases
// the mouse; that is the only key that does (F12 is the browser's tools, the
// game does not take it on the web). While the game wants
// the mouse and does not have it, a hint says to click.
(() => {
	const canvas = document.getElementById("canvas");
	if (!canvas || typeof canvas.requestPointerLock !== "function") return;
	let wanted = false;
	let failed = false;
	const hint = document.createElement("div");
	hint.id = "halo-mouse-hint";
	hint.hidden = true;
	hint.setAttribute("role", "status");
	hint.style.cssText = "position: fixed; left: 50%; bottom: 16px; transform: translateX(-50%); z-index: 10;"
		+ " pointer-events: none; padding: 5px 12px; border-radius: 4px; background: rgba(0, 0, 0, 0.75);"
		+ " color: #fff; font: 13px/1.3 system-ui, sans-serif; white-space: nowrap;";
	document.body.appendChild(hint);
	const locked = () => document.pointerLockElement === canvas;
	const update = () => {
		hint.textContent = failed ? "Click the game again to capture the mouse" : "Click to capture the mouse";
		hint.hidden = !wanted || locked();
	};
	const settle = (request) => {
		if (request && typeof request.catch === "function") request.catch(() => {});
	};
	const lock = () => {
		let request;
		try {
			// raw mouse motion (no pointer acceleration) where the browser has it
			request = canvas.requestPointerLock({ unadjustedMovement: true });
		} catch (error) {
			request = null;
		}
		if (request && typeof request.then === "function") {
			request.catch((error) => {
				if (error && error.name === "NotSupportedError" && wanted && !locked()) settle(canvas.requestPointerLock());
			});
		} else if (request === null) {
			settle(canvas.requestPointerLock());
		}
	};
	// capture phase on the window: before SDL's listeners on the canvas
	window.addEventListener("pointerdown", (event) => {
		if (event.target !== canvas || event.pointerType !== "mouse" || !wanted || locked()) return;
		event.stopImmediatePropagation();
		canvas.focus();
		lock();
	}, true);
	window.addEventListener("halo:mouse-capture", (event) => {
		wanted = !!(event.detail && event.detail.capture);
		if (!wanted && locked()) {
			document.exitPointerLock();
		} else if (wanted && !locked() && navigator.userActivation && navigator.userActivation.isActive) {
			// a click or key just now (the one that closed the menu): no second click
			lock();
		}
		update();
	});
	document.addEventListener("pointerlockchange", () => {
		if (locked()) failed = false;
		update();
	});
	// The game's keys that the browser acts on too: Tab moves the focus off
	// the canvas (out of the page, when the game is in a frame), and that
	// ends the pointer lock; Space scrolls, Alt opens the menu bar, F1-F11
	// open help, reload, full screen and so on, Backspace goes back, and / and
	// ' open Firefox's quick find. While the game has (or wants) the mouse,
	// their default actions are cancelled. Only the default: the event still
	// goes on to SDL's listeners, and the game sees the key. F12 and Esc keep
	// theirs (Esc releases the mouse, F12 is the browser's tools); a text
	// field keeps every key, and so do shortcuts with Ctrl or Cmd.
	const gameKeys = new Set(["Tab", "AltLeft", "AltRight", "Space", "Backspace", "Slash", "Quote",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11"]);
	const editable = (element) => !!element && element !== canvas &&
		(element.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(element.tagName));
	const gameKey = (event) => {
		if (!(wanted || locked()) || event.ctrlKey || event.metaKey || !gameKeys.has(event.code) ||
			editable(event.target) || editable(document.activeElement)) {
			return;
		}
		event.preventDefault();
		if (document.activeElement !== canvas) canvas.focus({ preventScroll: true });
	};
	// (capture phase on the window: before any other listener, which still
	// gets the event)
	window.addEventListener("keydown", gameKey, true);
	window.addEventListener("keyup", gameKey, true);
	document.addEventListener("pointerlockerror", () => {
		// e.g. a request within a second of leaving the lock with Esc; a
		// browser without raw motion refuses the first request, and the
		// retry without it follows at once
		setTimeout(() => {
			if (!locked()) failed = true;
			update();
		}, 250);
	});
})();
