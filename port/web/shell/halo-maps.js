// The maps kept in the browser (Cache Storage), for the game's loader
// (halo-loader.js) and for a site's page that starts the downloads before
// the game's frame exists (the site's /play: when the player picks a
// server, the server's map and ui.map start downloading while the page
// joins it and the game loads; the game then takes them from here).
//
//   haloMaps.get(mapsUrl, entry)          -> Promise<Uint8Array>
//   haloMaps.prefetch(mapsUrl, names)     -> Promise (each map stored)
//   haloMaps.forget(mapsUrl, index)       -> Promise (old versions deleted)
//
// entry is one of maps/index.json's: {name, size, sha256?}. A map is kept
// under its URL with its version, ?v=<sha256> when the index gives one (that
// URL is cacheable for ever: the site answers it with an immutable
// Cache-Control), else ?size=<size>: a map that changes gets a new key, and
// forget() deletes the keys of versions the index no longer has. What comes
// out of the storage is checked against the index's size; a missing or bad
// entry (the browser may evict the storage when the disk fills up) is
// downloaded again. Without Cache Storage (an insecure origin), or when
// storing fails (quota), maps are downloaded as before, through the HTTP
// cache.
//
// One download per map at a time, across the page and its game frame: a
// Web Lock per map key, so whoever asks second waits for the first download
// and then reads the stored map instead of downloading it again.
(() => {
	if (window.haloMaps) return;
	const CACHE = "halo-maps-v1";
	const storage = () => (typeof caches !== "undefined" && window.isSecureContext ? caches.open(CACHE) : Promise.resolve(null));
	const absolute = (url) => new URL(url, location.href).href;
	const versioned = (mapsUrl, entry) => entry.sha256 && /^[0-9a-f]{16,128}$/i.test(entry.sha256);
	const fileUrl = (mapsUrl, entry) => absolute(mapsUrl + encodeURIComponent(entry.name)
		+ (versioned(mapsUrl, entry) ? `?v=${entry.sha256}` : ""));
	const keyUrl = (mapsUrl, entry) => versioned(mapsUrl, entry) ? fileUrl(mapsUrl, entry)
		: absolute(`${mapsUrl}${encodeURIComponent(entry.name)}?size=${entry.size}`);
	const withLock = (name, work) => (navigator.locks && navigator.locks.request
		? navigator.locks.request(name, work) : work());
	const pending = {};
	const stats = { storage: 0, downloaded: 0 };

	// the stored map, if it is whole (else deleted)
	const stored = async (cache, key, entry) => {
		const response = await cache.match(key);
		if (!response) return null;
		const data = new Uint8Array(await response.arrayBuffer());
		if (!entry.size || data.length === entry.size) return data;
		await cache.delete(key);
		return null;
	};

	const download = async (mapsUrl, entry, cache) => {
		const url = fileUrl(mapsUrl, entry);
		if (cache) {
			try {
				// (straight into the storage: not a second copy in the HTTP cache)
				const response = await fetch(url, { cache: "no-store" });
				if (!response.ok) throw new Error(`${entry.name}: HTTP ${response.status}`);
				await cache.put(keyUrl(mapsUrl, entry), response);
				const data = await stored(cache, keyUrl(mapsUrl, entry), entry);
				if (data) return data;
			} catch (error) {
				if (/HTTP \d/.test(String(error))) throw error;
				console.warn(`[web] maps storage: ${entry.name} not stored (${error}); downloading it without`);
			}
		}
		const response = await fetch(url);
		if (!response.ok) throw new Error(`${entry.name}: HTTP ${response.status}`);
		const data = new Uint8Array(await response.arrayBuffer());
		if (entry.size && data.length !== entry.size)
			throw new Error(`${entry.name}: ${data.length} bytes, the index says ${entry.size}`);
		return data;
	};

	// the map's bytes: stored, or downloaded (and stored); with keep false,
	// only stored (prefetch: nothing held in memory)
	const load = (mapsUrl, entry, keep) => {
		const key = keyUrl(mapsUrl, entry);
		const id = key + (keep ? "" : "#prefetch");
		if (pending[id]) return pending[id];
		const promise = withLock("halo-map:" + key, async () => {
			let cache = null;
			try {
				cache = await storage();
			} catch (error) {
				cache = null;
			}
			if (cache) {
				try {
					if (!keep && await cache.match(key)) return null;
					const data = keep ? await stored(cache, key, entry) : null;
					if (data) {
						stats.storage++;
						return { data, from: "storage" };
					}
				} catch (error) {
					console.warn(`[web] maps storage: cannot read ${entry.name} (${error})`);
				}
			}
			const data = await download(mapsUrl, entry, cache);
			stats.downloaded++;
			return keep ? { data, from: "network" } : null;
		}).finally(() => { delete pending[id]; });
		pending[id] = promise;
		return promise;
	};

	const indexes = {};
	const index = (mapsUrl) => indexes[mapsUrl] || (indexes[mapsUrl] = fetch(mapsUrl + "index.json")
		.then((response) => (response.ok ? response.json() : Promise.reject(new Error(`index.json: HTTP ${response.status}`))))
		.catch((error) => { delete indexes[mapsUrl]; throw error; }));

	window.haloMaps = {
		stats,
		// {data: Uint8Array, from: "storage" | "network"}
		get: (mapsUrl, entry) => load(mapsUrl, entry, true),
		// names: "ui", "bloodgulch.map", ...; maps the index does not list are skipped
		prefetch: async (mapsUrl, names) => {
			const list = await index(mapsUrl);
			const wanted = names.map((name) => String(name).toLowerCase().replace(/\.map$/, "") + ".map");
			await Promise.all(list.filter((entry) => wanted.includes(entry.name.toLowerCase()))
				.map((entry) => load(mapsUrl, entry, false).catch((error) => console.warn(`[web] prefetch ${entry.name}: ${error}`))));
		},
		// deletes the stored maps under mapsUrl that the index does not name
		// in their current version (a map changed, or is gone)
		forget: async (mapsUrl, list) => {
			const cache = await storage();
			if (!cache) return 0;
			const current = new Set(list.map((entry) => keyUrl(mapsUrl, entry)));
			const prefix = absolute(mapsUrl);
			let deleted = 0;
			for (const request of await cache.keys()) {
				const url = new URL(request.url);
				if (!request.url.startsWith(prefix) || url.pathname.slice(new URL(prefix).pathname.length).includes("/")) continue;
				if (!current.has(request.url)) {
					await cache.delete(request);
					deleted++;
				}
			}
			return deleted;
		},
	};
})();
