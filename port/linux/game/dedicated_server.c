/*
DEDICATED_SERVER.C

The headless dedicated server (server.dedicated in config.toml,
HALO_DEDICATED=1; port/linux/README.md): the game runs without a window,
sound or a player of its own, and hosts system link games one after
another for other machines to join, as the pregame screen's fast setup
does (the automated host of network_test.c, without its player):

- once the main menu has loaded it hosts a game with the rotation's first
  map and game type (server.rotation);
- in the lobby the countdown (server.countdown seconds) starts whenever
  server.minimum_players players are in, and is never left paused;
- a game nobody is left in ends after server.empty_seconds;
- server.postgame_seconds after the scores show, the lobby opens again with
  the rotation's next map and game type (game_engine.c's postgame, which
  would wait for the host to press a button);
- a game lost to a network failure or an abort (the server gone, back at the
  main menu) is hosted again after server.rehost_seconds;
- SIGINT or SIGTERM tells the players and quits (sdl_platform.c's
  platform_quit_requested).

The rest of the headless mode: port_config.c forces the settings a server
has no use for (the null renderer, no sound, no internet play, updates or
Discord), sdl_platform.c starts only SDL's events, main.c draws nothing and
sleeps out each 30th of a second, and the machine is left out of the
players every machine must bring (network_server_manager.c), and of the
game ending when its last player leaves (network_client_manager.c).

Called from the main loop every frame instead of network_test_update
(main.c).
*/

#include "cseries.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
const char *config_string(char const *name);
long config_integer(char const *name);
void platform_log(char const *format, ...);
int halo_dedicated_server(void);
int platform_quit_requested(void);
int platform_data_has_map(char const *name);
int platform_data_map_type(char const *name);
/* network_server_manager.c's */
short network_game_server_dedicated_player_count(struct network_game_server *server);
boolean network_game_server_dedicated_in_pregame(struct network_game_server *server);
boolean network_game_server_dedicated_in_game(struct network_game_server *server);
void network_game_server_dedicated_lobby_update(struct network_game_server *server, long minimum_players,
	long maximum_players, long countdown_milliseconds);

enum
{
	MAXIMUM_ROTATION_ENTRIES = 64,
	DEDICATED_MAP_PATH_LENGTH = 128,
	DEDICATED_VARIANT_NAME_LENGTH = 32,
	/* server.max_players: system link's players on the Xbox */
	DEDICATED_MAXIMUM_PLAYERS = 16,
};

enum
{
	/* waiting at the main menu to host */
	_dedicated_waiting,
	/* hosting: the lobby is being set up */
	_dedicated_hosting,
	/* hosting a game: the lobby, the game or the scores */
	_dedicated_running,
};

struct dedicated_rotation_entry
{
	char map_path[DEDICATED_MAP_PATH_LENGTH];
	char variant_name[DEDICATED_VARIANT_NAME_LENGTH];
};

static struct
{
	boolean read_settings;
	struct dedicated_rotation_entry rotation[MAXIMUM_ROTATION_ENTRIES];
	short rotation_count;
	short rotation_index;
	/* the entry the server's lobby has (NONE none yet) */
	short applied_index;
	long minimum_players;
	long maximum_players;
	long countdown_milliseconds;
	real postgame_seconds;
	real empty_seconds;
	real rehost_seconds;

	short state;
	real wait_seconds;
	real wait_needed;
	real setup_seconds;
	real empty_elapsed;
	boolean ended_empty_game;
	real postgame_elapsed;
	boolean postgame_advanced;
	real postgame_retry;
	short last_player_count;
	short last_phase;
	unsigned long games_hosted;
} dedicated;

/* ---------- settings */

static boolean dedicated_variant_is_known(
	char const *name)
{
	struct game_variant variant;

	csmemset(&variant, 0, sizeof(variant));
	game_engine_get_variant_by_name(&variant, name);
	return variant.game_engine_index != 0;
}

/* "<map>[:<variant>]": a full scenario path (with backslashes) or a
multiplayer map's name, levels\test\<map>\<map> */
static void dedicated_add_rotation_entry(
	char const *text)
{
	char map[DEDICATED_MAP_PATH_LENGTH];
	char variant[DEDICATED_VARIANT_NAME_LENGTH] = "slayer";
	char const *colon = strchr(text, ':');
	char const *base;
	struct dedicated_rotation_entry *entry;
	size_t length = colon ? (size_t)(colon - text) : strlen(text);
	int map_type;

	if (!length)
		return;
	if (length >= sizeof(map))
	{
		platform_log("dedicated server: rotation entry \"%s\" is too long; left out", text);
		return;
	}
	memcpy(map, text, length);
	map[length] = 0;
	if (colon && colon[1])
		snprintf(variant, sizeof(variant), "%s", colon + 1);
	if (!dedicated_variant_is_known(variant))
	{
		platform_log("dedicated server: rotation entry \"%s\": no game type \"%s\"; left out", text, variant);
		return;
	}
	base = strrchr(map, '\\');
	base = base ? base + 1 : map;
	if (!platform_data_has_map(base))
	{
		platform_log("dedicated server: rotation entry \"%s\": no maps/%s.map; left out", text, base);
		return;
	}
	/* (a campaign level or the main menu cannot be hosted: the players'
	machines leave, and the lobby waits forever) */
	map_type = platform_data_map_type(base);
	if (map_type == 0 || map_type == 2)
	{
		platform_log("dedicated server: rotation entry \"%s\": maps/%s.map is %s, not a multiplayer map; left out",
			text, base, map_type == 0 ? "a campaign level" : "the main menu");
		return;
	}
	if (dedicated.rotation_count >= MAXIMUM_ROTATION_ENTRIES)
	{
		platform_log("dedicated server: more than %d rotation entries; \"%s\" left out", MAXIMUM_ROTATION_ENTRIES, text);
		return;
	}
	entry = &dedicated.rotation[dedicated.rotation_count++];
	if (strchr(map, '\\'))
		snprintf(entry->map_path, sizeof(entry->map_path), "%s", map);
	else
		snprintf(entry->map_path, sizeof(entry->map_path), "levels\\test\\%s\\%s", map, map);
	snprintf(entry->variant_name, sizeof(entry->variant_name), "%s", variant);
}

static real dedicated_seconds_setting(
	char const *name,
	long minimum,
	long maximum)
{
	long value = config_integer(name);

	return (real)PIN(value, minimum, maximum);
}

static void dedicated_read_settings(
	void)
{
	char rotation[1024];
	char *token;
	short index;

	if (dedicated.read_settings)
		return;
	dedicated.read_settings = TRUE;
	dedicated.applied_index = NONE;
	dedicated.last_player_count = NONE;
	dedicated.last_phase = NONE;

	snprintf(rotation, sizeof(rotation), "%s", config_string("server.rotation"));
	for (token = strtok(rotation, ",; \t\r\n"); token; token = strtok(NULL, ",; \t\r\n"))
		dedicated_add_rotation_entry(token);
	if (!dedicated.rotation_count)
	{
		platform_log("dedicated server: no playable entry in server.rotation (\"%s\"); hosting bloodgulch:slayer",
			config_string("server.rotation"));
		snprintf(dedicated.rotation[0].map_path, sizeof(dedicated.rotation[0].map_path),
			"levels\\test\\bloodgulch\\bloodgulch");
		snprintf(dedicated.rotation[0].variant_name, sizeof(dedicated.rotation[0].variant_name), "slayer");
		dedicated.rotation_count = 1;
	}

	dedicated.maximum_players = PIN(config_integer("server.max_players"), 1, DEDICATED_MAXIMUM_PLAYERS);
	dedicated.minimum_players = PIN(config_integer("server.minimum_players"), 1, 127);
	if (dedicated.minimum_players > dedicated.maximum_players)
	{
		platform_log("dedicated server: server.minimum_players %ld is more than server.max_players %ld; using %ld",
			dedicated.minimum_players, dedicated.maximum_players, dedicated.maximum_players);
		dedicated.minimum_players = dedicated.maximum_players;
	}
	dedicated.countdown_milliseconds = PIN(config_integer("server.countdown"), 0, 600) * 1000;
	/* (the 999 the original countdowns end on, so the last second shows) */
	if (dedicated.countdown_milliseconds)
		dedicated.countdown_milliseconds -= 1;
	dedicated.postgame_seconds = dedicated_seconds_setting("server.postgame_seconds", 0, 3600);
	dedicated.empty_seconds = dedicated_seconds_setting("server.empty_seconds", 0, 86400);
	dedicated.rehost_seconds = dedicated_seconds_setting("server.rehost_seconds", 1, 3600);

	platform_log("dedicated server: \"%s\", %ld to %ld players, %ld second countdown, %d game rotation:",
		config_string("server.name"), dedicated.minimum_players, dedicated.maximum_players,
		config_integer("server.countdown"),
		dedicated.rotation_count);
	for (index = 0; index < dedicated.rotation_count; index++)
	{
		platform_log("dedicated server:   %d. %s (%s)", index + 1, dedicated.rotation[index].map_path,
			dedicated.rotation[index].variant_name);
	}

	/* (the first host after the main menu settles) */
	dedicated.state = _dedicated_waiting;
	dedicated.wait_needed = 2.0f;
}

/* ---------- the rotation */

/* the names the built-in game types show (at most 11 characters: a
variant's name holds 12 with its terminator) */
static char const *dedicated_variant_display_name(
	char const *gametype)
{
	static struct
	{
		char const *gametype;
		char const *name;
	} const names[] =
	{
		{ "slayer", "Slayer" },
		{ "team_slayer", "Team Slayer" },
		{ "ctf", "CTF" },
		{ "ironctf", "Iron CTF" },
		{ "king", "King" },
		{ "team_king", "Team King" },
		{ "oddball", "Oddball" },
		{ "team_oddball", "TeamOddball" },
		{ "race", "Race" },
		{ "team_race", "Team Race" },
		{ "rally", "Rally" },
		{ "elimination", "Elimination" },
		{ "stalker", "Stalker" },
		{ "accumulation", "Accumulate" },
	};
	short index;

	for (index = 0; index < NUMBEROF(names); index++)
	{
		if (!strcmp(names[index].gametype, gametype))
			return names[index].name;
	}
	return gametype;
}

static void dedicated_entry_variant(
	struct dedicated_rotation_entry const *entry,
	struct game_variant *variant)
{
	struct game_variant built;

	csmemset(&built, 0, sizeof(built));
	*variant = *game_engine_get_variant_by_name(&built, entry->variant_name);
	/* the built-in variants have no name (the host's pregame screen names
	its choices from its own list), so the players' lobby and scores showed
	an empty game type: give the variant the game type's name */
	if (!variant->human_readable_game_description[0])
	{
		char const *name = dedicated_variant_display_name(entry->variant_name);
		short index;

		for (index = 0; name[index] && index < NUMBEROF(variant->human_readable_game_description) - 1; index++)
			variant->human_readable_game_description[index] = (wchar_t)(unsigned char)name[index];
		variant->human_readable_game_description[index] = 0;
	}
}

/* the game the next reset to the lobby sets up
(network_game_server_setup_game_from_playlist takes the stage) */
static void dedicated_set_stage(
	short index)
{
	struct dedicated_rotation_entry const *entry = &dedicated.rotation[index];
	struct game_variant variant;

	dedicated_entry_variant(entry, &variant);
	game_engine_override_map_name(entry->map_path);
	game_engine_override_game_variant(&variant);
}

/* the lobby's map and game type, as picking them in the pregame screen does */
static void dedicated_apply_to_lobby(
	struct network_game_server *server,
	short index)
{
	struct dedicated_rotation_entry const *entry = &dedicated.rotation[index];
	struct game_variant variant;

	dedicated_set_stage(index);
	network_game_server_change_map_name(server, entry->map_path);
	dedicated_entry_variant(entry, &variant);
	player_ui_set_game_variant(&variant);
	network_game_server_change_game_variant(server, &variant);
	dedicated.applied_index = index;
	platform_log("dedicated server: lobby open: %s (%s), game %d of %d in the rotation", entry->map_path,
		entry->variant_name, index + 1, dedicated.rotation_count);
}

/* ---------- shutting down */

static void dedicated_shut_down(
	void)
{
	struct network_game_server *server = global_network_game_server_get();

	platform_log("dedicated server: quit signal; shutting down");
	if (server && !network_game_server_dedicated_in_game(server))
		network_game_server_graceful_shutdown(server);
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	platform_log("dedicated server: stopped after %lu games", dedicated.games_hosted);
	exit(EXIT_SUCCESS);
}

/* ---------- public code */

void dedicated_server_update(
	boolean main_menu_loaded,
	real seconds)
{
	struct network_game_server *server;

	if (!halo_dedicated_server())
		return;
	dedicated_read_settings();
	if (platform_quit_requested())
		dedicated_shut_down();

	server = global_network_game_server_get();
	/* the game lost (a network failure, an abort: back at the main menu) */
	if (dedicated.state != _dedicated_waiting && !server)
	{
		platform_log("dedicated server: the game was lost; hosting again in %.0f seconds",
			(double)dedicated.rehost_seconds);
		dedicated.state = _dedicated_waiting;
		dedicated.wait_seconds = 0.0f;
		dedicated.wait_needed = dedicated.rehost_seconds;
		dedicated.applied_index = NONE;
		dedicated.last_player_count = NONE;
		dedicated.last_phase = NONE;
	}

	switch (dedicated.state)
	{
	case _dedicated_waiting:
		if (!main_menu_loaded)
			break;
		dedicated.wait_seconds += seconds;
		if (dedicated.wait_seconds < dedicated.wait_needed)
			break;
		dedicated.wait_seconds = 0.0f;
		dedicated.setup_seconds = 0.0f;
		dedicated.applied_index = NONE;
		player_ui_fast_setup_network_server();
		if (global_network_game_server_get())
		{
			dedicated.state = _dedicated_hosting;
			platform_log("dedicated server: hosting");
		}
		else
		{
			platform_log("dedicated server: cannot host; trying again in %.0f seconds",
				(double)dedicated.rehost_seconds);
			dedicated.wait_needed = dedicated.rehost_seconds;
		}
		break;

	case _dedicated_hosting:
		/* (the server's own client joined first, as network_test.c's host
		waits for it) */
		dedicated.setup_seconds += seconds;
		if (dedicated.setup_seconds >= 1.0f && network_game_server_dedicated_in_pregame(server))
		{
			dedicated_apply_to_lobby(server, dedicated.rotation_index);
			dedicated.state = _dedicated_running;
		}
		break;

	case _dedicated_running:
	{
		short player_count = network_game_server_dedicated_player_count(server);
		short phase = network_game_server_dedicated_in_pregame(server) ? 0 :
			network_game_server_dedicated_in_game(server) ? 1 : 2;

		if (phase != dedicated.last_phase)
		{
			static char const *const phase_names[] = { "lobby", "game", "scores" };

			platform_log("dedicated server: %s, %d players", phase_names[phase], player_count);
			if (phase == 1)
				dedicated.games_hosted++;
			dedicated.last_phase = phase;
			dedicated.last_player_count = player_count;
			dedicated.empty_elapsed = 0.0f;
			dedicated.ended_empty_game = FALSE;
			if (phase != 2)
			{
				dedicated.postgame_elapsed = 0.0f;
				dedicated.postgame_advanced = FALSE;
			}
		}
		else if (player_count != dedicated.last_player_count)
		{
			platform_log("dedicated server: %d players", player_count);
			dedicated.last_player_count = player_count;
		}

		switch (phase)
		{
		case 0:
			/* (after a reset the stage already had the entry: no change that
			would have every machine precache the map again) */
			if (dedicated.applied_index != dedicated.rotation_index)
				dedicated_apply_to_lobby(server, dedicated.rotation_index);
			network_game_server_dedicated_lobby_update(server, dedicated.minimum_players, dedicated.maximum_players,
				dedicated.countdown_milliseconds);
			break;
		case 1:
			if (player_count > 0 || dedicated.empty_seconds <= 0.0f)
			{
				dedicated.empty_elapsed = 0.0f;
			}
			else if (!dedicated.ended_empty_game)
			{
				dedicated.empty_elapsed += seconds;
				if (dedicated.empty_elapsed >= dedicated.empty_seconds && game_in_progress())
				{
					platform_log("dedicated server: nobody left in the game; ending it");
					game_engine_end_game();
					dedicated.ended_empty_game = TRUE;
				}
			}
			break;
		}
		break;
	}
	}
}

/* game_engine.c's postgame, in place of waiting for the host to press a
button: TRUE when the lobby is to open again (with the stage set to the
rotation's next game), again every second until it has */
boolean dedicated_server_postgame_update(
	real seconds)
{
	struct network_game_server *server = global_network_game_server_get();

	if (!halo_dedicated_server() || !server)
		return FALSE;
	if (!dedicated.postgame_advanced)
	{
		dedicated.postgame_elapsed += seconds;
		if (dedicated.postgame_elapsed < dedicated.postgame_seconds &&
			network_game_server_dedicated_player_count(server) > 0)
		{
			return FALSE;
		}
		dedicated.rotation_index = (short)((dedicated.rotation_index + 1) % dedicated.rotation_count);
		dedicated_set_stage(dedicated.rotation_index);
		/* (network_game_server_reset_to_pregame sets up the stage) */
		dedicated.applied_index = dedicated.rotation_index;
		dedicated.postgame_advanced = TRUE;
		dedicated.postgame_retry = 0.0f;
		platform_log("dedicated server: next game: %s (%s)", dedicated.rotation[dedicated.rotation_index].map_path,
			dedicated.rotation[dedicated.rotation_index].variant_name);
		return TRUE;
	}
	dedicated.postgame_retry += seconds;
	if (dedicated.postgame_retry < 1.0f)
		return FALSE;
	dedicated.postgame_retry = 0.0f;
	return TRUE;
}
