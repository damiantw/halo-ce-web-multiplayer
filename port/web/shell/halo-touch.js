// The on-screen touch controls for phones (Android Chrome, landscape),
// ported from upstream OpenCE's port/web/site/touch.js (4a9e515d "Web touch
// controls") into this fork's shell: a floating stick on the left, a swipe
// anywhere else turns the view, the controller's buttons and triggers named
// by the profile's mapping, and taps held for at least 60 ms
// (port/web/src/web_touch.c) so the game, which reads its controller once a
// frame, sees them.
//
// The overlay writes the controller state into a few words of the game's
// shared memory, which the game reads with port 0's controller
// (port/web/src/web_shared.h, web_touch.c; port/linux/src/touch_input.c,
// xinput_sdl.c), and reads back when to show, the buttons' names and the
// rumble. The stick moves the player directly and the swipe enters before
// the aim assist (source/game/player_control.c), which it gets as a stick
// does unless HALO_WEB_TOUCH_AIM_ASSIST=0.
//
// Added over upstream's, for this site:
//   - Only on a phone or tablet: a coarse primary pointer (matchMedia
//     "(pointer: coarse)") with touch events. A computer, a touch laptop
//     included, never shows it, and its mouse and keyboard are untouched.
//     HALO_WEB_TOUCH_CONTROLS=on / off (env=) overrides; "auto" also hides
//     the controls while a gamepad is connected.
//   - The first tap asks for fullscreen and a landscape lock: in the site's
//     /play wrapper, its game container (a "touch-fullscreen" message to the
//     parent, which holds the tap's user activation too); alone, this page.
//   - Portrait shows "rotate your phone".
//   - Menu (Start: the pause menu, in place of Esc), Score (Back, held
//     until tapped again: the scoreboard) and Chat (in the wrapper: a
//     "chat-key" message, so the page's chat input opens with the
//     on-screen keyboard) buttons at the top centre.
//   - In a menu (the pause menu, an error, the postgame) only what works
//     there shows: A, B, a D-pad where the stick was, Menu and Chat. (The web
//     build's menus take no pointer, so upstream's Android touch_menu.c,
//     taps in the menus, has nothing to drive here.)
//   - The page drops pinch zoom and scrolling (touch-action: none, the
//     viewport's maximum-scale=1) and the loader draws at a device pixel
//     ratio of 1 (halo-loader.js).
//
// Layout (on a 960 x 540 grid stretched to the screen, sizes keeping their
// shape). The game draws a 4:3 picture letterboxed in the screen, so a
// landscape phone (19.5:9, 915 x 412) has black bands about 180 px wide
// beside it: the controls sit in those bands and stay off the HUD. The
// motion tracker (radar) is at the picture's bottom left, near the left
// thumb, so the stick floats: it appears where the thumb lands anywhere in
// the left 40% below the top third (in the band, mostly) and nothing is drawn
// at rest but a faint "Move" ring in the band, until the stick is first
// used; the radar stays readable. Above the stick, in the left band: a
// second Fire (fire with the left thumb while the right one aims), Crouch,
// Flashlight and Grenade type. The right band has the face buttons as a
// controller's (Jump, Melee, Action / Reload, Weapon), Fire and Grenade above
// them and Zoom at the top. The picture's top centre, empty in Halo's HUD
// (ammo top left, shields top right), has Menu, Score and Chat. A 16:9 phone
// has narrower bands, and the outer buttons then reach a little into the
// picture's edges. The D-pad (it only moves, as the stick does) and Back
// (Score is Back) are left out of upstream's layout.
//
// window.HaloTouch: {enabled, start(module), state()} (state is for tests).
'use strict';

window.HaloTouch = (() => {
	// web_shared.h's word indices
	const SCENE = 1;
	const RUMBLE = 2;
	const BINDINGS_SERIAL = 3;
	const BINDINGS = 4;
	const STATE = 20;
	const PRESSES = 27;
	const LOOK = 44;
	const LOOKED = 48;
	const ASSISTED = 49;
	// touch_input.c's _touch_scene_* bits
	const SCENE_KNOWN = 1;
	const SCENE_MENUS = 2;
	const SCENE_ON = 4;
	const SCENE_OFF = 8;

	const GRID_WIDTH = 960;
	const GRID_HEIGHT = 540;
	const STICK_RADIUS = 64;
	const STICK_DEAD_ZONE = 0.12;
	const STICK_REACH = 1.28;
	const FLOATING_ZONE_WIDTH = 0.4;
	const FLOATING_ZONE_TOP = 0.33;
	const VIBRATION_MS = 110;
	const VIBRATION_RENEW_MS = 70;

	const params = new URLSearchParams(location.search);
	const envParam = (name) => {
		for (const pair of (params.get("env") || "").split(",")) {
			const [key, ...value] = pair.split("=");
			if (key === name) return value.join("=");
		}
		return null;
	};
	const forced = (envParam("HALO_WEB_TOUCH_CONTROLS") || "").toLowerCase();
	// a phone or tablet: its primary pointer is a finger, and it has touch
	// events (a touch laptop's primary pointer is its mouse or touchpad)
	const touchDevice = () => matchMedia("(pointer: coarse)").matches &&
		("ontouchstart" in window || navigator.maxTouchPoints > 0);
	const enabled = forced === "on" || forced === "1" || (forced !== "off" && forced !== "0" && touchDevice());

	// each control: its SDL button bit or trigger (axis 4 left, 5 right), the
	// game's controller button that names it (input.h), the Xbox button's
	// letter, its place on the grid and its radius (upstream's TouchLayout.java
	// sizes, placed for the bands beside the picture: Layout, above)
	const CONTROLS = [
		// the right thumb, in the band right of the picture
		{ trigger: 5, button: 7, key: "RT", x: 900, y: 252, radius: 40 },
		{ trigger: 4, button: 6, key: "LT", x: 806, y: 198, radius: 32 },
		{ bit: 8, button: 15, key: "RS", x: 906, y: 150, radius: 27 },
		{ bit: 3, button: 3, key: "Y", x: 852, y: 320, radius: 30 },
		{ bit: 2, button: 2, key: "X", x: 796, y: 390, radius: 32 },
		{ bit: 1, button: 1, key: "B", x: 908, y: 390, radius: 30, menu: true, menuLabel: "Back" },
		{ bit: 0, button: 0, key: "A", x: 852, y: 462, radius: 38, menu: true, menuLabel: "Select" },
		// the left thumb, in the band left of the picture, above the stick
		{ trigger: 5, button: 7, key: "RT", x: 60, y: 206, radius: 34 },
		{ bit: 7, button: 14, key: "LS", x: 150, y: 206, radius: 28 },
		{ bit: 9, button: 5, key: "White", x: 60, y: 116, radius: 25 },
		{ bit: 10, button: 4, key: "Black", x: 150, y: 116, radius: 25 },
		// the top centre: the pause menu, the scoreboard (latched), the chat
		{ bit: 6, button: 12, key: "Start", x: 410, y: 30, radius: 24, label: "Menu", menu: true },
		{ bit: 4, button: 13, key: "Back", x: 480, y: 30, radius: 24, label: "Score", latch: true },
		{ chat: true, key: "", x: 550, y: 30, radius: 24, label: "Chat", menu: true },
		// the D-pad, only while a menu is up (where the stick is in a game)
		{ bit: 11, button: 8, key: "Up", x: 100, y: 318, radius: 28, label: "▲", menuOnly: true },
		{ bit: 12, button: 9, key: "Down", x: 100, y: 458, radius: 28, label: "▼", menuOnly: true },
		{ bit: 13, button: 10, key: "Left", x: 34, y: 388, radius: 28, label: "◀", menuOnly: true },
		{ bit: 14, button: 11, key: "Right", x: 166, y: 388, radius: 28, label: "▶", menuOnly: true },
	];
	// the game's controls (input_abstraction.c's _game_control_*), as the
	// buttons are named by them
	const GAME_CONTROLS = ["Jump", "Gren. type", "Action", "Weapon", "Melee", "Light", "Grenade", "Fire", "Menu",
		"Score", "Crouch", "Zoom"];

	let words = null;
	let base = 0;
	let overlay = null;
	let stickKnob = null;
	let stickRing = null;
	let stickHint = null;
	let stickUsed = false;
	let shown = false;
	let menus = false;
	let bindingsSerial = -1;
	let lastVibration = 0;
	const fingers = new Map();
	const held = new Map();
	const latched = new Set();
	let stick = null;
	const counts = { taps: 0, chat: 0, fullscreen: 0 };

	const read = (index) => Atomics.load(words, base + index);
	const embedded = window.parent !== window;
	const post = (type, fields) => {
		if (window.haloBridge && typeof window.haloBridge.post === "function") window.haloBridge.post(type, fields || {});
		else if (embedded) window.parent.postMessage(Object.assign({ halo: 1, type }, fields || {}), location.origin);
	};

	// ---------- the page: no zoom nor scroll, rotate, fullscreen

	function preparePage() {
		document.documentElement.classList.add("halo-touch");
		let viewport = document.querySelector('meta[name="viewport"]');
		if (!viewport) {
			viewport = document.createElement("meta");
			viewport.name = "viewport";
			document.head.appendChild(viewport);
		}
		viewport.content = "width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover";
		const style = document.createElement("style");
		style.textContent = `
html.halo-touch, html.halo-touch body { touch-action: none; overscroll-behavior: none; overflow: hidden; height: 100%;
  -webkit-user-select: none; user-select: none; -webkit-touch-callout: none; -webkit-tap-highlight-color: transparent; }
html.halo-touch #canvas { position: fixed; inset: 0; width: 100vw !important; height: 100vh !important; height: 100dvh !important;
  object-fit: contain; touch-action: none; }
html.halo-touch #status, html.halo-touch #log { display: none; }
#halo-mouse-hint { display: none !important; }
.halo-touch-overlay { position: fixed; inset: 0; z-index: 6; touch-action: none; user-select: none; -webkit-user-select: none; }
.halo-touch-overlay.menus { background: transparent; }
.halo-touch-overlay.menus .halo-touch-control:not(.menu):not(.menu-only), .halo-touch-overlay:not(.menus) .halo-touch-control.menu-only,
.halo-touch-overlay.menus .halo-touch-stick-hint { display: none; }
.halo-touch-control { position: absolute; display: flex; flex-direction: column; align-items: center; justify-content: center;
  border-radius: 50%; border: 2px solid rgba(215, 221, 229, 0.42); background: rgba(14, 17, 22, 0.32);
  color: rgba(215, 221, 229, 0.92); font-family: system-ui, sans-serif; font-weight: 700; text-align: center; line-height: 1.05;
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.85); pointer-events: none; box-sizing: border-box; }
.halo-touch-control .key { font-size: 0.62em; font-weight: 600; color: rgba(139, 150, 165, 0.95); margin-top: 0.1em; }
.halo-touch-control.fire { border-color: rgba(95, 179, 255, 0.6); }
.halo-touch-control.top { border-radius: 8px; background: rgba(22, 27, 34, 0.6); border-color: rgba(42, 49, 60, 0.95); }
.halo-touch-control.held { background: rgba(95, 179, 255, 0.42); border-color: #5fb3ff; color: #fff; }
.halo-touch-stick-hint { position: absolute; border-radius: 50%; border: 2px dashed rgba(215, 221, 229, 0.22);
  background: rgba(14, 17, 22, 0.08); pointer-events: none; display: flex; align-items: center; justify-content: center;
  color: rgba(215, 221, 229, 0.45); font: 600 12px system-ui, sans-serif; box-sizing: border-box; }
.halo-touch-stick-ring, .halo-touch-stick-knob { position: absolute; display: none; border-radius: 50%; pointer-events: none; box-sizing: border-box; }
.halo-touch-stick-ring { border: 2px solid rgba(215, 221, 229, 0.38); background: rgba(14, 17, 22, 0.18); }
.halo-touch-stick-knob { background: rgba(215, 221, 229, 0.5); border: 2px solid rgba(95, 179, 255, 0.7); }
.halo-touch-rotate { position: fixed; inset: 0; z-index: 20; display: none; flex-direction: column; align-items: center;
  justify-content: center; gap: 14px; background: #0e1116; color: #d7dde5; font: 600 18px/1.4 system-ui, sans-serif; text-align: center; padding: 24px; }
.halo-touch-rotate .phone { width: 46px; height: 78px; border: 3px solid #5fb3ff; border-radius: 9px; animation: halo-touch-rotate 2.2s ease-in-out infinite; }
.halo-touch-rotate small { color: #8b96a5; font-weight: 500; font-size: 14px; }
@keyframes halo-touch-rotate { 0%, 25% { transform: rotate(0deg); } 55%, 100% { transform: rotate(-90deg); } }
@media (orientation: portrait) { html.halo-touch .halo-touch-rotate { display: flex; } }
`;
		document.head.appendChild(style);
		const rotate = document.createElement("div");
		rotate.className = "halo-touch-rotate";
		rotate.setAttribute("role", "alert");
		rotate.innerHTML = '<div class="phone" aria-hidden="true"></div><div>Rotate your phone</div><small>The game plays in landscape.</small>';
		document.body.appendChild(rotate);
		// (no pinch, double-tap zoom nor pull-to-refresh, wherever the finger is)
		const stop = (event) => { if (event.cancelable) event.preventDefault(); };
		document.addEventListener("touchmove", stop, { passive: false });
		document.addEventListener("gesturestart", stop, { passive: false });
		document.addEventListener("dblclick", stop, { passive: false });
		// the first tap: fullscreen and landscape
		document.addEventListener("pointerdown", immersive, { capture: true, once: true });
	}

	function immersive() {
		counts.fullscreen++;
		if (embedded && (window.haloBridge || window.parent)) {
			// the wrapper makes its game container fullscreen and locks the
			// orientation (the tap's activation reaches the parent too)
			post("touch-fullscreen", { orientation: "landscape" });
			return;
		}
		const target = document.documentElement;
		const request = target.requestFullscreen ? target.requestFullscreen({ navigationUI: "hide" }) : null;
		Promise.resolve(request).then(() => {
			if (screen.orientation && screen.orientation.lock) return screen.orientation.lock("landscape");
			return null;
		}).catch(() => {});
	}

	// ---------- the controls

	function setting() {
		const scene = read(SCENE);
		if (!(scene & SCENE_KNOWN) || (scene & SCENE_OFF) || !(enabled || (scene & SCENE_ON))) return "hidden";
		if (scene & SCENE_MENUS) return "menus";
		if (scene & SCENE_ON) return "game";
		const pads = navigator.getGamepads ? [...navigator.getGamepads()].some((pad) => pad && pad.connected) : false;
		return pads ? "hidden" : "game";
	}

	// pixels per grid unit across, down, and for sizes
	function scales() {
		const width = overlay.clientWidth;
		const height = overlay.clientHeight;
		return { x: width / GRID_WIDTH, y: height / GRID_HEIGHT, size: Math.min(width / GRID_WIDTH, height / GRID_HEIGHT) };
	}

	function layout() {
		const scale = scales();
		for (const control of CONTROLS) {
			const radius = control.radius * scale.size;
			const top = control.y < 60;
			Object.assign(control.element.style, {
				left: `${control.x * scale.x - (top ? radius * 1.3 : radius)}px`,
				top: `${control.y * scale.y - radius * (top ? 0.8 : 1)}px`,
				width: `${(top ? radius * 2.6 : radius * 2)}px`,
				height: `${radius * (top ? 1.6 : 2)}px`,
				fontSize: `${Math.max(10, radius * 0.4)}px`,
			});
		}
		const radius = STICK_RADIUS * scale.size;
		Object.assign(stickHint.style, { left: `${100 * scale.x - radius}px`, top: `${420 * scale.y - radius}px`,
			width: `${radius * 2}px`, height: `${radius * 2}px` });
	}

	function labels() {
		const serial = read(BINDINGS_SERIAL);
		if (serial === bindingsSerial) return;
		bindingsSerial = serial;
		for (const control of CONTROLS) {
			if (control.chat) continue;
			const action = serial ? read(BINDINGS + control.button) : -1;
			const name = (menus && control.menuLabel) || control.label || GAME_CONTROLS[action] || control.key;
			control.element.querySelector(".name").textContent = name;
			control.element.querySelector(".key").textContent = control.label || name === control.key ? "" : control.key;
			control.element.classList.toggle("fire", action === 7);
		}
	}

	// the state the game reads: the stick, the buttons held, the triggers
	function write() {
		let buttons = 0;
		let left = 0;
		let right = 0;
		const down = new Set([...held.values(), ...latched]);
		for (const control of down) {
			if (control.bit !== undefined) buttons |= 1 << control.bit;
			if (control.trigger === 4) left = 32767;
			if (control.trigger === 5) right = 32767;
		}
		let x = 0;
		let y = 0;
		if (stick) {
			const scale = scales();
			const reach = STICK_RADIUS * scale.size * STICK_REACH;
			x = (stick.x - stick.centreX) / reach;
			y = (stick.y - stick.centreY) / reach;
			const length = Math.hypot(x, y);
			if (length < STICK_DEAD_ZONE) {
				x = y = 0;
			} else if (length > 1) {
				x /= length;
				y /= length;
			}
			const knob = STICK_RADIUS * scale.size * 0.45;
			const ring = STICK_RADIUS * scale.size;
			const knobX = stick.centreX + x * reach;
			const knobY = stick.centreY + y * reach;
			Object.assign(stickKnob.style, { left: `${(length > 1 ? knobX : stick.x) - knob}px`, top: `${(length > 1 ? knobY : stick.y) - knob}px`,
				width: `${knob * 2}px`, height: `${knob * 2}px`, display: "block" });
			Object.assign(stickRing.style, { left: `${stick.centreX - ring}px`, top: `${stick.centreY - ring}px`,
				width: `${ring * 2}px`, height: `${ring * 2}px`, display: "block" });
		} else {
			stickKnob.style.display = "none";
			stickRing.style.display = "none";
		}
		const state = [Math.round(x * 32767), Math.round(y * 32767), 0, 0, left, right, buttons];
		state.forEach((value, index) => Atomics.store(words, base + STATE + index, value));
	}

	function press(control) {
		const input = control.bit !== undefined ? control.bit : 15 + (control.trigger === 5 ? 1 : 0);
		Atomics.add(words, base + PRESSES + input, 1);
		control.element.classList.add("held");
		counts.taps++;
	}

	function controlAt(x, y) {
		const scale = scales();
		let best = null;
		let bestDistance = Infinity;
		for (const control of CONTROLS) {
			if (menus ? !(control.menu || control.menuOnly) : control.menuOnly) continue;
			const distance = Math.hypot(x - control.x * scale.x, y - control.y * scale.y);
			if (distance <= control.radius * scale.size * 1.2 && distance < bestDistance) {
				best = control;
				bestDistance = distance;
			}
		}
		return best;
	}

	function chat() {
		counts.chat++;
		release();
		// the wrapper's chat (its input takes the on-screen keyboard)
		post("chat-key", { touch: true });
	}

	function down(event) {
		const rect = overlay.getBoundingClientRect();
		const x = event.clientX - rect.left;
		const y = event.clientY - rect.top;
		const control = controlAt(x, y);
		event.preventDefault();
		event.stopPropagation();
		if (control && control.chat) {
			control.element.classList.add("held");
			setTimeout(() => control.element.classList.remove("held"), 150);
			chat();
			return;
		}
		if (menus && !control) return;
		try {
			overlay.setPointerCapture(event.pointerId);
		} catch (error) {
			// as it is
		}
		if (control && control.latch) {
			// Score: down until tapped again
			if (latched.has(control)) {
				latched.delete(control);
				control.element.classList.remove("held");
			} else {
				latched.add(control);
				press(control);
			}
			write();
			return;
		}
		if (!control && !stick && x < rect.width * FLOATING_ZONE_WIDTH && y > rect.height * FLOATING_ZONE_TOP) {
			// the stick, floating where the thumb went down
			stick = { pointer: event.pointerId, centreX: x, centreY: y, x, y };
			fingers.set(event.pointerId, { kind: "stick" });
			if (!stickUsed) {
				stickUsed = true;
				stickHint.style.display = "none";
			}
		} else if (control) {
			held.set(event.pointerId, control);
			press(control);
			fingers.set(event.pointerId, { kind: "button", control, x, y });
		} else {
			fingers.set(event.pointerId, { kind: "look", x, y });
		}
		write();
	}

	function move(event) {
		const finger = fingers.get(event.pointerId);
		if (!finger) return;
		event.preventDefault();
		const rect = overlay.getBoundingClientRect();
		const x = event.clientX - rect.left;
		const y = event.clientY - rect.top;
		if (finger.kind === "stick" && stick) {
			stick.x = x;
			stick.y = y;
			write();
		} else if (finger.kind === "look" || (finger.kind === "button" && !finger.control.latch)) {
			// (a button dragged turns the view too, as on Android: fire and aim)
			const size = scales().size;
			Atomics.add(words, base + LOOK, Math.round((x - finger.x) / size * 256));
			Atomics.add(words, base + LOOK + 1, Math.round((y - finger.y) / size * 256));
			finger.x = x;
			finger.y = y;
		}
	}

	function up(event) {
		const finger = fingers.get(event.pointerId);
		if (!finger) return;
		fingers.delete(event.pointerId);
		if (finger.kind === "stick") stick = null;
		if (finger.kind === "button") {
			held.delete(event.pointerId);
			if (![...held.values()].includes(finger.control)) finger.control.element.classList.remove("held");
		}
		write();
	}

	function release() {
		fingers.clear();
		held.clear();
		latched.clear();
		stick = null;
		for (const control of CONTROLS) control.element.classList.remove("held");
		if (words) write();
	}

	function frame() {
		const want = setting();
		const wantShown = want !== "hidden";
		const wantMenus = want === "menus";
		if (wantShown !== shown || wantMenus !== menus) {
			// (whatever the fingers held does not carry into a menu or out of it)
			if (wantMenus !== menus || !wantShown) release();
			shown = wantShown;
			menus = wantMenus;
			overlay.hidden = !shown;
			overlay.classList.toggle("menus", menus);
			bindingsSerial = -1; // (A and B read Select and Back in a menu)
			labels();
			if (shown) layout();
		}
		if (shown) {
			// (the screen's fingers aim, not a mouse)
			if (document.pointerLockElement) document.exitPointerLock();
			labels();
			const rumble = read(RUMBLE);
			const now = performance.now();
			if (rumble && !menus && navigator.vibrate && now - lastVibration > VIBRATION_RENEW_MS) {
				try { navigator.vibrate(VIBRATION_MS); } catch (error) { /* (not allowed yet) */ }
				lastVibration = now;
			}
		}
		requestAnimationFrame(frame);
	}

	// starts the controls over the game once its runtime is up (module:
	// the loader's Module, with _web_state and HEAPU32)
	function start(module) {
		if (overlay || !module || !module._web_state || !module.HEAPU32) return false;
		words = new Int32Array(module.HEAPU32.buffer);
		base = module._web_state() >> 2;
		overlay = document.createElement("div");
		overlay.id = "halo-touch";
		overlay.className = "halo-touch-overlay";
		overlay.hidden = true;
		for (const control of CONTROLS) {
			control.element = document.createElement("div");
			control.element.className = "halo-touch-control" + (control.y < 60 ? " top" : "") + (control.menu ? " menu" : "") +
				(control.menuOnly ? " menu-only" : "");
			control.element.innerHTML = '<span class="name"></span><span class="key"></span>';
			control.element.querySelector(".name").textContent = control.label || control.key;
			if (control.title) control.element.title = control.title;
			overlay.append(control.element);
		}
		stickHint = document.createElement("div");
		stickHint.className = "halo-touch-stick-hint";
		stickHint.textContent = "Move";
		stickRing = document.createElement("div");
		stickRing.className = "halo-touch-stick-ring";
		stickKnob = document.createElement("div");
		stickKnob.className = "halo-touch-stick-knob";
		overlay.append(stickHint, stickRing, stickKnob);
		overlay.addEventListener("pointerdown", down);
		overlay.addEventListener("pointermove", move);
		overlay.addEventListener("pointerup", up);
		overlay.addEventListener("pointercancel", up);
		overlay.addEventListener("contextmenu", (event) => event.preventDefault());
		// (the overlay's fingers are not the mouse's, which the game would take
		// as aiming and firing)
		for (const type of ["mousedown", "mousemove", "mouseup", "wheel", "click"]) {
			overlay.addEventListener(type, (event) => event.stopPropagation());
		}
		document.body.appendChild(overlay);
		addEventListener("resize", () => shown && layout());
		addEventListener("orientationchange", () => { release(); setTimeout(() => shown && layout(), 200); });
		addEventListener("blur", release);
		document.addEventListener("visibilitychange", () => document.hidden && release());
		requestAnimationFrame(frame);
		return true;
	}

	// what the controls see and did (for tests)
	function state() {
		if (!words) return { enabled, started: false };
		return {
			enabled, started: true, shown, menus, scene: read(SCENE),
			stick: [read(STATE), read(STATE + 1)], buttons: read(STATE + 6), triggers: [read(STATE + 4), read(STATE + 5)],
			looked: read(LOOKED), assisted: read(ASSISTED), bindingsSerial: read(BINDINGS_SERIAL),
			labels: CONTROLS.map((control) => control.element.textContent), counts: Object.assign({}, counts),
		};
	}

	if (enabled) {
		if (document.body) preparePage();
		else document.addEventListener("DOMContentLoaded", preparePage);
		// the runtime comes up after halo.js loads (the loader's Module is
		// there already): start then; _web_state must not be called before
		const module = window.Module;
		if (module) {
			const initialized = module.onRuntimeInitialized;
			module.onRuntimeInitialized = function () {
				const result = initialized ? initialized.apply(this, arguments) : undefined;
				start(module);
				return result;
			};
		}
	}

	return { enabled, start, state };
})();
