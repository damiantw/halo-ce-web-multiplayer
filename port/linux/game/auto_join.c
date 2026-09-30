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

This replaces the web client's use of the netcode tests' join mode
(network_test.c, debug.network_test = "join"), which does the same and
also forces team 2 and logs every player's position every second.

Called from the main loop every frame (main.c), outside the dedicated
server.
*/

#include "cseries.h"
#include "main/main.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"

#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);

enum
{
	_auto_join_off,
	_auto_join_first,
};

static struct
{
	boolean checked;
	short mode;
	real menu_seconds;
	boolean searching;
	boolean joined;
	real joined_seconds;
	boolean player_added;
} auto_join;

static void auto_join_read_settings(
	void)
{
	char const *setting = config_string("web.join");

	auto_join.checked = TRUE;
	if (!strcmp(setting, "first"))
		auto_join.mode = _auto_join_first;
	else if (*setting)
		platform_log("auto join: unknown web.join \"%s\" (\"first\" or empty)", setting);
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

void auto_join_update(
	boolean main_menu_loaded,
	real seconds)
{
	if (!auto_join_enabled() || auto_join.player_added || !main_menu_loaded)
		return;
	auto_join.menu_seconds += seconds;
	/* (the main menu settling first) */
	if (auto_join.menu_seconds < 2.0f)
		return;

	if (!auto_join.searching)
	{
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
	}
	else if (!auto_join.joined)
	{
		if (network_game_client_join_first_available_game())
		{
			auto_join.joined = TRUE;
			ui_widgets_close_all();
			ui_widget_load_by_name_or_tag(
				"ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen",
				NONE, NULL, NONE, NONE, NONE, NONE);
			platform_log("auto join: joining");
		}
	}
	else
	{
		/* the player, once the lobby has settled (as pressing A in it) */
		auto_join.joined_seconds += seconds;
		if (auto_join.joined_seconds >= 3.0f && global_network_game_client_get())
		{
			auto_join.player_added = network_game_client_add_player(global_network_game_client_get(), 0);
			if (auto_join.player_added)
				platform_log("auto join: player added");
		}
	}
}
