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
//   prefetch=<a,b> maps fetched from the start alongside the preloaded ones,
//                 without holding up the start (the site passes the map of
//                 the server the player picked: it is there by the time the
//                 lobby names it)
//
// The maps are kept in the browser between visits when halo-maps.js (loaded
// before this script) is there: window.haloMaps, Cache Storage keyed by each
// map's name and version (index.json's sha256, else its size). A page that
// embeds the game may start the same downloads first (haloMaps.prefetch):
// the game then waits for those and reads them from the storage.
//
// Diagnostics, given with env= (the site's /play passes env= through):
//   HALO_WEB_DPR=<n>   the device pixel ratio the game sees (1: a drawing
//                      buffer of the canvas's CSS size on a Retina screen;
//                      a phone or tablet has 1 unless this says otherwise)
//   HALO_GL_DEBUG=1, HALO_GPU_STATS=1   the renderer's GL errors, and its draws
//                      (and those it skipped: no program, a link failure)
//                      each second, in the console (port_config.c)
//   HALO_GPU_CONSTANT_BUFFER=1   vertex shaders read their constants from a
//                      uniform buffer, as they do by themselves where the
//                      driver drops part of the c[] array (d3d8_gl.c)
//   HALO_WEB_GPU_OVERLAY=1 (or HALO_GL_DEBUG=1)  the GPU overlay over the
//                      game (below: the GPU, its extensions and limits, the
//                      GPU check, GL errors, shader failures, draws by
//                      vertex format); a tap folds it
//   HALO_WEB_CPU_DECODE_VERTICES=<auto|small|1|0>  vertices decoded into
//                      floats on the CPU (d3d8_gl.c; auto: small on PowerVR)
//   HALO_WEB_FORCE_DXT_DECODE=1  DXT textures decompressed on the CPU
//   HALO_WEB_NO_ASTC=1   where there is ASTC and no S3TC (PowerVR, Mali), DXT1
//                      textures decompressed on the CPU instead of given
//                      as ASTC 4x4 (port/linux/src/dxt1_astc.h)
//   HALO_WEB_GPU_CHECK=0   no GPU check (below) and none of its workarounds
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
//   HALO_WEB_TOUCH_CONTROLS=<auto|on|off>  the on-screen touch controls
//                                  (halo-touch.js): auto shows them on a
//                                  phone or tablet without a gamepad
//   HALO_WEB_TOUCH_AIM_ASSIST=<0|1>  0: the touch swipe gets no aim assist
//   HALO_WEB_TOUCH_SENSITIVITY=<x>  the touch swipe's multiplier, 0.1-10 (1)
//   HALO_WEB_EXIT_URL=<url>        where the page goes when the player
//                                  leaves the game (default: stay)
//   HALO_WEB_MENUS=1               the multiplayer-only build's menus back
//                                  (development)
// Scoreboard settings (port_config.c display.scoreboard_*), given with env=;
// the scoreboard opens while BACK or F1 is held and scrolls with the mouse
// wheel, Page Up / Page Down or the gamepad's d-pad:
//   HALO_SCOREBOARD_TEAM_LAYOUT=<teams|score>  a team game's players in a
//                                  column per team (teams) or by score
//   HALO_SCOREBOARD_BACKGROUND=<0|1>  0: no panel behind it (1)
//   HALO_SCOREBOARD_BACKGROUND_COLOR=<r g b a>  the panel's colour, each
//                                  0-255, spaces between (16 16 16 150)
// What this build understands, for the site's game bridge (which runs before
// the game starts, after this script): webJoin, the web.join setting
// (HALO_WEB_JOIN=first, port/linux/game/auto_join.c); playerColor,
// HALO_WEB_PLAYER_COLOR; playerTeam, HALO_WEB_PLAYER_TEAM; leave, the "halo:leave" event: the build has no
// main menu, it boots into the join and, when the player leaves the game,
// asks the page to leave (web_library.js, web_leave_game); inputSettings,
// HALO_WEB_LOOK_SENSITIVITY, HALO_WEB_INVERT_LOOK, HALO_WEB_STICK_DEADZONE,
// HALO_WEB_VIBRATION and HALO_WEB_MOUSE_SENSITIVITY; gamepadEvents, the
// "halo:gamepad" event (below); versionMismatch, the "halo:leave" event's
// "version_mismatch" reason ({hostVersion, clientVersion}) and ?v=<build> on
// this script's URL passed on to halo.wasm (halo-version.json has the
// build's network version); touchControls, halo-touch.js (the
// "touch-fullscreen" and "chat-key" {touch: true} messages to the wrapper).
window.haloFeatures = Object.assign(window.haloFeatures || {}, { webJoin: true, playerColor: true, playerTeam: true, leave: true,
	inputSettings: true, gamepadEvents: true, versionMismatch: true, touchControls: true });

// The GPU check and the debug overlay (docs/wasm-spike.md "GPU check").
// Before the game starts, a small WebGL 2 context of its own draws what the
// game's models need and reads it back: the models' vertex layout (32 bytes:
// a float3 position, three NORMPACKED3 uints unpacked with integer shifts,
// NORMSHORT2 texture coordinates, PBYTE2 node indices and a NORMSHORT1
// weight at bytes 24, 28 and 30), c[] indexed by a node index (skinning; a
// uniform array and a uniform block) and a DXT1 texture. A failed check
// turns on the game's workaround for it unless env= names that setting:
//   vertex formats (shorts, bytes)  HALO_WEB_CPU_DECODE_VERTICES=small
//   NORMPACKED3 (integer attributes)  HALO_WEB_CPU_DECODE_VERTICES=1
//   c[] indexed (array; block works)  HALO_GPU_CONSTANT_BUFFER=1
//   DXT1                            HALO_WEB_FORCE_DXT_DECODE=1
// (HALO_WEB_GPU_CHECK=0: no check). The overlay, with env=HALO_GL_DEBUG=1 or
// HALO_WEB_GPU_OVERLAY=1, shows the GPU, its extensions and limits, the
// check, and from the game's log its GL errors, shader compile and link
// failures and draws by vertex format (HALO_GPU_STATS, which it turns on).
window.haloGpuCheck = (() => {
	const glsl = (body) => "#version 300 es\nprecision highp float;\nprecision highp int;\n" + body;
	function compile(gl, type, source, log) {
		const shader = gl.createShader(type);
		gl.shaderSource(shader, source);
		gl.compileShader(shader);
		if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
			log.push(`compile: ${gl.getShaderInfoLog(shader)}`);
		return shader;
	}
	function program(gl, vertex, log, bind) {
		const p = gl.createProgram();
		gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, glsl(vertex), log));
		gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, glsl("in vec4 color;\nout vec4 fragment;\nvoid main() { fragment = color; }\n"), log));
		if (bind) bind(p);
		gl.linkProgram(p);
		if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
			log.push(`link: ${gl.getProgramInfoLog(p)}`);
			return null;
		}
		return p;
	}
	// one point per vertex at pixel x = vertex of a 16x1 target; the colours read back
	function drawPoints(gl, count, draw) {
		// (on a unit of its own: unit 0 has checkDxt's texture)
		gl.activeTexture(gl.TEXTURE7);
		const target = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, target);
		gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 16, 1);
		gl.activeTexture(gl.TEXTURE0);
		const framebuffer = gl.createFramebuffer();
		gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
		gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
		gl.viewport(0, 0, 16, 1);
		gl.clearColor(0, 0, 0, 0);
		gl.clear(gl.COLOR_BUFFER_BIT);
		draw();
		const pixels = new Uint8Array(16 * 4);
		gl.readPixels(0, 0, 16, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
		gl.deleteFramebuffer(framebuffer);
		gl.deleteTexture(target);
		return Array.from({ length: count }, (_, i) => Array.from(pixels.subarray(i * 4, i * 4 + 4)));
	}
	const near = (got, want, tolerance = 3) => got.every((value, i) => Math.abs(value - want[i]) <= tolerance);
	const pointX = "gl_PointSize = 1.0;\n\tgl_Position = vec4((position.x + 0.5) / 8.0 - 1.0, 0.0, 0.0, 1.0);\n";
	const COUNT = 8;
	const packed = (x, y, z) => ((x & 0x7ff) | ((y & 0x7ff) << 11) | ((z & 0x3ff) << 22)) >>> 0;

	// the models' vertices, read as the game reads them (d3d8_gl.c
	// web_setup_streams, nv2a_vsh.c unpack_normpacked3)
	function checkVertices(gl, log) {
		const stride = 32, data = new ArrayBuffer(COUNT * stride), view = new DataView(data);
		const want = { packed: [], short2: [], byte2: [], short1: [] };
		for (let i = 0; i < COUNT; i++) {
			const o = i * stride;
			view.setFloat32(o, i, true);
			view.setFloat32(o + 4, 0, true);
			view.setFloat32(o + 8, 0, true);
			const nx = -1000 + i * 250, ny = 900 - i * 230, nz = -500 + i * 140;
			for (const at of [12, 16, 20]) view.setUint32(o + at, packed(nx, ny, nz), true);
			want.packed.push([nx / 1023, ny / 1023, nz / 511].map((v) => Math.round((v * 0.5 + 0.5) * 255)).concat(255));
			const s0 = -32000 + i * 9000, s1 = 30000 - i * 7000;
			view.setInt16(o + 24, s0, true);
			view.setInt16(o + 26, s1, true);
			want.short2.push([s0 / 32767, s1 / 32767].map((v) => Math.round((v * 0.5 + 0.5) * 255)).concat(0, 255));
			view.setUint8(o + 28, i * 3);
			view.setUint8(o + 29, 255 - i * 30);
			want.byte2.push([i * 3, 255 - i * 30, 0, 255]);
			const w = 4000 * i;
			view.setInt16(o + 30, w, true);
			want.short1.push([Math.round((w / 32767) * 255), 0, 0, 255]);
		}
		const vertex = `layout(location = 0) in vec4 position;
layout(location = 1) in uint v1_packed;
layout(location = 2) in uint v2_packed;
layout(location = 3) in uint v3_packed;
layout(location = 4) in vec4 v4;
layout(location = 5) in vec4 v5;
layout(location = 6) in vec4 v6;
uniform int which;
out vec4 color;
vec4 unpack_normpacked3(uint p)
{
	int x = int(p << 21) >> 21;
	int y = int(p << 10) >> 21;
	int z = int(p) >> 22;
	return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);
}
void main()
{
	vec4 n = unpack_normpacked3(which == 0 ? v1_packed : which == 1 ? v2_packed : v3_packed);
	if (which <= 2) color = vec4(n.xyz * 0.5 + 0.5, 1.0);
	else if (which == 3) color = vec4(v4.xy * 0.5 + 0.5, 0.0, 1.0);
	else if (which == 4) color = vec4(v5.xy, 0.0, 1.0);
	else color = vec4(v6.x, 0.0, 0.0, 1.0);
	${pointX}}
`;
		const p = program(gl, vertex, log);
		if (!p) return { ok: false, detail: "the program did not link" };
		const vao = gl.createVertexArray();
		gl.bindVertexArray(vao);
		const buffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
		// exactly the vertices, as the game uploads them (no padding)
		gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW);
		gl.enableVertexAttribArray(0);
		gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, 0);
		for (const [index, at] of [[1, 12], [2, 16], [3, 20]]) {
			gl.enableVertexAttribArray(index);
			gl.vertexAttribIPointer(index, 1, gl.UNSIGNED_INT, stride, at);
		}
		gl.enableVertexAttribArray(4);
		gl.vertexAttribPointer(4, 2, gl.SHORT, true, stride, 24);
		gl.enableVertexAttribArray(5);
		gl.vertexAttribPointer(5, 2, gl.UNSIGNED_BYTE, true, stride, 28);
		gl.enableVertexAttribArray(6);
		gl.vertexAttribPointer(6, 1, gl.SHORT, true, stride, 30);
		const indices = gl.createBuffer();
		gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indices);
		gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([...Array(COUNT).keys()]), gl.STREAM_DRAW);
		gl.useProgram(p);
		const results = {}, names = ["packed", "packed", "packed", "short2", "byte2", "short1"];
		for (let which = 0; which < 6; which++) {
			const got = drawPoints(gl, COUNT, () => {
				gl.uniform1i(gl.getUniformLocation(p, "which"), which);
				gl.drawElements(gl.POINTS, COUNT, gl.UNSIGNED_SHORT, 0);
			});
			const name = names[which], ok = got.every((pixel, i) => near(pixel, want[name][i]));
			if (!ok) log.push(`${name}${which <= 2 ? "@" + (12 + which * 4) : ""}: got ${JSON.stringify(got.slice(0, 3))} want ${JSON.stringify(want[name].slice(0, 3))}`);
			results[name] = results[name] === false ? false : ok;
		}
		gl.bindVertexArray(null);
		gl.deleteVertexArray(vao);
		gl.deleteBuffer(buffer);
		gl.deleteBuffer(indices);
		gl.deleteProgram(p);
		return results;
	}

	// c[] (192 vec4s, as nv2a_vsh.c declares it) indexed by a node index
	// from a PBYTE2 attribute, as the models' vertex shaders do
	function checkRelative(gl, log, block) {
		const constants = new Float32Array(192 * 4);
		const nodes = [0, 9, 20, 31, 40, 52, 60, 63];
		for (let r = 0; r < 192; r++) constants.set([(r % 7) / 6, (r % 11) / 10, (r % 5) / 4, 1], r * 4);
		const data = new Float32Array(COUNT * 2);
		nodes.forEach((node, i) => { data[i * 2] = i; data[i * 2 + 1] = node; });
		const vertex = `layout(location = 0) in vec4 position;
${block ? "layout(std140) uniform vertex_constants { vec4 c[192]; };" : "uniform vec4 c[192];"}
out vec4 color;
void main()
{
	float a0 = position.y * 3.0 + 1.0;
	color = c[clamp(int(floor(a0 + 0.5)), 0, 191)] + c[clamp(int(floor(a0 + 0.5)) + 2, 0, 191)] * 0.0;
	${pointX}}
`;
		const p = program(gl, vertex, log);
		if (!p) return { ok: false, detail: "the program did not link" };
		gl.useProgram(p);
		let ubo = null;
		if (block) {
			ubo = gl.createBuffer();
			gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
			gl.bufferData(gl.UNIFORM_BUFFER, constants, gl.DYNAMIC_DRAW);
			gl.uniformBlockBinding(p, gl.getUniformBlockIndex(p, "vertex_constants"), 0);
			gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, ubo);
		} else {
			gl.uniform4fv(gl.getUniformLocation(p, "c"), constants);
		}
		const vao = gl.createVertexArray();
		gl.bindVertexArray(vao);
		const buffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
		gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW);
		gl.enableVertexAttribArray(0);
		gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
		const got = drawPoints(gl, COUNT, () => gl.drawArrays(gl.POINTS, 0, COUNT));
		const want = nodes.map((node) => { const r = node * 3 + 1; return [(r % 7) / 6, (r % 11) / 10, (r % 5) / 4, 1].map((v) => Math.round(v * 255)); });
		const ok = got.every((pixel, i) => near(pixel, want[i]));
		if (!ok) log.push(`c[a0] ${block ? "block" : "array"}: got ${JSON.stringify(got.slice(0, 4))} want ${JSON.stringify(want.slice(0, 4))}`);
		const uniforms = !block && gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
		let size = 0;
		for (let u = 0; !block && u < uniforms; u++) {
			const info = gl.getActiveUniform(p, u);
			if (info && /^c(\[0\])?$/.test(info.name)) size = info.size;
		}
		gl.bindVertexArray(null);
		gl.deleteVertexArray(vao);
		gl.deleteBuffer(buffer);
		if (ubo) gl.deleteBuffer(ubo);
		gl.deleteProgram(p);
		return block ? { ok } : { ok, size };
	}

	// a red DXT1 block, sampled
	function checkDxt(gl, log) {
		const s3tc = gl.getExtension("WEBGL_compressed_texture_s3tc");
		if (!s3tc) return { ok: null, detail: "no WEBGL_compressed_texture_s3tc (the game decodes DXT on the CPU, or gives DXT1 as ASTC where there is WEBGL_compressed_texture_astc)" };
		const texture = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, texture);
		gl.compressedTexImage2D(gl.TEXTURE_2D, 0, s3tc.COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4, 0, new Uint8Array([0x00, 0xf8, 0x00, 0xf8, 0, 0, 0, 0]));
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
		const error = gl.getError();
		const p = program(gl, `layout(location = 0) in vec4 position;
uniform sampler2D map;
out vec4 color;
void main() { color = textureLod(map, vec2(0.5), 0.0); ${pointX}}
`, log);
		if (!p) return { ok: false, detail: "the program did not link" };
		gl.useProgram(p);
		const vao = gl.createVertexArray();
		gl.bindVertexArray(vao);
		const buffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
		gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0]), gl.STREAM_DRAW);
		gl.enableVertexAttribArray(0);
		gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
		const got = drawPoints(gl, 1, () => gl.drawArrays(gl.POINTS, 0, 1))[0];
		const ok = !error && near(got, [255, 0, 0, 255], 8);
		if (!ok) log.push(`DXT1: error ${error}, got ${JSON.stringify(got)} want [255,0,0,255]`);
		gl.bindVertexArray(null);
		gl.deleteVertexArray(vao);
		gl.deleteBuffer(buffer);
		gl.deleteTexture(texture);
		gl.deleteProgram(p);
		return { ok };
	}

	function run() {
		const report = { renderer: "", vendor: "", version: "", extensions: [], limits: {}, precision: {}, checks: {}, log: [], fixes: {} };
		let gl = null;
		try {
			const canvas = typeof OffscreenCanvas === "function" ? new OffscreenCanvas(16, 1) : document.createElement("canvas");
			gl = canvas.getContext("webgl2", { antialias: false, depth: false, stencil: false, preserveDrawingBuffer: true });
			if (!gl) {
				report.log.push("no WebGL 2 context");
				return report;
			}
			const info = gl.getExtension("WEBGL_debug_renderer_info");
			report.renderer = String(info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER));
			report.vendor = String(info ? gl.getParameter(info.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR));
			report.version = String(gl.getParameter(gl.VERSION));
			report.extensions = gl.getSupportedExtensions() || [];
			for (const name of ["MAX_VERTEX_ATTRIBS", "MAX_VERTEX_UNIFORM_VECTORS", "MAX_FRAGMENT_UNIFORM_VECTORS", "MAX_VARYING_VECTORS",
				"MAX_VERTEX_UNIFORM_BLOCKS", "MAX_UNIFORM_BLOCK_SIZE", "MAX_TEXTURE_SIZE", "MAX_COMBINED_TEXTURE_IMAGE_UNITS"])
				report.limits[name] = gl.getParameter(gl[name]);
			for (const [stage, type] of [["vs", gl.VERTEX_SHADER], ["fs", gl.FRAGMENT_SHADER]]) {
				for (const [kind, format] of [["float", gl.HIGH_FLOAT], ["int", gl.HIGH_INT]]) {
					const p = gl.getShaderPrecisionFormat(type, format);
					report.precision[`${stage} highp ${kind}`] = p ? `${p.precision}/${p.rangeMax}` : "?";
				}
			}
			const vertices = checkVertices(gl, report.log);
			report.checks.packed = vertices.packed;
			report.checks.short2 = vertices.short2;
			report.checks.byte2 = vertices.byte2;
			report.checks.short1 = vertices.short1;
			const array = checkRelative(gl, report.log, false);
			report.checks.relativeArray = array.ok;
			report.limits.cArraySize = array.size;
			report.checks.relativeBlock = checkRelative(gl, report.log, true).ok;
			const dxt = checkDxt(gl, report.log);
			report.checks.dxt1 = dxt.ok;
			if (dxt.detail) report.log.push(dxt.detail);
			const error = gl.getError();
			if (error) report.log.push(`GL error ${error} after the check`);
		} catch (error) {
			report.log.push(`check: ${error}`);
		} finally {
			const lose = gl && gl.getExtension("WEBGL_lose_context");
			if (lose) lose.loseContext();
		}
		const c = report.checks;
		if (c.packed === false) report.fixes.HALO_WEB_CPU_DECODE_VERTICES = "1";
		else if (c.short2 === false || c.byte2 === false || c.short1 === false) report.fixes.HALO_WEB_CPU_DECODE_VERTICES = "small";
		if (c.relativeArray === false && c.relativeBlock !== false) report.fixes.HALO_GPU_CONSTANT_BUFFER = "1";
		if (c.dxt1 === false) report.fixes.HALO_WEB_FORCE_DXT_DECODE = "1";
		return report;
	}

	// the overlay
	let panel = null, report = null;
	const seen = { errors: [], shaders: [], formats: [], notes: [] };
	let lastFormats = "", lastFrame = "", decoding = "";
	function overlay(r) {
		report = r;
		if (!document.body) return;
		// below the touch controls' Menu, Score and Chat (top centre), and
		// taking no touches (the look swipe goes through it); its tab folds it
		panel = document.createElement("div");
		panel.id = "halo-gpu-overlay";
		panel.style.cssText = "position:fixed;top:max(64px,14vh);left:50%;transform:translateX(-50%);z-index:2147483000;" +
			"width:min(62vw,760px);max-height:44vh;overflow:hidden;background:rgba(6,10,14,0.8);color:#9fe3ff;font:10px/1.3 monospace;" +
			"padding:4px 6px;border:1px solid #2f5f7a;border-radius:4px;white-space:pre-wrap;word-break:break-word;pointer-events:none;box-sizing:border-box";
		const tab = document.createElement("button");
		tab.textContent = "GPU";
		tab.style.cssText = "position:fixed;top:max(64px,14vh);right:calc(50% - min(31vw,380px));z-index:2147483001;font:10px monospace;" +
			"color:#9fe3ff;background:rgba(6,10,14,0.8);border:1px solid #2f5f7a;border-radius:4px;padding:4px 6px;touch-action:manipulation";
		let folded = false;
		const fold = (event) => { event.preventDefault(); event.stopPropagation(); folded = !folded; panel.style.display = folded ? "none" : ""; };
		tab.addEventListener("click", fold);
		tab.addEventListener("touchend", fold);
		document.body.appendChild(panel);
		document.body.appendChild(tab);
		render();
		setInterval(render, 1000);
	}
	const mark = (value) => value === true ? "ok" : value === false ? "FAIL" : "n/a";
	function render() {
		if (!panel || !report) return;
		const want = ["WEBGL_compressed_texture_s3tc", "WEBGL_compressed_texture_s3tc_srgb", "WEBGL_compressed_texture_etc1",
			"WEBGL_compressed_texture_etc", "WEBGL_compressed_texture_astc", "EXT_color_buffer_float", "EXT_texture_filter_anisotropic",
			"OES_texture_float_linear", "WEBGL_debug_renderer_info"];
		const c = report.checks;
		// (what matters most first: a phone shows the top of it)
		const lines = [
			`GPU ${report.renderer} (${report.vendor})`,
			`check: NORMPACKED3 ${mark(c.packed)}, NORMSHORT2 ${mark(c.short2)}, PBYTE2@28 ${mark(c.byte2)}, NORMSHORT1@30 ${mark(c.short1)}, ` +
				`c[a0] array ${mark(c.relativeArray)}, c[a0] block ${mark(c.relativeBlock)}, DXT1 ${mark(c.dxt1)}`,
			`fixes: ${Object.keys(report.applied || {}).length ? Object.entries(report.applied).map(([k, v]) => `${k}=${v}`).join(" ") : "none"}` +
				(report.env ? `  env: ${report.env}` : ""),
			...report.log.slice(0, 8).map((line) => `  ${line}`),
		];
		if (decoding) lines.push(decoding);
		if (lastFrame) lines.push(lastFrame);
		if (lastFormats) lines.push(lastFormats + "  (F floats, R c[a0], P NORMPACKED3, S shorts, B bytes)");
		if (seen.errors.length) lines.push(`GL errors (${seen.errors.length} unique):`, ...seen.errors.map((line) => `  ${line}`));
		if (seen.shaders.length) lines.push("shader failures:", ...seen.shaders.map((line) => `  ${line}`));
		lines.push(`${report.version}; ext: ${want.map((name) => (report.extensions.includes(name) ? "+" : "-") + name.replace(/^(WEBGL|EXT|OES)_/, "")).join(" ")}`,
			`limits: ${Object.entries(report.limits).map(([k, v]) => `${k.replace(/^MAX_/, "")}=${v}`).join(" ")}`,
			`precision: ${Object.entries(report.precision).map(([k, v]) => `${k} ${v}`).join(", ")}`);
		if (seen.notes.length) lines.push("game:", ...seen.notes.map((line) => `  ${line}`));
		if (seen.formats.length) lines.push("vertex formats:", ...seen.formats.map((line) => `  ${line}`));
		panel.textContent = lines.join("\n");
	}
	// a line of the game's log
	function line(text) {
		if (!panel || typeof text !== "string") return;
		const add = (list, value, max) => { if (!list.includes(value) && list.length < max) list.push(value); };
		let match;
		if (/GL error|gl error/i.test(text)) add(seen.errors, text.slice(0, 200), 20);
		else if (/cannot (compile|link)/.test(text)) add(seen.shaders, text.slice(0, 600), 6);
		else if ((match = /draw formats \(drawn\/skipped\):(.*)/.exec(text))) lastFormats = `draws/60 frames (drawn/skipped):${match[1]}`;
		else if ((match = /^.*?(frame \d+: .*)$/.exec(text)) && /skipped/.test(text)) lastFrame = match[1];
		else if ((match = /draw format: (.*)/.exec(text))) add(seen.formats, match[1].slice(0, 220), 40);
		else if ((match = /web: GPU .*; (vertices decoded .*)$/.exec(text))) decoding = match[1];
		else if (/OpenGL|S3TC|ASTC|DXT|vertices decoded|constants|uniform buffer|trimmed/i.test(text)) add(seen.notes, text.slice(0, 240), 12);
	}
	return { run, overlay, line };
})();
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
	// A phone or tablet (a coarse primary pointer with touch: halo-touch.js)
	// draws at a device pixel ratio of 1, a drawing buffer of the canvas's
	// CSS size: its 2.5-3x screen would cost the GPU 6-9 times the pixels
	// for little gain at the Xbox's detail. HALO_WEB_DPR wins.
	const phone = typeof matchMedia === "function" && matchMedia("(pointer: coarse)").matches &&
		("ontouchstart" in window || navigator.maxTouchPoints > 0);
	const dpr = Number(envParam("HALO_WEB_DPR")) || (phone ? 1 : 0);
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
		window.haloGpuCheck.line(text);
		if (logElement) logElement.textContent = lines.join("\n");
	};
	window.haloLog = lines;

	// The build's version (the site adds ?v=<build> to this script's URL and
	// to halo.js's, docs/gateway.md "Versions"): the same on halo.wasm's,
	// so that a browser never runs a cached wasm of another build with this
	// halo.js (their names are not hashed)
	const buildVersion = (() => {
		try {
			return new URL(document.currentScript.src).searchParams.get("v");
		} catch (error) {
			return null;
		}
	})();
	window.haloBuildVersion = buildVersion;

	var Module = window.Module = {
		canvas: document.getElementById("canvas"),
		locateFile: (path, prefix) => (buildVersion && /\.(wasm|js)$/.test(path)
			? `${prefix}${path}?v=${encodeURIComponent(buildVersion)}` : prefix + path),
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
			// the GPU check (above): its workarounds where env= does not
			// name the setting
			if (envParam("HALO_WEB_GPU_CHECK") !== "0") {
				const report = window.haloGpuCheck.run();
				report.applied = {};
				for (const [name, value] of Object.entries(report.fixes)) {
					if (envParam(name) === null) {
						Module.ENV[name] = value;
						report.applied[name] = value;
					}
				}
				report.env = Object.entries(Module.ENV).filter(([name]) => /^HALO_(WEB_CPU|WEB_FORCE|GPU_|GL_)/.test(name))
					.map(([name, value]) => `${name}=${value}`).join(" ");
				const c = report.checks;
				print(`[web] GPU check on ${report.renderer}: ${Object.entries(c).map(([name, ok]) => `${name} ${ok === true ? "ok" : ok === false ? "FAIL" : "n/a"}`).join(", ")}` +
					`${Object.keys(report.applied).length ? "; turned on " + Object.entries(report.applied).map(([k, v]) => `${k}=${v}`).join(" ") : ""}`);
				for (const line of report.log) print(`[web] GPU check: ${line}`);
				window.haloGpuReport = report;
				if (envParam("HALO_GL_DEBUG") === "1" || envParam("HALO_WEB_GPU_OVERLAY") === "1") {
					if (envParam("HALO_GPU_STATS") === null) Module.ENV.HALO_GPU_STATS = "1";
					window.haloGpuCheck.overlay(report);
				}
			}
			Module.addRunDependency("maps-index");
			const preload = (params.get("preload") || "ui").toLowerCase();
			const prefetch = (params.get("prefetch") || "").toLowerCase().split(",").filter((name) => /^[a-z0-9_\-]{1,64}$/.test(name));
			const fetching = {};
			let maps = [];
			// maps/<name>.map into the file system once: true when it is there
			const fetchMap = (map) => fetching[map.name] || (fetching[map.name] = (async () => {
				const started = performance.now();
				let data, from = "network";
				if (window.haloMaps) {
					({ data, from } = await window.haloMaps.get(mapsUrl, map));
				} else {
					const response = await fetch(mapsUrl + map.name);
					if (!response.ok) throw new Error(`${map.name}: HTTP ${response.status}`);
					data = new Uint8Array(await response.arrayBuffer());
				}
				FS.writeFile("/data/maps/" + map.name, data);
				print(`[web] fetched ${map.name} (${(data.length / 1048576).toFixed(0)} MB, from the ${from === "storage" ? "browser's storage" : "network"} in ${Math.round(performance.now() - started)} ms)`);
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
				// (the prefetched maps go alongside: not waited for here)
				for (const map of maps.filter((entry) => prefetch.includes(entry.name.toLowerCase().replace(/\.map$/, ""))))
					fetchMap(map).catch((error) => { delete fetching[map.name]; print(`[web] cannot prefetch ${map.name}: ${error}`); });
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
				// the stored versions of maps the index no longer has (a map
				// updated or removed), once the game is under way
				if (window.haloMaps) setTimeout(() => window.haloMaps.forget(mapsUrl, maps).then((count) => {
					if (count) print(`[web] removed ${count} old map version${count === 1 ? "" : "s"} from the browser's storage`);
				}, () => {}), 30000);
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
	// top centre, below the page's own corner buttons and stats panel
	notice.style.cssText = "position: fixed; left: 50%; top: 96px; transform: translateX(-50%); z-index: 10; pointer-events: none; padding: 6px 12px;"
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

// A host of another network version (web_library.js, web_leave_game:
// "version_mismatch"): a host newer than this build means the page has a
// cached older build, so it reloads, once a minute at most for one pair of
// versions (sessionStorage), so that a server really ahead of the site's
// build does not reload the page for ever. True when it reloads.
function reloadForVersion(detail) {
	if (!(detail.hostVersion > detail.clientVersion)) return false;
	const key = `halo.version-reload.${detail.clientVersion}.${detail.hostVersion}`;
	try {
		const last = Number(sessionStorage.getItem(key));
		if (last && Date.now() - last < 60000) return false;
		sessionStorage.setItem(key, String(Date.now()));
	} catch (error) {
		return false;
	}
	location.reload();
	return true;
}
window.haloReloadForVersion = reloadForVersion;

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
		} else if (event.detail && event.detail.reason === "version_mismatch" && reloadForVersion(event.detail)) {
			// (reloading: a cached build older than the server)
		} else if (window.Module && window.Module.setStatus) {
			window.Module.setStatus(event.detail && event.detail.reason === "version_mismatch"
				? `This server runs network version ${event.detail.hostVersion}, this page ${event.detail.clientVersion}: the server is being updated`
				: event.detail && event.detail.reason === "no_game" ? "No game to join" : "You left the game");
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
