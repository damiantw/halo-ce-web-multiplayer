/*
WEB_HOST.C

What the Android guest's host provides to the shared platform layer
(port/android/guest/runtime/guest_host.h), for the browser: the few
OpenGL ES helpers d3d8_gl.c calls under HALO_ANDROID, in their WebGL 2
form, and stubs for the Android-only and internet-play-only features the
web build leaves out (tools/web_build.py PLATFORM_EXCLUDE).
*/

#include "platform.h"
#include "gl.h"

#include <string.h>
#include <SDL3/SDL.h>
#include <emscripten/html5.h>

/* WebGL names some extensions differently from OpenGL ES (and Emscripten
reports them with a GL_ prefix added) */
static const char *web_extension_name(const char *name)
{
	static const char *const aliases[][2] =
	{
		{ "GL_EXT_texture_compression_s3tc", "WEBGL_compressed_texture_s3tc" },
		{ "GL_EXT_texture_filter_anisotropic", "EXT_texture_filter_anisotropic" },
	};
	unsigned int index;

	for (index = 0; index < sizeof(aliases) / sizeof(aliases[0]); index++)
		if (!strcmp(name, aliases[index][0]))
			return aliases[index][1];
	return name;
}

int host_gl_has_extension(const char *name)
{
	const char *wanted = web_extension_name(name);
	GLint count = 0, index;

	if (!strncmp(wanted, "GL_", 3))
		wanted += 3;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strncmp(extension, "GL_", 3))
			extension += 3;
		if (extension && !strcmp(extension, wanted))
			return 1;
	}
	return 0;
}

/* the visibility test counters: atomic counters are OpenGL ES 3.1, which
WebGL 2 lacks, so xgpu_capabilities.atomic_counters is never set and this
is not reached */
unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset)
{
	(void)buffer;
	(void)offset;
	return 0;
}

/* WebGL has no buffer mapping; bufferSubData is the only way in. The
browser orders it after the draws already queued, so the ring's fences
(below) are not needed for correctness. */
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	(void)slot;
}

bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int xoffset, int yoffset)
{
	(void)duration;
	(void)gravity;
	(void)xoffset;
	(void)yoffset;
	platform_log("%s", message);
	return true;
}

/* internet play's Discord presence and UPnP: not in the browser */
void p2p_discord_update(void)
{
}

void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count)
{
	(void)secret;
	(void)player_count;
	(void)maximum_player_count;
}

/* scenario.c declares the MSVC compiler barrier as a function */
void _ReadWriteBarrier(void)
{
	__sync_synchronize();
}

/* ---------- maps fetched when the game first wants them

The page (port/web/shell/halo-loader.js) fetches ui.map before the start and
any other map when the game asks for it: Module.haloFetchMap(name) resolves
once maps/<name>.map is in the file system (true), or false if the maps
index has no such map. A system link client starts fetching the host's map
as the pregame lobby names it (network_game_globals.c), and waits for it
when the map loads (cache_files_windows.c). */

#include <emscripten.h>
#include <stdlib.h>
#include <time.h>

BOOL platform_data_has_map(const char *name);

static void web_request_map(const char *name, volatile int *state)
{
	MAIN_THREAD_ASYNC_EM_ASM({
		var name = UTF8ToString($0);
		var state = $1;
		_free($0);
		var done = function (value) {
			if (state)
				Atomics.store(new Int32Array(wasmMemory.buffer), state >> 2, value);
		};
		if (!Module.haloFetchMap)
			return done(-1);
		Module.haloFetchMap(name).then(function (ok) { done(ok ? 1 : -1); }, function () { done(-1); });
	}, strdup(name), state);
}

/* starts fetching maps/<name>.map if the file system lacks it */
void web_prefetch_map(const char *name)
{
	if (name && *name && !platform_data_has_map(name))
		web_request_map(name, NULL);
}

/* whether maps/<name>.map is in the file system; if not, fetches it: with
wait, waits for it (FALSE if the page has no such map), else only starts
the download (FALSE) */
BOOL web_fetch_map(const char *name, BOOL wait)
{
	static volatile int state;
	unsigned waited_ms = 0;

	if (!name || !*name || platform_data_has_map(name))
		return TRUE;
	if (!wait)
	{
		web_request_map(name, NULL);
		return FALSE;
	}
	platform_log("web: waiting for maps/%s.map", name);
	state = 0;
	web_request_map(name, &state);
	while (!state)
	{
		struct timespec pause = { 0, 20 * 1000 * 1000 };

		nanosleep(&pause, NULL);
		waited_ms += 20;
	}
	platform_log("web: maps/%s.map %s after %u ms", name, state > 0 ? "fetched" : "not found", waited_ms);
	return state > 0 ? TRUE : FALSE;
}

/* ---------- a stall watchdog (HALO_WEB_WATCHDOG=1: logs when frames stop) */

#include <pthread.h>
#include <unistd.h>

volatile unsigned web_trace_frames;

static void *web_watchdog_thread(void *unused)
{
	unsigned last = 0, still = 0;

	(void)unused;
	for (;;)
	{
		sleep(1);
		if (web_trace_frames == last)
		{
			if (still++ < 5)
				platform_log("web: watchdog: no frame for %us (pause the game worker in DevTools to see where)", still);
		}
		else
		{
			still = 0;
			last = web_trace_frames;
		}
	}
	return NULL;
}

void web_watchdog_start(void)
{
	static int started;
	pthread_t thread;

	if (started++ || !getenv("HALO_WEB_WATCHDOG"))
		return;
	pthread_create(&thread, NULL, web_watchdog_thread, NULL);
	pthread_detach(thread);
}
