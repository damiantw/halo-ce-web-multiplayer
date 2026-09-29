// The web build's page glue (docs/wasm-spike.md): mounts the maps from an
// HTTP URL as d:\maps (lazily, a file is fetched when the game first reads
// it), picks the save root, and forwards the game's log to the page.
//
// Query parameters:
//   maps=<url>    where the maps are (default: maps/ next to the page); the
//                 server must answer maps/index.json with [{name, size}]
//   init=<text>   init.txt's contents (console commands at start-up)
//   env=A=1,B=2   environment variables (the HALO_* settings, port_config.c)
//   preload=<a,b> the maps fetched before start (default: every map in the
//                 index). Emscripten's lazy files need synchronous XHR on the
//                 thread that owns the JS file system, which is the page's
//                 main thread in a threaded build, and browsers forbid that
//                 for binary data; see docs/wasm-spike.md for the real fix
//                 (WasmFS's fetch backend, or OPFS from a worker).
(() => {
	const params = new URLSearchParams(location.search);
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
			const preload = params.get("preload");
			fetch(mapsUrl + "index.json").then((response) => response.json()).then(async (maps) => {
				const wanted = preload ? maps.filter((map) => preload.split(",").some((name) =>
					map.name.toLowerCase() === name.toLowerCase() + ".map")) : maps;
				let bytes = 0;
				await Promise.all(wanted.map(async (map) => {
					const data = new Uint8Array(await (await fetch(mapsUrl + map.name)).arrayBuffer());
					FS.writeFile("/data/maps/" + map.name, data);
					bytes += data.length;
					Module.setStatus(`maps: ${(bytes / 1048576).toFixed(0)} MB`);
				}));
				print(`[web] ${wanted.length} of ${maps.length} maps (${(bytes / 1048576).toFixed(0)} MB) from ${mapsUrl} in /data/maps`);
				if (params.get("init")) FS.writeFile("/data/init.txt", params.get("init"));
				FS.chdir("/data");
				Module.removeRunDependency("maps-index");
			}).catch((error) => {
				print(`[web] cannot list the maps at ${mapsUrl}: ${error}`);
			});
		}],
		onAbort: (what) => print(`[web] abort: ${what}`),
	};
})();
