/*
TOUCH_GAME.C

What the web build's touch controls (port/linux/src/touch_input.c) need to
know of the game, from upstream OpenCE's touch_game.c (4a9e515d "Web touch
controls"). The platform layer cannot see the game's types, so it asks
here (this file is compiled as the game's own sources are). Nothing but in
the web build.
*/

#include "cseries.h"

#ifdef HALO_WEB

#include "game/game.h"
#include "cutscene/cinematics.h"
#include "input/input.h"
#include "input/input_abstraction.h"
#include "interface/ui_widget.h"

/* asks whether a cinematic is playing: the player has no controls, so the
on-screen touch controls hide (touch_input.c); tests cinematic_globals
first because the controller is read before the game exists during
input_initialize and shell_initialize, and game_initialize makes it after
the game time globals that game_in_progress reads (game.c) */
int touch_game_cinematic_playing(void)
{
	return cinematic_globals && game_in_progress() && cinematic_in_progress();
}

/* asks whether a game is being played: not while the game starts, nor
between maps, nor at the main menu (whose ui.map is a game too), where the
on-screen touch controls do not show (touch_input.c) */
int touch_game_playing(void)
{
	return cinematic_globals && game_in_progress() && !main_menu_is_active();
}

/* asks whether a menu is up for the first player (the pause menu, an
error, the postgame's): the overlay then shows the buttons that work
menus (touch_input.c). The web build's menus take no pointer (d3d8_gl.c,
halo_ui_pointer_update), so this is not sdl_platform.c's ui_pointer. */
int touch_game_menu_up(void)
{
	return cinematic_globals && ui_widgets_active_for_local_player(0);
}

/* (touch_input.c's TOUCH_BUTTONS and the overlay count 16 buttons) */
typedef char verify_touch_buttons[NUMBER_OF_GAMEPAD_BUTTONS == 16 ? 1 : -1];

/* the game control on each of player 1's 16 controller buttons, as the
gamepad's buttons are numbered (input.h), -1 for none: the touch controls
name their buttons by them, so a profile's own mapping shows */
void touch_game_button_controls(int *controls)
{
	short buttons[NUMBER_OF_GAMEPAD_BUTTONS];
	short index;

	input_abstraction_port_button_controls(0, buttons);
	for (index = 0; index < NUMBER_OF_GAMEPAD_BUTTONS; index++)
	{
		controls[index] = buttons[index];
	}
}

#else
/* (an empty unit otherwise) */
typedef int touch_game_unused;
#endif
