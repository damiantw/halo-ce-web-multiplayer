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

Its control channel (port/linux/README.md, "Dedicated server control"): the
process that started the server reads its events, JSON lines on stdout
(the transport is port/linux/src/dedicated_control.c), and sends it
commands on stdin. The events come from here, between frames: the lobby,
players joining and leaving, the game starting, its scores, its end, and a
status snapshot every server.status_interval seconds; so do the commands'
effects (a map change, the rotation, a kick). SIGHUP reads the rotation and
the timings again, SIGUSR1 asks for a status snapshot.

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
#include "game/players.h"
#include "memory/data.h"
#include "network_distributed.h"
#include "../src/dedicated_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
const char *config_string(char const *name);
long config_integer(char const *name);
int config_boolean(char const *name);
int config_reload(char const *const *names, int count);
void platform_log(char const *format, ...);
int halo_dedicated_server(void);
int platform_quit_requested(void);
int platform_quit_signal_number(void);
int platform_reload_requested(void);
int platform_status_requested(void);
int platform_data_has_map(char const *name);
int platform_data_map_type(char const *name);
/* network_server_manager.c's */
short network_game_server_dedicated_player_count(struct network_game_server *server);
boolean network_game_server_dedicated_in_pregame(struct network_game_server *server);
boolean network_game_server_dedicated_in_game(struct network_game_server *server);
void network_game_server_dedicated_lobby_update(struct network_game_server *server, long minimum_players,
	long maximum_players, long countdown_milliseconds);
boolean network_game_server_dedicated_countdown(struct network_game_server *server, long *milliseconds_remaining);
boolean network_game_server_dedicated_player(struct network_game_server *server, long slot, wchar_t *name,
	long *machine_index, long *controller_index, long *team_index);
short network_game_server_dedicated_player_color(struct network_game_server *server, long slot);
boolean network_game_server_dedicated_machine(struct network_game_server *server, long machine_index, wchar_t *name,
	unsigned long *address, word *port);
boolean network_game_server_dedicated_remove_machine(struct network_game_server *server, long machine_index);

enum
{
	MAXIMUM_ROTATION_ENTRIES = 64,
	DEDICATED_MAP_PATH_LENGTH = 128,
	DEDICATED_VARIANT_NAME_LENGTH = 32,
	DEDICATED_ROTATION_TEXT_LENGTH = 4096,
	/* server.max_players: system link's players on the Xbox */
	DEDICATED_MAXIMUM_PLAYERS = 16,

	DEDICATED_PLAYER_SLOTS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	DEDICATED_MACHINE_SLOTS = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	/* a player is known by its machine and controller */
	DEDICATED_PLAYER_KEYS = HALO_PORT_MAXIMUM_NETWORK_MACHINES * MAXIMUM_LOCAL_PLAYERS,
	/* players in the list (connected, and those who left the game in progress) */
	DEDICATED_MAXIMUM_ROWS = 256,
	DEDICATED_MAXIMUM_REJECTED = 16,
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

enum
{
	_phase_lobby,
	_phase_game,
	_phase_postgame,
};

struct dedicated_rotation_entry
{
	char map_path[DEDICATED_MAP_PATH_LENGTH];
	char variant_name[DEDICATED_VARIANT_NAME_LENGTH];
};

/* a player the server has seen, for the joined and left events */
struct dedicated_known_player
{
	boolean present;
	short slot;
	long team_index;
	short color_index;
	wchar_t name[12];
	wchar_t machine_name[32];
	unsigned long address;
	word port;
};

/* a line of the player list: a player in the server's list and, in a game,
its statistics; or a player who left the game in progress */
struct dedicated_player_row
{
	long slot;
	long machine_index;
	long controller_index;
	long team_index;
	short color_index;
	wchar_t name[12];
	boolean connected;
	boolean has_statistics;
	long player_index;
	long kills;
	long deaths;
	long assists;
	long suicides;
	long team_kills;
	long score;
};

/* what the score event is written for a change of */
struct dedicated_score_state
{
	long kills;
	long deaths;
	long assists;
	long suicides;
	long team_kills;
	long score;
	long team_score;
};

struct dedicated_rejected_entry
{
	char text[DEDICATED_MAP_PATH_LENGTH];
	char reason[96];
};

static struct
{
	boolean read_settings;
	char rotation_text[DEDICATED_ROTATION_TEXT_LENGTH];
	struct dedicated_rotation_entry rotation[MAXIMUM_ROTATION_ENTRIES];
	short rotation_count;
	/* the rotation's entry the current game is, or was the last one before
	a change_map game (NONE: the next is the first) */
	short rotation_position;
	/* the game the lobby has (or is to have) */
	struct dedicated_rotation_entry current;
	boolean current_from_rotation;
	/* the lobby has current's map and game type */
	boolean applied;
	/* change_map's game, played next */
	struct dedicated_rotation_entry pending;
	boolean pending_valid;
	/* a command changed the next game after the scores had already set it:
	the lobby takes the new one */
	boolean reapply_next;

	long minimum_players;
	long maximum_players;
	long countdown_milliseconds;
	real postgame_seconds;
	real empty_seconds;
	real rehost_seconds;
	real status_interval;
	boolean exit_on_eof;

	short state;
	real wait_seconds;
	real wait_needed;
	real setup_seconds;
	real empty_elapsed;
	boolean ended_empty_game;
	real postgame_elapsed;
	boolean postgame_advanced;
	boolean skip_postgame;
	real postgame_retry;
	short last_player_count;
	short last_phase;
	unsigned long games_hosted;

	/* the control channel */
	real uptime;
	real status_elapsed;
	boolean announced;
	boolean quit_requested;
	char const *quit_reason;
	/* the lobby */
	boolean lobby_countdown;
	short lobby_players;
	/* the game */
	boolean game_start_pending;
	boolean game_reported;
	boolean game_running_seen;
	boolean game_end_reported;
	char const *end_reason;
	struct dedicated_rotation_entry game_entry;
	real score_elapsed;
	boolean score_changed;
	struct dedicated_score_state scores[DEDICATED_MAXIMUM_ROWS];
	/* the players */
	struct dedicated_known_player known[DEDICATED_PLAYER_KEYS];
	boolean kicked[DEDICATED_MACHINE_SLOTS];

	struct dedicated_rejected_entry rejected[DEDICATED_MAXIMUM_REJECTED];
	short rejected_count;
} dedicated;

static struct dedicated_player_row dedicated_rows[DEDICATED_MAXIMUM_ROWS];
static short dedicated_row_count;
static struct control_command dedicated_command;

/* ---------- settings */

static boolean dedicated_variant_is_known(
	char const *name)
{
	struct game_variant variant;

	csmemset(&variant, 0, sizeof(variant));
	game_engine_get_variant_by_name(&variant, name);
	return variant.game_engine_index != 0;
}

static void dedicated_reject(
	char const *text,
	char const *reason)
{
	platform_log("dedicated server: rotation entry \"%s\": %s; left out", text, reason);
	if (dedicated.rejected_count < DEDICATED_MAXIMUM_REJECTED)
	{
		struct dedicated_rejected_entry *rejected = &dedicated.rejected[dedicated.rejected_count++];

		snprintf(rejected->text, sizeof(rejected->text), "%s", text);
		snprintf(rejected->reason, sizeof(rejected->reason), "%s", reason);
	}
}

/* "<map>[:<variant>]": a full scenario path (with backslashes) or a
multiplayer map's name, levels\test\<map>\<map>; FALSE (and the reason
kept in dedicated.rejected) if it cannot be played */
static boolean dedicated_parse_entry(
	char const *text,
	struct dedicated_rotation_entry *entry)
{
	char map[DEDICATED_MAP_PATH_LENGTH];
	char variant[DEDICATED_VARIANT_NAME_LENGTH] = "slayer";
	char reason[96];
	char const *colon = strchr(text, ':');
	char const *base;
	size_t length = colon ? (size_t)(colon - text) : strlen(text);
	int map_type;

	if (!length)
	{
		dedicated_reject(text, "no map");
		return FALSE;
	}
	if (length >= sizeof(map) || (colon && strlen(colon + 1) >= sizeof(variant)))
	{
		dedicated_reject(text, "too long");
		return FALSE;
	}
	memcpy(map, text, length);
	map[length] = 0;
	if (colon && colon[1])
		snprintf(variant, sizeof(variant), "%s", colon + 1);
	if (!dedicated_variant_is_known(variant))
	{
		snprintf(reason, sizeof(reason), "no game type \"%s\"", variant);
		dedicated_reject(text, reason);
		return FALSE;
	}
	base = strrchr(map, '\\');
	base = base ? base + 1 : map;
	if (!platform_data_has_map(base))
	{
		snprintf(reason, sizeof(reason), "no maps/%s.map", base);
		dedicated_reject(text, reason);
		return FALSE;
	}
	/* (a campaign level or the main menu cannot be hosted: the players'
	machines leave, and the lobby waits forever) */
	map_type = platform_data_map_type(base);
	if (map_type == 0 || map_type == 2)
	{
		snprintf(reason, sizeof(reason), "maps/%s.map is %s, not a multiplayer map", base,
			map_type == 0 ? "a campaign level" : "the main menu");
		dedicated_reject(text, reason);
		return FALSE;
	}
	if (strchr(map, '\\'))
		snprintf(entry->map_path, sizeof(entry->map_path), "%s", map);
	else
		snprintf(entry->map_path, sizeof(entry->map_path), "levels\\test\\%s\\%s", map, map);
	snprintf(entry->variant_name, sizeof(entry->variant_name), "%s", variant);
	return TRUE;
}

/* the entries of a rotation's text (commas, semicolons or spaces between
them) that can be played; their count */
static short dedicated_parse_rotation(
	char const *text,
	struct dedicated_rotation_entry *entries)
{
	char copy[DEDICATED_ROTATION_TEXT_LENGTH];
	char *token;
	short count = 0;

	dedicated.rejected_count = 0;
	snprintf(copy, sizeof(copy), "%s", text);
	for (token = strtok(copy, ",; \t\r\n"); token; token = strtok(NULL, ",; \t\r\n"))
	{
		if (count >= MAXIMUM_ROTATION_ENTRIES)
		{
			dedicated_reject(token, "more than 64 entries");
			continue;
		}
		if (dedicated_parse_entry(token, &entries[count]))
			count++;
	}
	return count;
}

static void dedicated_log_rotation(
	void)
{
	short index;

	platform_log("dedicated server: \"%s\", %ld to %ld players, %ld second countdown, %d game rotation:",
		config_string("server.name"), dedicated.minimum_players, dedicated.maximum_players, (dedicated.countdown_milliseconds + 1) / 1000,
		dedicated.rotation_count);
	for (index = 0; index < dedicated.rotation_count; index++)
	{
		platform_log("dedicated server:   %d. %s (%s)", index + 1, dedicated.rotation[index].map_path,
			dedicated.rotation[index].variant_name);
	}
}

/* a new rotation (at least one entry): the game being played goes on, the
next is the new rotation's first */
static void dedicated_set_rotation(
	char const *text,
	struct dedicated_rotation_entry const *entries,
	short count)
{
	snprintf(dedicated.rotation_text, sizeof(dedicated.rotation_text), "%s", text);
	csmemcpy(dedicated.rotation, entries, count * sizeof(entries[0]));
	dedicated.rotation_count = count;
	dedicated.rotation_position = NONE;
	dedicated.current_from_rotation = FALSE;
}

static real dedicated_seconds_setting(
	char const *name,
	long minimum,
	long maximum)
{
	long value = config_integer(name);

	return (real)PIN(value, minimum, maximum);
}

/* the timings and the other numbers (the start, and a reload) */
static void dedicated_read_numbers(
	void)
{
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
	dedicated.status_interval = dedicated_seconds_setting("server.status_interval", 0, 3600);
	dedicated.exit_on_eof = config_boolean("server.control_exit_on_eof") != 0;
}

static void dedicated_read_settings(
	void)
{
	static struct dedicated_rotation_entry entries[MAXIMUM_ROTATION_ENTRIES];
	short count;

	if (dedicated.read_settings)
		return;
	dedicated.read_settings = TRUE;
	dedicated.last_player_count = NONE;
	dedicated.last_phase = NONE;

	dedicated_read_numbers();
	count = dedicated_parse_rotation(config_string("server.rotation"), entries);
	if (!count)
	{
		platform_log("dedicated server: no playable entry in server.rotation (\"%s\"); hosting bloodgulch:slayer",
			config_string("server.rotation"));
		control_error(NULL, "no_playable_rotation", "no playable entry in server.rotation (\"%s\"); hosting "
			"bloodgulch:slayer", config_string("server.rotation"));
		snprintf(entries[0].map_path, sizeof(entries[0].map_path), "levels\\test\\bloodgulch\\bloodgulch");
		snprintf(entries[0].variant_name, sizeof(entries[0].variant_name), "slayer");
		count = 1;
	}
	else
	{
		short index;

		for (index = 0; index < dedicated.rejected_count; index++)
		{
			control_error(NULL, "rotation_entry_rejected", "rotation entry \"%s\": %s", dedicated.rejected[index].text,
				dedicated.rejected[index].reason);
		}
	}
	dedicated_set_rotation(config_string("server.rotation"), entries, count);
	dedicated_log_rotation();
	/* (the first game: the rotation's first) */
	dedicated.rotation_position = 0;
	dedicated.current = dedicated.rotation[0];
	dedicated.current_from_rotation = TRUE;
	dedicated.applied = FALSE;

	/* (the first host after the main menu settles) */
	dedicated.state = _dedicated_waiting;
	dedicated.wait_needed = 2.0f;
}

/* ---------- the rotation */

/* the game that follows the current one, and the rotation's entry it is
(unchanged for change_map's game) */
static void dedicated_peek_next(
	struct dedicated_rotation_entry *entry,
	short *position)
{
	if (dedicated.pending_valid)
	{
		*entry = dedicated.pending;
		*position = dedicated.rotation_position;
	}
	else
	{
		*position = dedicated.rotation_position == NONE ? 0 :
			(short)((dedicated.rotation_position + 1) % dedicated.rotation_count);
		*entry = dedicated.rotation[*position];
	}
}

/* the next game becomes the current one (for the lobby to take) */
static void dedicated_advance(
	void)
{
	short position;

	dedicated_peek_next(&dedicated.current, &position);
	dedicated.current_from_rotation = !dedicated.pending_valid;
	dedicated.rotation_position = position;
	dedicated.pending_valid = FALSE;
	dedicated.reapply_next = FALSE;
	dedicated.applied = FALSE;
}

/* the game after the current one: what the scores set (after they have),
else what follows */
static void dedicated_upcoming(
	struct dedicated_rotation_entry *entry)
{
	short position;

	if (dedicated.postgame_advanced && !dedicated.reapply_next)
		*entry = dedicated.current;
	else
		dedicated_peek_next(entry, &position);
}

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
	struct dedicated_rotation_entry const *entry)
{
	struct game_variant variant;

	dedicated_entry_variant(entry, &variant);
	game_engine_override_map_name(entry->map_path);
	game_engine_override_game_variant(&variant);
}

static void dedicated_emit_lobby(struct network_game_server *server, char const *reason);

/* the lobby's map and game type, as picking them in the pregame screen does */
static void dedicated_apply_to_lobby(
	struct network_game_server *server)
{
	struct dedicated_rotation_entry const *entry = &dedicated.current;
	struct game_variant variant;
	boolean opened = !dedicated.announced || dedicated.last_phase != _phase_lobby;

	dedicated_set_stage(entry);
	network_game_server_change_map_name(server, entry->map_path);
	dedicated_entry_variant(entry, &variant);
	player_ui_set_game_variant(&variant);
	network_game_server_change_game_variant(server, &variant);
	dedicated.applied = TRUE;
	if (dedicated.current_from_rotation)
	{
		platform_log("dedicated server: lobby: %s (%s), game %d of %d in the rotation", entry->map_path,
			entry->variant_name, dedicated.rotation_position + 1, dedicated.rotation_count);
	}
	else
	{
		platform_log("dedicated server: lobby: %s (%s), set by a command", entry->map_path, entry->variant_name);
	}
	if (!opened)
		dedicated_emit_lobby(server, "map_changed");
}

/* ---------- names */

static char const *dedicated_map_name(
	char const *map_path)
{
	char const *base = strrchr(map_path, '\\');

	return base ? base + 1 : map_path;
}

static char const *dedicated_engine_name(
	long engine_index)
{
	static char const *const names[] =
	{
		"none", "ctf", "slayer", "oddball", "king", "race", "terminator", "stub",
	};

	return engine_index >= 0 && engine_index < NUMBEROF(names) ? names[engine_index] : "unknown";
}

/* a player's colour, by its index in player_profile.c's colour table (the
order of the game's colour picker); NULL for none */
static char const *dedicated_color_name(
	long color_index)
{
	static char const *const names[] =
	{
		"white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple",
		"cyan", "cobalt", "orange", "teal", "sage", "brown", "tan", "maroon", "salmon",
	};

	return color_index >= 0 && color_index < NUMBEROF(names) ? names[color_index] : NULL;
}

static void dedicated_write_color(
	long color_index)
{
	char const *name = dedicated_color_name(color_index);

	if (name)
		control_field_string("color", name);
	else
		control_field_null("color");
}

static char const *dedicated_team_name(
	long team_index)
{
	switch (team_index)
	{
	case _team_red: return "red";
	case _team_blue: return "blue";
	}
	return NULL;
}

static char const *dedicated_state_name(
	void)
{
	struct network_game_server *server = global_network_game_server_get();

	switch (dedicated.state)
	{
	case _dedicated_waiting:
		return dedicated.announced ? "waiting" : "starting";
	case _dedicated_hosting:
		return "hosting";
	}
	if (network_game_server_dedicated_in_pregame(server))
		return "lobby";
	if (network_game_server_dedicated_in_game(server))
		return "game";
	return "postgame";
}

static void dedicated_format_address(
	unsigned long address,
	char *text,
	size_t size)
{
	snprintf(text, size, "%lu.%lu.%lu.%lu", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF,
		address & 0xFF);
}

/* ---------- writing: games */

/* "map", "map_path", "gametype" */
static void dedicated_write_entry_fields(
	struct dedicated_rotation_entry const *entry)
{
	control_field_string("map", dedicated_map_name(entry->map_path));
	control_field_string("map_path", entry->map_path);
	control_field_string("gametype", entry->variant_name);
}

static void dedicated_write_entry(
	char const *key,
	struct dedicated_rotation_entry const *entry)
{
	if (key)
		control_key(key);
	control_object_begin();
	dedicated_write_entry_fields(entry);
	control_object_end();
}

static void dedicated_write_rotation(
	void)
{
	short index;

	control_key("rotation");
	control_array_begin();
	for (index = 0; index < dedicated.rotation_count; index++)
		dedicated_write_entry(NULL, &dedicated.rotation[index]);
	control_array_end();
}

static void dedicated_write_rejected(
	void)
{
	short index;

	control_key("rejected");
	control_array_begin();
	for (index = 0; index < dedicated.rejected_count; index++)
	{
		control_object_begin();
		control_field_string("entry", dedicated.rejected[index].text);
		control_field_string("reason", dedicated.rejected[index].reason);
		control_object_end();
	}
	control_array_end();
}

/* the game type's fields, from the variant being played (in a game) or the
entry's */
static void dedicated_write_variant_fields(
	struct dedicated_rotation_entry const *entry,
	boolean in_game)
{
	struct game_variant variant;

	if (in_game && game_engine)
		variant = *game_engine_get_variant();
	else
		dedicated_entry_variant(entry, &variant);
	control_field_utf16("variant", (unsigned short const *)variant.human_readable_game_description,
		NUMBEROF(variant.human_readable_game_description));
	control_field_string("engine", dedicated_engine_name(variant.game_engine_index));
	control_field_boolean("teams", variant.universal_variant.teams);
	control_field_integer("score_limit", variant.universal_variant.score_to_win);
}

static boolean dedicated_game_has_teams(
	void)
{
	return game_engine && game_engine_get_variant()->universal_variant.teams;
}

/* ---------- players */

static boolean dedicated_statistics_available(
	struct network_game_server *server)
{
	return server && !network_game_server_dedicated_in_pregame(server) && game_engine && game_in_progress() &&
		player_data && player_data->valid;
}

/* the players' teams mean something: the game's variant in a game, else the
lobby's */
static boolean dedicated_teams(
	struct network_game_server *server)
{
	struct game_variant variant;

	if (dedicated_statistics_available(server))
		return dedicated_game_has_teams();
	dedicated_entry_variant(network_game_server_dedicated_in_pregame(server) || !dedicated.game_reported ?
		&dedicated.current : &dedicated.game_entry, &variant);
	return variant.universal_variant.teams;
}

static void dedicated_row_statistics(
	struct dedicated_player_row *row,
	long player_index,
	struct player_datum const *player)
{
	row->has_statistics = TRUE;
	row->player_index = player_index;
	row->team_index = player->team_index;
	row->kills = player->statistics.kills[0];
	row->deaths = player->statistics.deaths;
	row->assists = player->statistics.assists[0];
	row->suicides = player->statistics.suicides;
	row->team_kills = player->statistics.friendly_fire_kills;
	row->score = game_engine && game_engine->get_player_score ?
		game_engine->get_player_score(player_index, _get_score_individual) : row->kills;
}

/* the player list: the server's players (in a game, with their statistics)
and, in a game, those who left it */
static short dedicated_collect_players(
	struct network_game_server *server)
{
	boolean statistics = dedicated_statistics_available(server);
	boolean matched[DEDICATED_MAXIMUM_ROWS];
	struct data_iterator iterator;
	struct player_datum *player;
	long slot;

	csmemset(matched, 0, sizeof(matched));
	dedicated_row_count = 0;
	for (slot = 0; server && slot < DEDICATED_PLAYER_SLOTS && dedicated_row_count < DEDICATED_MAXIMUM_ROWS; slot++)
	{
		struct dedicated_player_row *row = &dedicated_rows[dedicated_row_count];

		csmemset(row, 0, sizeof(*row));
		if (!network_game_server_dedicated_player(server, slot, row->name, &row->machine_index, &row->controller_index,
			&row->team_index))
		{
			continue;
		}
		row->slot = slot;
		row->connected = TRUE;
		row->player_index = NONE;
		row->color_index = network_game_server_dedicated_player_color(server, slot);
		if (statistics)
		{
			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

				if (absolute_index < DEDICATED_MAXIMUM_ROWS && !matched[absolute_index] && !player->quit_out_of_game &&
					player->network_player_data.machine_index == row->machine_index &&
					player->network_player_data.controller_index == row->controller_index)
				{
					matched[absolute_index] = TRUE;
					dedicated_row_statistics(row, iterator.datum_index, player);
					break;
				}
			}
		}
		dedicated_row_count++;
	}
	if (statistics)
	{
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL &&
			dedicated_row_count < DEDICATED_MAXIMUM_ROWS)
		{
			long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
			struct dedicated_player_row *row = &dedicated_rows[dedicated_row_count];

			if (absolute_index < DEDICATED_MAXIMUM_ROWS && matched[absolute_index])
				continue;
			csmemset(row, 0, sizeof(*row));
			row->slot = NONE;
			row->machine_index = player->network_player_data.machine_index;
			row->controller_index = player->network_player_data.controller_index;
			row->color_index = player->network_player_data.primary_color_index;
			csmemcpy(row->name, player->name, sizeof(row->name));
			row->name[NUMBEROF(row->name) - 1] = 0;
			row->connected = FALSE;
			dedicated_row_statistics(row, iterator.datum_index, player);
			dedicated_row_count++;
		}
	}
	return dedicated_row_count;
}

static void dedicated_write_player(
	struct network_game_server *server,
	struct dedicated_player_row const *row,
	boolean has_teams,
	boolean results)
{
	wchar_t machine_name[32];
	unsigned long address = 0;
	word port = 0;
	boolean machine = row->connected &&
		network_game_server_dedicated_machine(server, row->machine_index, machine_name, &address, &port);
	real round_trip;

	control_object_begin();
	if (row->slot != NONE)
		control_field_integer("player", row->slot);
	else
		control_field_null("player");
	control_field_utf16("name", (unsigned short const *)row->name, NUMBEROF(row->name));
	control_field_integer("machine", row->machine_index);
	control_field_integer("controller", row->controller_index);
	dedicated_write_color(row->color_index);
	if (has_teams)
	{
		control_field_integer("team", row->team_index);
		control_field_string("team_name", dedicated_team_name(row->team_index));
	}
	else
	{
		control_field_null("team");
		control_field_null("team_name");
	}
	control_field_boolean("connected", row->connected);
	if (machine)
	{
		char text[32];

		control_field_utf16("machine_name", (unsigned short const *)machine_name, NUMBEROF(machine_name));
		if (address)
		{
			dedicated_format_address(address, text, sizeof(text));
			control_field_string("address", text);
			control_field_integer("port", port);
		}
		else
		{
			control_field_null("address");
			control_field_null("port");
		}
	}
	else
	{
		control_field_null("machine_name");
		control_field_null("address");
		control_field_null("port");
	}
	if (row->connected && network_game_distributed() &&
		distributed_machine_round_trip_measured(row->machine_index, &round_trip))
	{
		control_field_integer("ping_ms", (long)(round_trip * 1000.0f / TICKS_PER_SECOND + 0.5f));
	}
	else
	{
		control_field_null("ping_ms");
	}
	if (row->has_statistics)
	{
		wchar_t score_text[256];

		control_field_integer("kills", row->kills);
		control_field_integer("deaths", row->deaths);
		control_field_integer("assists", row->assists);
		control_field_integer("suicides", row->suicides);
		control_field_integer("team_kills", row->team_kills);
		control_field_integer("score", row->score);
		score_text[0] = 0;
		if (game_engine && game_engine->format_player_score)
			game_engine->format_player_score(row->player_index, score_text);
		score_text[NUMBEROF(score_text) - 1] = 0;
		control_field_utf16("score_text", (unsigned short const *)score_text, NUMBEROF(score_text));
		if (results)
		{
			long won = game_engine_did_player_win(row->player_index);

			control_key("won");
			if (won == NONE)
				control_null();
			else
				control_boolean(won != 0);
		}
	}
	control_object_end();
}

static void dedicated_write_players(
	struct network_game_server *server,
	boolean results)
{
	boolean has_teams = dedicated_teams(server);
	short index;

	control_key("players");
	control_array_begin();
	for (index = 0; index < dedicated_row_count; index++)
		dedicated_write_player(server, &dedicated_rows[index], has_teams, results);
	control_array_end();
}

/* the teams' scores (a team game with statistics; null otherwise), from the
first player of each team in the list */
static void dedicated_write_team_scores(
	struct network_game_server *server)
{
	boolean written[DEDICATED_MAXIMUM_ROWS];
	short index;

	control_key("team_scores");
	if (!dedicated_statistics_available(server) || !dedicated_game_has_teams() || !game_engine->get_player_score)
	{
		control_null();
		return;
	}
	csmemset(written, 0, sizeof(written));
	control_array_begin();
	for (index = 0; index < dedicated_row_count; index++)
	{
		struct dedicated_player_row const *row = &dedicated_rows[index];

		if (!row->has_statistics || row->team_index < 0 || row->team_index >= DEDICATED_MAXIMUM_ROWS ||
			written[row->team_index])
		{
			continue;
		}
		written[row->team_index] = TRUE;
		control_object_begin();
		control_field_integer("team", row->team_index);
		control_field_string("name", dedicated_team_name(row->team_index));
		control_field_integer("score", game_engine->get_player_score(row->player_index, _get_score_team));
		control_object_end();
	}
	control_array_end();
}

/* ---------- events */

/* the fields of the game being played: its type, time, scores and players */
static void dedicated_write_game_fields(
	struct network_game_server *server,
	boolean results)
{
	boolean statistics = dedicated_statistics_available(server);

	dedicated_write_entry_fields(&dedicated.game_entry);
	dedicated_write_variant_fields(&dedicated.game_entry, statistics);
	if (statistics)
		control_field_real("time_elapsed", (real)game_time_get() / TICKS_PER_SECOND, 1);
	else
		control_field_null("time_elapsed");
	/* (the Xbox game types have no time limit: a game ends at its score) */
	control_field_null("time_remaining");
	dedicated_collect_players(server);
	dedicated_write_team_scores(server);
	dedicated_write_players(server, results);
}

static void dedicated_write_lobby_fields(
	struct network_game_server *server)
{
	long milliseconds;

	control_field_integer("player_count", network_game_server_dedicated_player_count(server));
	control_field_integer("minimum_players", dedicated.minimum_players);
	control_field_integer("max_players", dedicated.maximum_players);
	control_key("countdown");
	if (network_game_server_dedicated_countdown(server, &milliseconds))
		control_real(milliseconds / 1000.0, 1);
	else
		control_null();
}

static void dedicated_emit_lobby(
	struct network_game_server *server,
	char const *reason)
{
	control_begin("lobby");
	control_field_string("reason", reason);
	dedicated_write_entry_fields(&dedicated.current);
	dedicated_write_variant_fields(&dedicated.current, FALSE);
	dedicated_write_lobby_fields(server);
	dedicated_collect_players(server);
	dedicated_write_players(server, FALSE);
	control_end();
}

/* the snapshot: "status" (every server.status_interval seconds, SIGUSR1 and
the status command) */
static void dedicated_emit_status(
	struct control_command const *command)
{
	struct network_game_server *server = global_network_game_server_get();
	struct dedicated_rotation_entry upcoming;
	char const *state = dedicated_state_name();

	if (command)
		control_begin_reply("status", command);
	else
		control_begin("status");
	control_field_string("state", state);
	control_field_string("name", config_string("server.name"));
	control_field_real("uptime", dedicated.uptime, 1);
	control_field_integer("games_hosted", (long)dedicated.games_hosted);
	dedicated_write_rotation();
	control_key("rotation_index");
	if (dedicated.current_from_rotation && dedicated.rotation_position != NONE)
		control_integer(dedicated.rotation_position);
	else
		control_null();
	dedicated_upcoming(&upcoming);
	dedicated_write_entry("next", &upcoming);
	if (!strcmp(state, "game") || !strcmp(state, "postgame"))
	{
		control_field_null("lobby");
		control_key("game");
		control_object_begin();
		control_field_boolean("ended", dedicated.game_end_reported);
		dedicated_write_game_fields(server, FALSE);
		control_object_end();
	}
	else if (!strcmp(state, "lobby"))
	{
		control_key("lobby");
		control_object_begin();
		dedicated_write_entry_fields(&dedicated.current);
		dedicated_write_variant_fields(&dedicated.current, FALSE);
		dedicated_write_lobby_fields(server);
		dedicated_collect_players(server);
		dedicated_write_players(server, FALSE);
		control_object_end();
		control_field_null("game");
	}
	else
	{
		control_field_null("lobby");
		control_field_null("game");
	}
	control_end();
	dedicated.status_elapsed = 0.0f;
}

static void dedicated_write_settings(
	void)
{
	control_key("settings");
	control_object_begin();
	control_field_integer("countdown", (dedicated.countdown_milliseconds + 1) / 1000);
	control_field_integer("minimum_players", dedicated.minimum_players);
	control_field_integer("max_players", dedicated.maximum_players);
	control_field_integer("postgame_seconds", (long)dedicated.postgame_seconds);
	control_field_integer("empty_seconds", (long)dedicated.empty_seconds);
	control_field_integer("rehost_seconds", (long)dedicated.rehost_seconds);
	control_field_integer("status_interval", (long)dedicated.status_interval);
	control_field_boolean("exit_on_eof", dedicated.exit_on_eof);
	control_object_end();
}

static void dedicated_emit_server_started(
	void)
{
	control_begin("server_started");
	control_field_string("name", config_string("server.name"));
	control_field_integer("protocol", DEDICATED_CONTROL_PROTOCOL_VERSION);
	control_field_boolean("distributed", network_game_distributed());
	dedicated_write_rotation();
	dedicated_write_settings();
	control_end();
}

static void dedicated_emit_game_ended(
	struct network_game_server *server)
{
	struct dedicated_rotation_entry upcoming;

	if (!dedicated.game_reported || dedicated.game_end_reported)
		return;
	dedicated.game_end_reported = TRUE;
	control_begin("game_ended");
	control_field_string("reason", dedicated.end_reason ? dedicated.end_reason : "game_over");
	dedicated_write_game_fields(server, dedicated_statistics_available(server));
	dedicated_upcoming(&upcoming);
	dedicated_write_entry("next", &upcoming);
	control_end();
}

/* ---------- the players' joins and departures */

static void dedicated_write_known_player(
	struct dedicated_known_player const *known,
	long key)
{
	char text[32];

	control_field_integer("player", known->slot);
	control_field_utf16("name", (unsigned short const *)known->name, NUMBEROF(known->name));
	control_field_integer("machine", key / MAXIMUM_LOCAL_PLAYERS);
	control_field_integer("controller", key % MAXIMUM_LOCAL_PLAYERS);
	dedicated_write_color(known->color_index);
	if (known->team_index >= 0)
	{
		control_field_integer("team", known->team_index);
		control_field_string("team_name", dedicated_team_name(known->team_index));
	}
	else
	{
		control_field_null("team");
		control_field_null("team_name");
	}
	control_field_utf16("machine_name", (unsigned short const *)known->machine_name, NUMBEROF(known->machine_name));
	if (known->address)
	{
		dedicated_format_address(known->address, text, sizeof(text));
		control_field_string("address", text);
		control_field_integer("port", known->port);
	}
	else
	{
		control_field_null("address");
		control_field_null("port");
	}
}

/* every frame: the server's players against those it had (by machine and
controller), a player_joined or player_left event for each difference */
static void dedicated_update_players(
	struct network_game_server *server)
{
	static boolean present[DEDICATED_PLAYER_KEYS];
	boolean teams = dedicated_teams(server);
	long slot;
	long key;

	csmemset(present, 0, sizeof(present));
	for (slot = 0; server && slot < DEDICATED_PLAYER_SLOTS; slot++)
	{
		wchar_t name[12];
		long machine_index, controller_index, team_index;
		struct dedicated_known_player *known;

		if (!network_game_server_dedicated_player(server, slot, name, &machine_index, &controller_index, &team_index) ||
			machine_index < 0 || machine_index >= DEDICATED_MACHINE_SLOTS || controller_index < 0 ||
			controller_index >= MAXIMUM_LOCAL_PLAYERS)
		{
			continue;
		}
		key = machine_index * MAXIMUM_LOCAL_PLAYERS + controller_index;
		known = &dedicated.known[key];
		present[key] = TRUE;
		if (known->present && !csmemcmp(known->name, name, sizeof(name)))
		{
			known->slot = (short)slot;
			known->team_index = teams ? team_index : NONE;
			known->color_index = network_game_server_dedicated_player_color(server, slot);
			continue;
		}
		if (known->present)
		{
			/* (another player in its place: the one before left) */
			control_begin("player_left");
			dedicated_write_known_player(known, key);
			control_field_string("reason", "left");
			control_end();
		}
		csmemset(known, 0, sizeof(*known));
		known->present = TRUE;
		known->slot = (short)slot;
		known->team_index = teams ? team_index : NONE;
		known->color_index = network_game_server_dedicated_player_color(server, slot);
		csmemcpy(known->name, name, sizeof(name));
		network_game_server_dedicated_machine(server, machine_index, known->machine_name, &known->address,
			&known->port);
		dedicated.kicked[machine_index] = FALSE;
		control_begin("player_joined");
		dedicated_write_known_player(known, key);
		control_field_boolean("in_game", network_game_server_dedicated_in_game(server));
		control_end();
		platform_log("dedicated server: player joined: slot %ld, machine %ld", slot, machine_index);
	}
	for (key = 0; key < DEDICATED_PLAYER_KEYS; key++)
	{
		struct dedicated_known_player *known = &dedicated.known[key];

		if (!known->present || present[key])
			continue;
		control_begin("player_left");
		dedicated_write_known_player(known, key);
		control_field_string("reason", dedicated.kicked[key / MAXIMUM_LOCAL_PLAYERS] ? "kicked" : "left");
		control_end();
		platform_log("dedicated server: player left: slot %d, machine %ld", known->slot, key / MAXIMUM_LOCAL_PLAYERS);
		known->present = FALSE;
	}
}

/* ---------- the game's scores */

/* every frame of a game: a score event when a statistic or a score has
changed, a second apart at most */
static void dedicated_update_scores(
	struct network_game_server *server,
	real seconds)
{
	boolean has_teams = dedicated_game_has_teams();
	short index;

	dedicated.score_elapsed += seconds;
	dedicated_collect_players(server);
	for (index = 0; index < dedicated_row_count; index++)
	{
		struct dedicated_player_row const *row = &dedicated_rows[index];
		struct dedicated_score_state state;
		long absolute_index;

		if (!row->has_statistics)
			continue;
		absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(row->player_index);
		if (absolute_index >= DEDICATED_MAXIMUM_ROWS)
			continue;
		state.kills = row->kills;
		state.deaths = row->deaths;
		state.assists = row->assists;
		state.suicides = row->suicides;
		state.team_kills = row->team_kills;
		state.score = row->score;
		state.team_score = has_teams && game_engine->get_player_score ?
			game_engine->get_player_score(row->player_index, _get_score_team) : 0;
		if (csmemcmp(&state, &dedicated.scores[absolute_index], sizeof(state)))
		{
			dedicated.scores[absolute_index] = state;
			dedicated.score_changed = TRUE;
		}
	}
	if (dedicated.score_changed && dedicated.score_elapsed >= 1.0f)
	{
		dedicated.score_changed = FALSE;
		dedicated.score_elapsed = 0.0f;
		control_begin("score");
		dedicated_write_entry_fields(&dedicated.game_entry);
		control_field_real("time_elapsed", (real)game_time_get() / TICKS_PER_SECOND, 1);
		dedicated_write_team_scores(server);
		dedicated_write_players(server, FALSE);
		control_end();
	}
}

/* ---------- shutting down */

static void dedicated_shut_down(
	char const *reason)
{
	struct network_game_server *server = global_network_game_server_get();

	platform_log("dedicated server: %s; shutting down", reason);
	if (server && !network_game_server_dedicated_in_game(server))
		network_game_server_graceful_shutdown(server);
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	platform_log("dedicated server: stopped after %lu games", dedicated.games_hosted);
	control_begin("shutdown");
	control_field_string("reason", dedicated.quit_reason ? dedicated.quit_reason : "signal");
	if (platform_quit_signal_number())
		control_field_integer("signal", platform_quit_signal_number());
	else
		control_field_null("signal");
	control_field_boolean("immediate", FALSE);
	control_field_integer("games_hosted", (long)dedicated.games_hosted);
	control_end();
	dedicated_control_drain(2000);
	exit(EXIT_SUCCESS);
}

/* ---------- reloading the settings */

/* the rotation and the timings, read again (SIGHUP, the reload command):
TRUE when the rotation changed */
static boolean dedicated_reload(
	struct control_command const *command)
{
	static char const *const names[] =
	{
		"server.rotation", "server.countdown", "server.minimum_players", "server.max_players", "server.postgame_seconds",
		"server.empty_seconds", "server.rehost_seconds", "server.status_interval", "server.control_exit_on_eof",
	};
	static struct dedicated_rotation_entry entries[MAXIMUM_ROTATION_ENTRIES];
	boolean rotation_changed = FALSE;
	short count;

	if (!config_reload(names, NUMBEROF(names)))
	{
		control_error(command, "reload_failed", "config.toml has errors (the log has them); nothing changed");
		return FALSE;
	}
	dedicated_read_numbers();
	dedicated.rejected_count = 0;
	if (strcmp(config_string("server.rotation"), dedicated.rotation_text) != 0)
	{
		count = dedicated_parse_rotation(config_string("server.rotation"), entries);
		if (count)
		{
			dedicated_set_rotation(config_string("server.rotation"), entries, count);
			rotation_changed = TRUE;
			dedicated_log_rotation();
		}
		else
		{
			control_error(command, "invalid_rotation", "no playable entry in the new server.rotation (\"%s\"); "
				"the rotation stays as it was", config_string("server.rotation"));
		}
	}
	if (command)
		control_begin_reply("ack", command);
	else
		control_begin("reloaded");
	control_field_boolean("rotation_changed", rotation_changed);
	dedicated_write_rotation();
	dedicated_write_rejected();
	dedicated_write_settings();
	control_end();
	return rotation_changed;
}

/* ---------- commands */

/* the next game changed (change_map, set_rotation, a reload): the lobby
takes it now, a game at its end */
static char const *dedicated_next_game_changed(
	struct network_game_server *server)
{
	if (dedicated.state != _dedicated_running)
	{
		dedicated_advance();
		return "next_lobby";
	}
	if (network_game_server_dedicated_in_pregame(server))
	{
		dedicated_advance();
		return "lobby";
	}
	if (!network_game_server_dedicated_in_game(server) && dedicated.postgame_advanced)
		dedicated.reapply_next = TRUE;
	return "next_game";
}

static void dedicated_end_game(
	char const *reason)
{
	platform_log("dedicated server: ending the game (%s)", reason);
	dedicated.end_reason = reason;
	game_engine_end_game();
}

static boolean dedicated_can_end_game(
	struct network_game_server *server)
{
	return dedicated.state == _dedicated_running && network_game_server_dedicated_in_game(server) &&
		game_in_progress() && game_engine && game_engine_can_score();
}

static void dedicated_command_next_map(
	struct control_command const *command,
	struct network_game_server *server)
{
	char const *effect;
	struct dedicated_rotation_entry upcoming;
	int skip = 0;

	if (!control_argument_boolean(command, "skip_postgame", &skip) && control_argument(command, "skip_postgame"))
	{
		control_error(command, "bad_request", "\"skip_postgame\" is to be true or false");
		return;
	}
	dedicated_upcoming(&upcoming);
	if (dedicated_can_end_game(server))
	{
		dedicated_end_game("next_map");
		dedicated.skip_postgame = skip != 0;
		effect = "ending_game";
	}
	else if (dedicated.state == _dedicated_running && network_game_server_dedicated_in_game(server))
	{
		/* (a game already ending, or still loading) */
		dedicated.skip_postgame = dedicated.skip_postgame || skip;
		effect = "ending_game";
	}
	else if (dedicated.state == _dedicated_running && !network_game_server_dedicated_in_pregame(server))
	{
		effect = dedicated.postgame_advanced ? "already_advancing" : "skipping_postgame";
		dedicated.skip_postgame = TRUE;
	}
	else
	{
		effect = dedicated_next_game_changed(server);
		upcoming = dedicated.current;
	}
	control_begin_reply("ack", command);
	control_field_string("effect", effect);
	dedicated_write_entry("next", &upcoming);
	control_end();
}

static void dedicated_command_end_game(
	struct control_command const *command,
	struct network_game_server *server)
{
	if (!dedicated_can_end_game(server))
	{
		control_error(command, "not_in_game", "no game is being played (the server is in the %s state)",
			dedicated_state_name());
		return;
	}
	dedicated_end_game("end_game");
	control_begin_reply("ack", command);
	control_field_string("effect", "ending_game");
	control_end();
}

/* "map" and "gametype" (and "now"), or typed "map[:gametype] [now]" */
static void dedicated_command_change_map(
	struct control_command const *command,
	struct network_game_server *server)
{
	char text[DEDICATED_MAP_PATH_LENGTH + DEDICATED_VARIANT_NAME_LENGTH + 2];
	char const *map = control_argument(command, "map");
	char const *gametype = control_argument(command, "gametype");
	struct dedicated_rotation_entry entry;
	char const *effect;
	int now = 0;

	if (!control_argument_boolean(command, "now", &now) && control_argument(command, "now"))
	{
		control_error(command, "bad_request", "\"now\" is to be true or false");
		return;
	}
	if (!map)
	{
		char words[256];
		char *space;

		snprintf(words, sizeof(words), "%s", control_text(command));
		space = strchr(words, ' ');
		if (space)
		{
			*space = 0;
			if (!strcmp(space + 1, "now"))
				now = 1;
		}
		if (!words[0])
		{
			control_error(command, "bad_request", "no \"map\"");
			return;
		}
		snprintf(text, sizeof(text), "%s", words);
	}
	else if (gametype && gametype[0])
	{
		snprintf(text, sizeof(text), "%s:%s", map, gametype);
	}
	else
	{
		snprintf(text, sizeof(text), "%s", map);
	}
	dedicated.rejected_count = 0;
	if (!dedicated_parse_entry(text, &entry))
	{
		control_error(command, "invalid_map", "\"%s\": %s", dedicated.rejected[0].text, dedicated.rejected[0].reason);
		return;
	}
	dedicated.pending = entry;
	dedicated.pending_valid = TRUE;
	effect = dedicated_next_game_changed(server);
	if (now && !strcmp(effect, "next_game") && dedicated_can_end_game(server))
	{
		dedicated_end_game("change_map");
		dedicated.skip_postgame = TRUE;
		effect = "ending_game";
	}
	platform_log("dedicated server: change_map %s (%s): %s", entry.map_path, entry.variant_name, effect);
	control_begin_reply("ack", command);
	control_field_string("effect", effect);
	dedicated_write_entry("next", &entry);
	control_end();
}

/* "rotation": text ("a:b,c") or an array of entries */
static void dedicated_command_set_rotation(
	struct control_command const *command,
	struct network_game_server *server)
{
	static struct dedicated_rotation_entry entries[MAXIMUM_ROTATION_ENTRIES];
	char const *text = control_argument(command, "rotation");
	char const *effect;
	short count;

	if (!text)
		text = control_text(command);
	if (!text[0])
	{
		control_error(command, "bad_request", "no \"rotation\"");
		return;
	}
	if (strlen(text) >= DEDICATED_ROTATION_TEXT_LENGTH)
	{
		control_error(command, "bad_request", "the rotation is longer than %d characters",
			DEDICATED_ROTATION_TEXT_LENGTH - 1);
		return;
	}
	count = dedicated_parse_rotation(text, entries);
	if (!count)
	{
		control_begin_reply("error", command);
		control_field_string("code", "invalid_rotation");
		control_field_boolean("fatal", FALSE);
		control_field_string("message", "no playable entry; the rotation stays as it was");
		dedicated_write_rejected();
		control_end();
		return;
	}
	dedicated_set_rotation(text, entries, count);
	dedicated_log_rotation();
	effect = dedicated_next_game_changed(server);
	control_begin_reply("ack", command);
	control_field_string("effect", effect);
	dedicated_write_rotation();
	dedicated_write_rejected();
	control_end();
}

/* "player" (the number the events give a player), "machine" or "name"; or
typed "kick <player>" */
static void dedicated_command_kick(
	struct control_command const *command,
	struct network_game_server *server)
{
	long player = NONE;
	long machine = NONE;
	char const *name = control_argument(command, "name");
	wchar_t machine_name[32];
	unsigned long address;
	word port;
	short index;
	short kicked_players = 0;

	if (!server || dedicated.state != _dedicated_running)
	{
		control_error(command, "not_hosting", "the server has no game (it is in the %s state)", dedicated_state_name());
		return;
	}
	if (control_argument(command, "player") && !control_argument_integer(command, "player", &player))
	{
		control_error(command, "bad_request", "\"player\" is to be a whole number");
		return;
	}
	if (control_argument(command, "machine") && !control_argument_integer(command, "machine", &machine))
	{
		control_error(command, "bad_request", "\"machine\" is to be a whole number");
		return;
	}
	if (player == NONE && machine == NONE && !name)
	{
		char *end = NULL;
		char const *text = control_text(command);

		player = text[0] ? strtol(text, &end, 10) : NONE;
		if (!text[0] || *end || player < 0)
		{
			control_error(command, "bad_request", "\"player\", \"machine\" or \"name\" is needed");
			return;
		}
	}
	dedicated_collect_players(server);
	if (player != NONE)
	{
		for (index = 0; index < dedicated_row_count && dedicated_rows[index].slot != player; index++);
		if (index == dedicated_row_count)
		{
			control_error(command, "no_such_player", "no player %ld", player);
			return;
		}
		machine = dedicated_rows[index].machine_index;
	}
	else if (name)
	{
		short matches = 0;

		for (index = 0; index < dedicated_row_count; index++)
		{
			char row_name[64];

			if (!dedicated_rows[index].connected)
				continue;
			wide_to_ascii(dedicated_rows[index].name, row_name, sizeof(row_name));
			if (!strcmp(row_name, name))
			{
				matches++;
				machine = dedicated_rows[index].machine_index;
			}
		}
		if (matches != 1)
		{
			if (matches)
				control_error(command, "ambiguous_name", "%d players are named \"%s\"", matches, name);
			else
				control_error(command, "no_such_player", "no player is named \"%s\"", name);
			return;
		}
	}
	if (machine == network_game_client_get_local_machine_index())
	{
		control_error(command, "cannot_kick_host", "machine %ld is the server's own", machine);
		return;
	}
	if (machine < 0 || machine >= DEDICATED_MACHINE_SLOTS ||
		!network_game_server_dedicated_machine(server, machine, machine_name, &address, &port))
	{
		control_error(command, "no_such_machine", "no machine %ld", machine);
		return;
	}
	for (index = 0; index < dedicated_row_count; index++)
	{
		if (dedicated_rows[index].connected && dedicated_rows[index].machine_index == machine)
			kicked_players++;
	}
	dedicated.kicked[machine] = TRUE;
	if (!network_game_server_dedicated_remove_machine(server, machine))
	{
		dedicated.kicked[machine] = FALSE;
		control_error(command, "kick_failed", "machine %ld could not be removed (debug.txt has the details)", machine);
		return;
	}
	platform_log("dedicated server: kicked machine %ld (%d players)", machine, kicked_players);
	control_begin_reply("ack", command);
	control_field_integer("machine", machine);
	control_field_utf16("machine_name", (unsigned short const *)machine_name, NUMBEROF(machine_name));
	control_field_integer("players", kicked_players);
	control_end();
	/* (its players' player_left events, reason "kicked", straight after) */
	dedicated_update_players(server);
}

static void dedicated_command_help(
	struct control_command const *command)
{
	static char const *const commands[] =
	{
		"status", "next_map", "end_game", "change_map", "set_rotation", "kick", "reload", "quit", "help",
	};
	short index;

	control_begin_reply("ack", command);
	control_field_integer("protocol", DEDICATED_CONTROL_PROTOCOL_VERSION);
	control_key("commands");
	control_array_begin();
	for (index = 0; index < NUMBEROF(commands); index++)
		control_string(commands[index]);
	control_array_end();
	control_end();
}

static void dedicated_handle_command(
	struct control_command const *command)
{
	struct network_game_server *server = global_network_game_server_get();
	char const *name = command->name;

	if (!strcmp(name, "status"))
	{
		control_begin_reply("ack", command);
		control_end();
		dedicated_emit_status(command);
	}
	else if (!strcmp(name, "next_map"))
	{
		dedicated_command_next_map(command, server);
	}
	else if (!strcmp(name, "end_game"))
	{
		dedicated_command_end_game(command, server);
	}
	else if (!strcmp(name, "change_map"))
	{
		dedicated_command_change_map(command, server);
	}
	else if (!strcmp(name, "set_rotation"))
	{
		dedicated_command_set_rotation(command, server);
	}
	else if (!strcmp(name, "kick"))
	{
		dedicated_command_kick(command, server);
	}
	else if (!strcmp(name, "reload"))
	{
		if (dedicated_reload(command))
			dedicated_next_game_changed(server);
	}
	else if (!strcmp(name, "quit"))
	{
		control_begin_reply("ack", command);
		control_end();
		dedicated.quit_requested = TRUE;
		dedicated.quit_reason = "command";
	}
	else if (!strcmp(name, "help"))
	{
		dedicated_command_help(command);
	}
	else if (!strcmp(name, "say") || !strcmp(name, "broadcast"))
	{
		control_error(command, "not_supported", "system link has no chat: no message from the host shows text on "
			"the players' machines");
	}
	else
	{
		control_error(command, "unknown_command", "no command \"%s\" (\"help\" lists them)", name);
	}
}

/* once a frame: the commands that came, the signals, the end of the input */
static void dedicated_control_update(
	void)
{
	dedicated_control_poll();
	while (dedicated_control_next_command(&dedicated_command))
		dedicated_handle_command(&dedicated_command);
	if (dedicated_control_input_ended())
	{
		control_begin("input_closed");
		control_field_boolean("exiting", dedicated.exit_on_eof);
		control_end();
		if (dedicated.exit_on_eof)
		{
			dedicated.quit_requested = TRUE;
			dedicated.quit_reason = "input_closed";
		}
	}
	if (platform_reload_requested())
	{
		platform_log("dedicated server: SIGHUP: reading the rotation and the timings again");
		if (dedicated_reload(NULL))
			dedicated_next_game_changed(global_network_game_server_get());
	}
	if (platform_status_requested())
		dedicated_emit_status(NULL);
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
	dedicated.uptime += seconds;
	dedicated_control_update();
	if (platform_quit_requested())
	{
		dedicated.quit_reason = "signal";
		dedicated_shut_down("quit signal");
	}
	if (dedicated.quit_requested)
		dedicated_shut_down(dedicated.quit_reason);

	server = global_network_game_server_get();
	/* the game lost (a network failure, an abort: back at the main menu) */
	if (dedicated.state != _dedicated_waiting && !server)
	{
		platform_log("dedicated server: the game was lost; hosting again in %.0f seconds",
			(double)dedicated.rehost_seconds);
		if (dedicated.last_phase == _phase_game)
			dedicated_emit_game_ended(NULL);
		control_error(NULL, "game_lost", "the game was lost (a network failure or an abort); hosting again in %.0f "
			"seconds", (double)dedicated.rehost_seconds);
		dedicated.state = _dedicated_waiting;
		dedicated.wait_seconds = 0.0f;
		dedicated.wait_needed = dedicated.rehost_seconds;
		dedicated.applied = FALSE;
		dedicated.last_player_count = NONE;
		dedicated.last_phase = NONE;
		dedicated.game_reported = FALSE;
		dedicated.game_start_pending = FALSE;
		dedicated.postgame_advanced = FALSE;
		dedicated.reapply_next = FALSE;
	}
	dedicated_update_players(server);

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
		dedicated.applied = FALSE;
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
			control_error(NULL, "cannot_host", "cannot host a game; trying again in %.0f seconds",
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
			dedicated_apply_to_lobby(server);
			dedicated.state = _dedicated_running;
			if (!dedicated.announced)
			{
				dedicated_emit_server_started();
				dedicated.announced = TRUE;
			}
		}
		break;

	case _dedicated_running:
	{
		short player_count = network_game_server_dedicated_player_count(server);
		short phase = network_game_server_dedicated_in_pregame(server) ? _phase_lobby :
			network_game_server_dedicated_in_game(server) ? _phase_game : _phase_postgame;

		if (phase != dedicated.last_phase)
		{
			static char const *const phase_names[] = { "lobby", "game", "scores" };

			platform_log("dedicated server: %s, %d players", phase_names[phase], player_count);
			if (dedicated.last_phase == _phase_game)
				dedicated_emit_game_ended(server);
			if (phase == _phase_game)
			{
				dedicated.games_hosted++;
				dedicated.game_entry = dedicated.current;
				dedicated.game_start_pending = TRUE;
				dedicated.game_reported = FALSE;
				dedicated.game_running_seen = FALSE;
				dedicated.game_end_reported = FALSE;
				dedicated.end_reason = NULL;
				dedicated.score_changed = FALSE;
				dedicated.score_elapsed = 0.0f;
				csmemset(dedicated.scores, 0, sizeof(dedicated.scores));
			}
			dedicated.last_phase = phase;
			dedicated.last_player_count = player_count;
			dedicated.empty_elapsed = 0.0f;
			dedicated.ended_empty_game = FALSE;
			if (phase != _phase_postgame)
			{
				dedicated.postgame_elapsed = 0.0f;
				dedicated.postgame_advanced = FALSE;
				dedicated.skip_postgame = FALSE;
			}
			if (phase == _phase_lobby)
			{
				long milliseconds;

				/* (a command changed the next game after the scores had set
				it: the lobby takes the new one) */
				if (dedicated.reapply_next)
					dedicated_advance();
				if (!dedicated.applied)
					dedicated_apply_to_lobby(server);
				dedicated.lobby_players = player_count;
				dedicated.lobby_countdown = network_game_server_dedicated_countdown(server, &milliseconds);
				dedicated_emit_lobby(server, "opened");
			}
			else if (phase == _phase_postgame)
			{
				struct dedicated_rotation_entry upcoming;

				dedicated_upcoming(&upcoming);
				control_begin("postgame");
				dedicated_write_entry_fields(&dedicated.game_entry);
				control_field_integer("postgame_seconds", (long)dedicated.postgame_seconds);
				dedicated_write_entry("next", &upcoming);
				control_end();
			}
		}
		else if (player_count != dedicated.last_player_count)
		{
			platform_log("dedicated server: %d players", player_count);
			dedicated.last_player_count = player_count;
		}

		switch (phase)
		{
		case _phase_lobby:
		{
			long milliseconds;
			boolean countdown;

			/* (after a reset the stage already had the entry: no change that
			would have every machine precache the map again) */
			if (!dedicated.applied)
				dedicated_apply_to_lobby(server);
			network_game_server_dedicated_lobby_update(server, dedicated.minimum_players, dedicated.maximum_players,
				dedicated.countdown_milliseconds);
			countdown = network_game_server_dedicated_countdown(server, &milliseconds);
			if (countdown != dedicated.lobby_countdown)
			{
				dedicated.lobby_countdown = countdown;
				dedicated.lobby_players = player_count;
				dedicated_emit_lobby(server, countdown ? "countdown_started" : "countdown_stopped");
			}
			else if (player_count != dedicated.lobby_players)
			{
				dedicated.lobby_players = player_count;
				dedicated_emit_lobby(server, "players");
			}
			break;
		}
		case _phase_game:
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
					dedicated.end_reason = "empty";
					game_engine_end_game();
					dedicated.ended_empty_game = TRUE;
				}
			}
			/* (the game is on once the host's map has loaded and it can
			score; it has ended once it no longer can) */
			if (!main_menu_loaded && dedicated_statistics_available(server))
			{
				if (game_engine_can_score())
				{
					dedicated.game_running_seen = TRUE;
					if (dedicated.game_start_pending)
					{
						dedicated.game_start_pending = FALSE;
						dedicated.game_reported = TRUE;
						control_begin("game_started");
						dedicated_write_entry_fields(&dedicated.game_entry);
						dedicated_write_variant_fields(&dedicated.game_entry, TRUE);
						dedicated_collect_players(server);
						dedicated_write_players(server, FALSE);
						control_end();
					}
					dedicated_update_scores(server, seconds);
				}
				else if (dedicated.game_running_seen)
				{
					dedicated_emit_game_ended(server);
				}
			}
			break;
		}
		break;
	}
	}

	if (dedicated.status_interval > 0.0f)
	{
		dedicated.status_elapsed += seconds;
		if (dedicated.status_elapsed >= dedicated.status_interval)
			dedicated_emit_status(NULL);
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
		if (dedicated.postgame_elapsed < dedicated.postgame_seconds && !dedicated.skip_postgame &&
			network_game_server_dedicated_player_count(server) > 0)
		{
			return FALSE;
		}
		dedicated_advance();
		dedicated_set_stage(&dedicated.current);
		/* (network_game_server_reset_to_pregame sets up the stage) */
		dedicated.applied = TRUE;
		dedicated.postgame_advanced = TRUE;
		dedicated.postgame_retry = 0.0f;
		platform_log("dedicated server: next game: %s (%s)", dedicated.current.map_path, dedicated.current.variant_name);
		return TRUE;
	}
	dedicated.postgame_retry += seconds;
	if (dedicated.postgame_retry < 1.0f)
		return FALSE;
	dedicated.postgame_retry = 0.0f;
	return TRUE;
}

/* the fatal errors of main.c (a map that does not load) */
void dedicated_server_fatal(
	char const *code,
	char const *message)
{
	control_fatal(code, "%s", message);
}
