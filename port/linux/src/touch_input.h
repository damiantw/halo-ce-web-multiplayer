/*
TOUCH_INPUT.H

The on-screen touch controls of the web build (touch_input.c), ported from
upstream OpenCE's touch_input.h (4a9e515d "Web touch controls"): only its
on-screen controls half. The Android menus' finger pointer (touch_menu.c)
is not ported: this fork's web build has no menus of its own, and its pause
menu and postgame take the controller's buttons (A, B, the D-pad), which
the overlay shows while a menu is up.
Everything here runs on the game's main thread (XInputGetState and
player_control.c).
*/

#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#ifdef HALO_WEB

#include "platform.h"

/* tells the overlay (port/web/shell/halo-touch.js) whether a menu is up and
HALO_WEB_TOUCH_CONTROLS, and adds its buttons and triggers to port 0's state */
void touch_input_controls(XINPUT_GAMEPAD *pad, int menus);

/* the touch controls' stick as the player's movement, -1..1 (forward, and
strafe to the left); nonzero while it is pushed */
int touch_input_move(float *forward, float *strafe);

/* adds the touch controls' view swipe since the last call to yaw and pitch,
in radians at `scale` radians per grid unit (and a gyroscope's turn to
gyro_yaw and gyro_pitch: none yet) */
void touch_input_look(float scale, float *yaw, float *pitch, float *gyro_yaw, float *gyro_pitch);

/* whether the swipe gets the stick's aim assist (HALO_WEB_TOUCH_AIM_ASSIST,
on unless 0) */
int touch_input_aim_assist(void);

/* port 0's motors, for the phone's vibration while the touch controls show */
void touch_input_rumble(unsigned int left, unsigned int right);

/* a swipe turned the view this frame; assisted: the magnetism acted on it */
void touch_input_looked(int assisted);

#endif

#endif
