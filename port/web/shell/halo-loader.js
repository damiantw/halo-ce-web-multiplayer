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
//   preload=<a,b> the maps fetched before start (default: ui; "all" for
//                 every map in the index). Any other map is fetched when the
//                 game first wants it (Module.haloFetchMap, called from
//                 port/web/src/web_host.c: a system link client starts on the
//                 host's map in the pregame lobby and waits for it as the map
//                 loads). Emscripten's lazy files would need synchronous XHR
//                 on the page's main thread, which browsers forbid for binary
//                 data; see docs/wasm-spike.md.
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
				FS.chdir("/data");
				Module.removeRunDependency("maps-index");
			}).catch((error) => {
				print(`[web] cannot list the maps at ${mapsUrl}: ${error}`);
			});
		}],
		onAbort: (what) => print(`[web] abort: ${what}`),
	};
})();
