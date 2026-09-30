/*
AUTO_JOIN.C

Joining a game without the menus (web.join in config.toml, HALO_WEB_JOIN):
the site's /play page picks a server, and its join token lets the game see
only that server through the gateway, so "first" (join the first game
found) joins the server the player picked. The steps are what picking a
game in Multiplayer > System Link and pressing A in its lobby do: the
network client searches, joins the game, opens the pregame lobby screen and
adds the player of controller 1. The server gives the player a team (a team
game balances them); nothing else is scripted.

A game that ends while the machine joins it (the host lets it go:
network_server_manager.c's network_game_server_close_ended_game), or that
refuses it, puts the network client back to searching, or (when the game
ended as the machine loaded it) makes the client leave it: the join starts
again, and takes the next lobby when the game is open again. That lasts
until the machine is in a game; a client that is gone otherwise (the host
went down, the player was kicked) is left at the main menu.

This replaces the web client's use of the netcode tests' join mode
(network_test.c, debug.network_test = "join"), which does the same and
also forces team 2 and logs every player's position every second.

Called from the main loop every frame (main.c), outside the dedicated
server.

The web build is only about the multiplayer (HALO_WEB with
HALO_MULTIPLAYER_ONLY, web_multiplayer_only): there is no main menu (the
main menu scenario loads without its menus, ui_widget.c
main_screen_shell_load), the game joins by itself even without web.join,
and leaving the game (the pause menu's Quit, B in the lobby, a lost
connection, a refused join: anything that would go back to the main menu),
or finding no game to join, leaves the page (web_leave: web_library.js,
web_leave_game, a "halo:leave" event the site's page answers by going back
to its home page). HALO_WEB_MENUS=1 brings the menus back, for development.
*/

#include "cseries.h"
#include "main/main.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"

#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);

enum
{
	_auto_join_off,
	_auto_join_first,
};

/* why the web build leaves (web_library.js, web_leave_game) */
enum
{
	_web_leave_menu,
	_web_leave_no_game,
};

static struct
{
	boolean checked;
	short mode;
	real menu_seconds;
	boolean searching;
	real searching_seconds;
	boolean joined;
	real joined_seconds;
	boolean player_added;
	/* in a game, or the game lost: done */
	boolean finished;
	short attempts;
} auto_join;

/* how long the web build looks for the game before giving up and leaving */
#define WEB_SEARCH_SECONDS 45.0f

/* whether the web build is the multiplayer-only one without menus */
boolean web_multiplayer_only(
	void)
{
#if defined(HALO_WEB) && defined(HALO_MULTIPLAYER_ONLY)
	char const *menus = getenv("HALO_WEB_MENUS");

	return !menus || !*menus || !strcmp(menus, "0") || !strcmp(menus, "false");
#else
	return FALSE;
#endif
}

#if defined(HALO_WEB) && defined(HALO_MULTIPLAYER_ONLY)
/* web_library.js */
extern void web_leave_game(int reason, int error_code);
#endif

/* the web build leaves the page, once (reasons in web_library.js,
web_leave_game); other builds carry on */
void web_leave(
	short reason,
	short error_code)
{
#if defined(HALO_WEB) && defined(HALO_MULTIPLAYER_ONLY)
	static boolean left;

	if (!web_multiplayer_only() || left)
		return;
	left = TRUE;
	platform_log("web: leaving the game (reason %d, error %d)", reason, error_code);
	web_leave_game(reason, error_code);
#else
	(void)reason;
	(void)error_code;
#endif
}

/* the web build asked to go back to its main menu (main_screen_shell_load:
Quit, B in the lobby, a lost connection, a refused join): it leaves the page
unless the join starts over meanwhile (a game that ended or refused this
machine as it joined: the next one takes it, auto_join_restart) */
static struct
{
	boolean pending;
	short error_code;
	real seconds;
} web_menu_request;

#define WEB_MENU_LEAVE_SECONDS 1.5f

void web_menu_requested(
	short error_code)
{
	if (!web_multiplayer_only())
		return;
	web_menu_request.pending = TRUE;
	web_menu_request.error_code = error_code;
	web_menu_request.seconds = 0.0f;
}

static void auto_join_read_settings(
	void)
{
	char const *setting = config_string("web.join");

	auto_join.checked = TRUE;
	if (!strcmp(setting, "first"))
		auto_join.mode = _auto_join_first;
	else if (*setting)
		platform_log("auto join: unknown web.join \"%s\" (\"first\" or empty)", setting);
	/* (the web build has nothing else to do) */
	if (web_multiplayer_only())
		auto_join.mode = _auto_join_first;
}

/* whether web.join asks for a join (network_test.c's join mode then leaves
the joining to it) */
boolean auto_join_enabled(
	void)
{
	if (!auto_join.checked)
		auto_join_read_settings();
	return auto_join.mode != _auto_join_off;
}

/* back to the start: a new client searches, and joins the game when it is
open again */
static void auto_join_restart(
	void)
{
	auto_join.searching = FALSE;
	auto_join.menu_seconds = 0.0f;
	auto_join.joined = FALSE;
	auto_join.player_added = FALSE;
	auto_join.finished = FALSE;
	auto_join.searching_seconds = 0.0f;
	/* (the join starts over: the page stays) */
	web_menu_request.pending = FALSE;
}

void auto_join_update(
	boolean main_menu_loaded,
	real seconds)
{
	short progress;

	if (!auto_join_enabled())
		return;
	/* (a game that ended as this machine loaded it let the machine go, and
	the client has left it: network_client_message_handler.c; this can come
	just after the machine is in the game) */
	if (auto_join.searching && !global_network_game_client_get() && network_game_client_take_let_go())
	{
		platform_log("auto join: the game ended as this machine joined it; joining again when it is open");
		auto_join_restart();
	}
	if (web_menu_request.pending)
	{
		web_menu_request.seconds += seconds;
		if (web_menu_request.seconds >= WEB_MENU_LEAVE_SECONDS)
		{
			web_menu_request.pending = FALSE;
			web_leave(0, web_menu_request.error_code);
		}
	}
	if (auto_join.finished)
		return;
	if (!auto_join.searching)
	{
		if (!main_menu_loaded)
			return;
		auto_join.menu_seconds += seconds;
		/* (the main menu settling first; without menus, only the scenario) */
		if (auto_join.menu_seconds < (web_multiplayer_only() ? 0.5f : 2.0f))
			return;
		auto_join.searching = TRUE;
		dispose_global_network_game_client();
		dispose_global_network_game_server();
		if (create_global_network_game_client())
		{
			game_connection_set(_game_connection_network_client);
			platform_log("auto join: searching for games");
		}
		else
		{
			/* tried again next frame */
			auto_join.searching = FALSE;
		}
		return;
	}

	if (!global_network_game_client_get())
	{
		/* (gone before it was in a game: the host went down, or kicked the
		player; not joined again) */
		if (auto_join.joined)
		{
			platform_log("auto join: the game was lost before it started for this machine");
			auto_join.finished = TRUE;
		}
		return;
	}

	progress = network_game_client_join_progress();
	if (!auto_join.joined)
	{
		auto_join.searching_seconds += seconds;
		if (auto_join.searching_seconds >= WEB_SEARCH_SECONDS && web_multiplayer_only())
		{
			platform_log("auto join: no game found in %.0f seconds", WEB_SEARCH_SECONDS);
			web_leave(_web_leave_no_game, NONE);
		}
		else if (network_game_client_join_first_available_game())
		{
			auto_join.joined = TRUE;
			auto_join.joined_seconds = 0.0f;
			auto_join.player_added = FALSE;
			auto_join.attempts++;
			ui_widgets_close_all();
			ui_widget_load_by_name_or_tag(
				"ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen",
				NONE, NULL, NONE, NONE, NONE, NONE);
			if (auto_join.attempts > 1)
				platform_log("auto join: joining (attempt %d)", auto_join.attempts);
			else
				platform_log("auto join: joining");
		}
		return;
	}

	auto_join.joined_seconds += seconds;
	if (progress >= 3)
	{
		auto_join.finished = TRUE;
		platform_log("auto join: in the game");
	}
	else if (progress == 0 && auto_join.joined_seconds >= 1.0f)
	{
		/* back to searching: the game refused the machine, or ended while it
		joined (the next lobby takes it) */
		platform_log("auto join: the game did not take this machine (it ended or refused it); joining again when it is open");
		/* (a new client: the one refused keeps its join in progress,
		network_game_client_reset) */
		auto_join_restart();
	}
	else if (progress == 2 && !auto_join.player_added && auto_join.joined_seconds >= 3.0f)
	{
		/* the player, once the lobby has settled (as pressing A in it) */
		auto_join.player_added = network_game_client_add_player(global_network_game_client_get(), 0);
		if (auto_join.player_added)
			platform_log("auto join: player added");
	}
}
