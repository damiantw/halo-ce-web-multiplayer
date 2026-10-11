/*
WEB_SHARED.H

The words of the game's memory the page reads and writes itself
(port/web/shell/halo-touch.js): the WebAssembly memory is shared, so the
page's thread sees them as the game's threads do, without a message. The
page finds them with web_state (web_touch.c) and knows them by these
indices, 32-bit words each: change them together.

Ported from upstream OpenCE (4a9e515d "Web touch controls"), where the
page's app.js also reads the frames shown; this fork's page does not, so
word 0 is kept only so that the indices stay upstream's.
*/

#ifndef WEB_SHARED_H
#define WEB_SHARED_H

#include <stdint.h>

enum
{
	/* (upstream's frames shown; unused here) */
	_web_frames_shown = 0,
	/* the game's, for the touch controls: touch_input.c's _touch_scene_*
	bits, port 0's rumble (0, or 64 to 255), and the game control on each of
	the 16 controller buttons with a count of their changes */
	_web_touch_scene = 1,
	_web_touch_rumble = 2,
	_web_touch_bindings_serial = 3,
	_web_touch_bindings = 4,
	/* the page's: the controller state the touch controls make (the SDL
	axes left x and y, right x and y, the left and right triggers, then
	the SDL button bits), a count of presses of each input (the 15 buttons,
	then the triggers), and the view's swipe (and two words for a
	gyroscope's turn, unused yet) since the game last read them, in 1/256 of
	a grid unit (halo-touch.js) */
	_web_touch_state = 20,
	_web_touch_presses = 27,
	_web_touch_look = 44,
	/* the game's, for the page's tests: counts of the frames in which a
	swipe turned the view, and in which the aim assist's magnetism then
	acted on it (player_control.c) */
	_web_touch_looked = 48,
	_web_touch_assisted = 49,
	WEB_SHARED_WORDS = 52,
};

#define WEB_TOUCH_INPUTS 17

/* the words (web_touch.c) */
extern volatile int32_t web_shared[WEB_SHARED_WORDS];

#endif
