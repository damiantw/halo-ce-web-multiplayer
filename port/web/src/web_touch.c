/*
WEB_TOUCH.C

The on-screen touch controls between the page's overlay
(port/web/shell/halo-touch.js) and the game, as upstream OpenCE's
port/web/src/web_touch.c (4a9e515d "Web touch controls") and its Android
host_touch.c have them: the services touch_input.c asks of the host. The
page writes its stick, buttons and swipes into the shared words
(web_shared.h); the game reads them when it reads port 0's controller
(xinput_sdl.c), and writes back whether a game is being played (the scene),
the profile's button mapping and port 0's rumble.
*/

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <emscripten/emscripten.h>

#include "web_shared.h"

/* a press shorter than this still reaches the game, which reads the
controller once a frame (host_touch.c's TOUCH_TAP_NS) */
#define TOUCH_TAP_NS 60000000LL

volatile int32_t web_shared[WEB_SHARED_WORDS];

static int32_t presses_seen[WEB_TOUCH_INPUTS];
static int64_t pressed_ns[WEB_TOUCH_INPUTS];

/* where the page finds the words (Module._web_state(), a byte address) */
EMSCRIPTEN_KEEPALIVE volatile int32_t *web_state(void)
{
	return web_shared;
}

static int64_t now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* the overlay's controller state, with each press held for at least
TOUCH_TAP_NS (a tap can go down and up between two reads) */
void host_touch_read(int32_t *state)
{
	int64_t now = now_ns();
	int input;

	for (input = 0; input < 7; input++)
		state[input] = __atomic_load_n(&web_shared[_web_touch_state + input], __ATOMIC_RELAXED);
	for (input = 0; input < WEB_TOUCH_INPUTS; input++)
	{
		int32_t presses = __atomic_load_n(&web_shared[_web_touch_presses + input], __ATOMIC_RELAXED);

		if (presses != presses_seen[input])
		{
			presses_seen[input] = presses;
			pressed_ns[input] = now;
		}
		if (pressed_ns[input] && now - pressed_ns[input] < TOUCH_TAP_NS)
		{
			if (input < 15)
				state[6] |= 1 << input;
			else if (!state[4 + input - 15])
				state[4 + input - 15] = 32767;
		}
	}
}

/* the swipe (then a gyroscope's turn, which nothing writes yet) since the
last read, in the overlay's grid units */
void host_touch_look_read(float *delta)
{
	int index;

	for (index = 0; index < 4; index++)
		delta[index] = (float)__atomic_exchange_n(&web_shared[_web_touch_look + index], 0, __ATOMIC_RELAXED) / 256.0f;
}

/* port 0's motors, each 0..65535, as the phone's vibration (the page's
navigator.vibrate) */
void host_touch_rumble(unsigned int low, unsigned int high)
{
	unsigned int strength = low > high ? low : high;

	__atomic_store_n(&web_shared[_web_touch_rumble], strength ? 64 + (int)(strength * 191u / 65535u) : 0,
		__ATOMIC_RELAXED);
}

void host_touch_scene(int scene)
{
	__atomic_store_n(&web_shared[_web_touch_scene], scene, __ATOMIC_RELAXED);
}

/* the game control on each controller button (port/linux/game/touch_game.c),
which name the overlay's buttons */
void host_touch_bindings(const int32_t *controls)
{
	int index;

	for (index = 0; index < 16; index++)
		__atomic_store_n(&web_shared[_web_touch_bindings + index], controls[index], __ATOMIC_RELAXED);
	__atomic_add_fetch(&web_shared[_web_touch_bindings_serial], 1, __ATOMIC_RELEASE);
}

/* a swipe turned the view this frame, and whether the aim assist's
magnetism acted on it (player_control.c, through xinput_sdl.c) */
void host_touch_looked(int assisted)
{
	__atomic_add_fetch(&web_shared[assisted ? _web_touch_assisted : _web_touch_looked], 1, __ATOMIC_RELAXED);
}
