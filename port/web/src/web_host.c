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
#include <stdlib.h>
#include <strings.h>
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
int posix_web_net_owner_thread(void);

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
		/* on the gateway's thread, yield to its event loop (JSPI) while
		waiting, so the host's messages keep being taken during the
		download rather than pile up unread */
		if (posix_web_net_owner_thread())
		{
			emscripten_sleep(20);
		}
		else
		{
			struct timespec pause = { 0, 20 * 1000 * 1000 };

			nanosleep(&pause, NULL);
		}
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

/* ---------- the player's name

HALO_WEB_PLAYER_NAME (the page passes it with the other settings: the name
the player chose on the site) names the player in network games instead of
the profile's name or a random one (player_ui.c,
player_ui_get_active_player_profile). UTF-8, cut to capacity - 1
characters; characters outside the Basic Multilingual Plane become '?'.
The game's wchar_t is 16 bits, so the name is given as such. */
int web_player_name(unsigned short *name, int capacity)
{
	const unsigned char *text = (const unsigned char *)getenv("HALO_WEB_PLAYER_NAME");
	int count = 0;

	if (!text || !*text || capacity < 2)
		return 0;
	while (*text && count < capacity - 1)
	{
		unsigned long code = *text++;
		int extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0;

		if (code >= 0x80 && code < 0xc0)
			continue; /* (a stray continuation byte) */
		code &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
		for (; extra && (*text & 0xc0) == 0x80; extra--)
			code = (code << 6) | (*text++ & 0x3f);
		if (extra)
			continue; /* (cut short) */
		if (code < 0x20 || code == 0x7f)
			continue;
		name[count++] = (unsigned short)(code > 0xffff || (code >= 0xd800 && code < 0xe000) ? '?' : code);
	}
	/* (no leading or trailing spaces: a name of spaces is no name) */
	while (count && name[count - 1] == ' ')
		count--;
	{
		int start = 0, index;

		while (start < count && name[start] == ' ')
			start++;
		for (index = start; index < count; index++)
			name[index - start] = name[index];
		count -= start;
	}
	name[count] = 0;
	return count;
}

/* ---------- the player's colour

HALO_WEB_PLAYER_COLOR (the page passes it with the name: the colour the
player chose on the site) is the player's colour in network games instead of
the profile's (none: the host picks one at random), as the profile's colour
picker would set it (player_ui.c, player_ui_get_active_player_profile). The
index into player_profile.c's profile_color_table (0-17) or its name, in the
order of the game's colour picker. Team games colour the players by team
(game_engine.c, game_engine_player_get_change_color). -1: none, or not a
colour. */
int web_player_color(void)
{
	static char const *const names[] =
	{
		"white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple",
		"cyan", "cobalt", "orange", "teal", "sage", "brown", "tan", "maroon", "salmon",
	};
	const char *text = getenv("HALO_WEB_PLAYER_COLOR");
	int index;

	if (!text || !*text)
		return -1;
	if (text[0] >= '0' && text[0] <= '9')
	{
		char *end;
		long value = strtol(text, &end, 10);

		return !*end && value >= 0 && value < (long)(sizeof(names) / sizeof(names[0])) ? (int)value : -1;
	}
	for (index = 0; index < (int)(sizeof(names) / sizeof(names[0])); index++)
	{
		if (!strcasecmp(text, names[index]) || (index == 4 && !strcasecmp(text, "grey")))
			return index;
	}
	return -1;
}

/* the team the player asked for on the site (HALO_WEB_PLAYER_TEAM: red or 0,
blue or 1; anything else, or none, is "auto"): 0 or 1, else -1. The join
request carries it (network_client_manager.c, network_game_client_add_player)
and a port's server honours it while the teams stay within one player of
each other (network_server_manager.c, network_game_server_pick_team). */
int web_player_team(void)
{
	const char *text = getenv("HALO_WEB_PLAYER_TEAM");

	if (!text || !*text)
		return -1;
	if (!strcasecmp(text, "red") || !strcmp(text, "0"))
		return 0;
	if (!strcasecmp(text, "blue") || !strcmp(text, "1"))
		return 1;
	return -1;
}
