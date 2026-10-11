/*
TOUCH_INPUT.C

The on-screen touch controls of the web build, from upstream OpenCE's
touch_input.c (4a9e515d "Web touch controls"; the "on-screen touch
controls" half, which its Android and web builds share). The overlay
(port/web/shell/halo-touch.js) draws and reads its own fingers on the
page's thread and hands its state over in shared memory
(port/web/src/web_touch.c, web_shared.h); this reads it with port 0's
controller (xinput_sdl.c) and tells the overlay when to show.

Adapted: the settings are the site's HALO_WEB_* variables
(port/web/src/web_host.c) rather than port_config.c's input.touch_*; the
menus' finger pointer (upstream touch_menu.c) is left out: this fork's web
build has no menus of its own, and its pause menu and postgame take a
controller's buttons, which the overlay shows while one is up.
*/

#ifdef HALO_WEB

#include "touch_input.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/web/src/web_touch.c */
void host_touch_read(int *state);
void host_touch_look_read(float *delta);
void host_touch_rumble(unsigned int low, unsigned int high);
void host_touch_scene(int scene);
void host_touch_bindings(const int *controls);
void host_touch_looked(int assisted);

/* port/web/src/web_host.c */
int web_touch_controls(void);
int web_touch_aim_assist(void);

/* the game's (port/linux/game/touch_game.c) */
int touch_game_cinematic_playing(void);
int touch_game_playing(void);
int touch_game_menu_up(void);
void touch_game_button_controls(int *controls);

/* the gamepad's buttons (input.h), which the touch controls are named by */
#define TOUCH_BUTTONS 16

/* These bits tell the overlay when to show: */
enum
{
	/* the game has read its controller: the other bits are known */
	_touch_scene_known = 1 << 0,
	/* a menu or a cinematic is up, or no game is being played: the
	overlay shows only what works there (A, B, the D-pad, Menu, Chat) */
	_touch_scene_menus = 1 << 1,
	/* HALO_WEB_TOUCH_CONTROLS: "on" and "off" (none: "auto", shown on a
	touchscreen without a controller) */
	_touch_scene_on = 1 << 2,
	_touch_scene_off = 1 << 3,
};

/* the touch controls' stick at port 0's last read, -1..1, y down */
static float move_x, move_y;

void touch_input_controls(XINPUT_GAMEPAD *pad, int menus)
{
	/* SDL's gamepad buttons, in order (SDL_GamepadButton); the first four
	are the analog A, B, X and Y */
	static const WORD digital[] =
	{
		0, 0, 0, 0, XINPUT_GAMEPAD_BACK, 0, XINPUT_GAMEPAD_START,
		XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
		0, 0, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
		XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT
	};
	static const int analog[] =
	{
		XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y
	};
	static int bindings_sent[TOUCH_BUTTONS];
	static int bindings_known;
	int bindings[TOUCH_BUTTONS];
	int state[7];
	int setting = web_touch_controls();
	int index;

	/* (the controls show only in a game: not while it starts or loads a
	map, in a menu or during a cinematic) */
	host_touch_scene(_touch_scene_known |
		(setting > 0 ? _touch_scene_on : setting < 0 ? _touch_scene_off : 0) |
		(menus || touch_game_menu_up() || !touch_game_playing() || touch_game_cinematic_playing() ?
			_touch_scene_menus : 0));
	/* (the profile's mapping, for the buttons' names; only when it changes) */
	touch_game_button_controls(bindings);
	if (!bindings_known || memcmp(bindings, bindings_sent, sizeof(bindings)))
	{
		memcpy(bindings_sent, bindings, sizeof(bindings));
		bindings_known = TRUE;
		host_touch_bindings(bindings);
	}
	host_touch_read(state);
	/* the stick moves the player through the game's input state, whatever
	the profile's sticks do (touch_input_move), not as the left stick: no
	dead zone nor diagonal snapping on top of the overlay's own */
	move_x = state[0] / 32767.0f;
	move_y = state[1] / 32767.0f;
	for (index = 0; index < 4; index++)
	{
		if (state[6] & (1 << index))
			pad->bAnalogButtons[analog[index]] = 0xff;
	}
	for (index = 0; index < (int)(sizeof(digital) / sizeof(digital[0])); index++)
	{
		if (state[6] & (1 << index))
			pad->wButtons |= digital[index];
	}
	/* the shoulders are white and black, as xinput_sdl.c has a pad's */
	if (state[6] & (1 << 9))
		pad->bAnalogButtons[XINPUT_GAMEPAD_WHITE] = 0xff;
	if (state[6] & (1 << 10))
		pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = 0xff;
	if (state[4])
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 0xff;
	if (state[5])
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 0xff;
}

int touch_input_move(float *forward, float *strafe)
{
	float x = move_x;
	float y = move_y;
	float largest = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);

	if (largest == 0.0f)
	{
		*forward = 0.0f;
		*strafe = 0.0f;
		return FALSE;
	}
	/* the overlay's circle onto the square a controller's stick gives
	(input_abstraction_update): a full diagonal is full on both axes */
	{
		float scale = sqrtf(x * x + y * y) / largest;

		x *= scale;
		y *= scale;
		x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
		y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
	}
	/* (SDL's y runs down; strafe is positive to the left) */
	*forward = -y;
	*strafe = -x;
	return TRUE;
}

void touch_input_look(float scale, float *yaw, float *pitch, float *gyro_yaw, float *gyro_pitch)
{
	float delta[4];

	host_touch_look_read(delta);
	*yaw -= delta[0] * scale;
	*pitch -= delta[1] * scale;
	*gyro_yaw -= delta[2] * scale;
	*gyro_pitch -= delta[3] * scale;
}

int touch_input_aim_assist(void)
{
	return web_touch_aim_assist();
}

void touch_input_rumble(unsigned int left, unsigned int right)
{
	host_touch_rumble(left, right);
}

void touch_input_looked(int assisted)
{
	host_touch_looked(assisted);
}

#else
/* (an empty unit otherwise) */
typedef int touch_input_unused;
#endif
