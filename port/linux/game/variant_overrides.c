/*
VARIANT_OVERRIDES.C

The rule changes of a dedicated server's rotation entry,
"<map>:<game type>+key=value+key=value" (dedicated_server.c keeps the text
after the first "+"). Each key is a field of the game type's variant
(game_engine.h); values are whole numbers, or true/false (1/0) for the
switches. Times are in seconds (the variant's are ticks), health is a
percentage (25 to 400, the variant clamps it to 0.25 to 4.0). A key another
game type's variant has (flag_time in slayer) is refused, as is an unknown
key or a value out of range, so a bad rotation is reported rather than
played wrongly.

universal keys: score, lives, respawn, respawn_growth, suicide_penalty,
	health, weapons (0-10), vehicles (0-4), goal_radar (0-2), odd_man_out,
	radar, friend_indicators, infinite_grenades, no_shields, invisible,
	generic_equipment
ctf: assault, flag_must_reset, flag_at_home, flag_time
slayer: death_bonus, kill_penalty, kill_in_order
king: moving_hill
oddball: random_start, ball_speed (0-2), trait_with_ball (0-3),
	trait_without_ball (0-3), ball_type (0-2), balls (1-16)
race: race_type (0-2), team_scoring (0-2)
*/

#include "cseries.h"
#include "game/game_engine.h"
#include "variant_overrides.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	_override_engine_any = 0,
	_override_engine_ctf = 1,
	_override_engine_slayer = 2,
	_override_engine_oddball = 3,
	_override_engine_king = 4,
	_override_engine_race = 5,

	_override_integer = 0,
	_override_boolean,
	_override_seconds,
	_override_percent,
	_override_flag,
	_override_inverted_boolean,

	TICKS_PER_OVERRIDE_SECOND = 30,
};

struct variant_override_key
{
	char const *name;
	long engine;
	long kind;
	long minimum;
	long maximum;
	/* the field's offset in the variant; for a flag, the bit */
	unsigned long offset;
};

#define UNIVERSAL(field) (offsetof(struct game_variant, universal_variant) + offsetof(struct universal_variant, field))
#define ENGINE(type, field) (offsetof(struct game_variant, game_engine_variant) + offsetof(struct type, field))

static struct variant_override_key const variant_override_keys[] =
{
	{ "score", _override_engine_any, _override_integer, 1, 999, UNIVERSAL(score_to_win) },
	{ "lives", _override_engine_any, _override_integer, 0, 99, UNIVERSAL(lives) },
	{ "respawn", _override_engine_any, _override_seconds, 0, 300, UNIVERSAL(respawn_time) },
	{ "respawn_growth", _override_engine_any, _override_seconds, 0, 60, UNIVERSAL(respawn_time_growth) },
	{ "suicide_penalty", _override_engine_any, _override_seconds, 0, 60, UNIVERSAL(suicide_penalty) },
	{ "health", _override_engine_any, _override_percent, 25, 400, UNIVERSAL(health) },
	{ "weapons", _override_engine_any, _override_integer, 0, 10, UNIVERSAL(weapon_set) },
	{ "vehicles", _override_engine_any, _override_integer, 0, 4, UNIVERSAL(vehicle_set) },
	{ "goal_radar", _override_engine_any, _override_integer, 0, 2, UNIVERSAL(goal_radar) },
	{ "odd_man_out", _override_engine_any, _override_boolean, 0, 1, UNIVERSAL(odd_man_out) },
	{ "radar", _override_engine_any, _override_flag, 0, 1, 0 },
	{ "friend_indicators", _override_engine_any, _override_flag, 0, 1, 1 },
	{ "infinite_grenades", _override_engine_any, _override_flag, 0, 1, 2 },
	{ "no_shields", _override_engine_any, _override_flag, 0, 1, 3 },
	{ "invisible", _override_engine_any, _override_flag, 0, 1, 4 },
	{ "generic_equipment", _override_engine_any, _override_flag, 0, 1, 5 },

	{ "assault", _override_engine_ctf, _override_boolean, 0, 1, ENGINE(ctf_variant, assault) },
	{ "flag_must_reset", _override_engine_ctf, _override_boolean, 0, 1, ENGINE(ctf_variant, flag_must_reset) },
	{ "flag_at_home", _override_engine_ctf, _override_boolean, 0, 1, ENGINE(ctf_variant, flag_at_home_to_score) },
	{ "flag_time", _override_engine_ctf, _override_seconds, 0, 600, ENGINE(ctf_variant, single_flag_time) },

	{ "death_bonus", _override_engine_slayer, _override_inverted_boolean, 0, 1, ENGINE(slayer_variant, no_death_bonus) },
	{ "kill_penalty", _override_engine_slayer, _override_inverted_boolean, 0, 1, ENGINE(slayer_variant, no_kill_penalty) },
	{ "kill_in_order", _override_engine_slayer, _override_boolean, 0, 1, ENGINE(slayer_variant, kill_in_order) },

	{ "moving_hill", _override_engine_king, _override_boolean, 0, 1, ENGINE(king_variant, moving_hill) },

	{ "random_start", _override_engine_oddball, _override_boolean, 0, 1, ENGINE(oddball_variant, random_start) },
	{ "ball_speed", _override_engine_oddball, _override_integer, 0, 2, ENGINE(oddball_variant, speed_with_ball) },
	{ "trait_with_ball", _override_engine_oddball, _override_integer, 0, 3, ENGINE(oddball_variant, trait_with_ball) },
	{ "trait_without_ball", _override_engine_oddball, _override_integer, 0, 3, ENGINE(oddball_variant, trait_without_ball) },
	{ "ball_type", _override_engine_oddball, _override_integer, 0, 2, ENGINE(oddball_variant, oddball_ball_type) },
	{ "balls", _override_engine_oddball, _override_integer, 1, 16, ENGINE(oddball_variant, ball_spawn_count) },

	{ "race_type", _override_engine_race, _override_integer, 0, 2, ENGINE(race_variant, race_type) },
	{ "team_scoring", _override_engine_race, _override_integer, 0, 2, ENGINE(race_variant, team_scoring) },
};

static struct variant_override_key const *variant_override_find(
	char const *name,
	size_t length)
{
	size_t index;

	for (index = 0; index < NUMBEROF(variant_override_keys); index++)
	{
		if (strlen(variant_override_keys[index].name) == length &&
			!strncmp(variant_override_keys[index].name, name, length))
			return &variant_override_keys[index];
	}
	return NULL;
}

static boolean variant_override_value(
	char const *text,
	size_t length,
	long *value)
{
	char copy[16];
	char *end;

	if (!length || length >= sizeof(copy))
		return FALSE;
	memcpy(copy, text, length);
	copy[length] = 0;
	if (!strcmp(copy, "true") || !strcmp(copy, "on") || !strcmp(copy, "yes"))
	{
		*value = 1;
		return TRUE;
	}
	if (!strcmp(copy, "false") || !strcmp(copy, "off") || !strcmp(copy, "no"))
	{
		*value = 0;
		return TRUE;
	}
	*value = strtol(copy, &end, 10);
	return *end == 0;
}

/* one "key=value" of the text; walks *cursor past it and its "+" */
static boolean variant_override_next(
	char const **cursor,
	struct variant_override_key const **key,
	long *value,
	long engine,
	char *reason,
	unsigned long reason_size)
{
	char const *start = *cursor;
	char const *end = strchr(start, '+');
	char const *equals;
	size_t length;

	if (!end)
		end = start + strlen(start);
	length = (size_t)(end - start);
	*cursor = *end ? end + 1 : end;
	equals = memchr(start, '=', length);
	if (!equals)
	{
		snprintf(reason, reason_size, "rule \"%.*s\" has no value", (int)length, start);
		return FALSE;
	}
	*key = variant_override_find(start, (size_t)(equals - start));
	if (!*key)
	{
		snprintf(reason, reason_size, "no rule \"%.*s\"", (int)(equals - start), start);
		return FALSE;
	}
	if ((*key)->engine != _override_engine_any && (*key)->engine != engine)
	{
		snprintf(reason, reason_size, "rule \"%s\" is not this game type's", (*key)->name);
		return FALSE;
	}
	if (!variant_override_value(equals + 1, (size_t)(end - equals - 1), value) ||
		*value < (*key)->minimum || *value > (*key)->maximum)
	{
		snprintf(reason, reason_size, "rule \"%s\" takes %ld to %ld", (*key)->name, (*key)->minimum, (*key)->maximum);
		return FALSE;
	}
	return TRUE;
}

boolean variant_overrides_validate(
	struct game_variant const *variant,
	char const *text,
	char *reason,
	unsigned long reason_size)
{
	char const *cursor = text;

	if (!text[0])
	{
		snprintf(reason, reason_size, "no rules after \"+\"");
		return FALSE;
	}
	if (text[0] == '+' || text[strlen(text) - 1] == '+' || strstr(text, "++"))
	{
		snprintf(reason, reason_size, "an empty rule between \"+\"s");
		return FALSE;
	}
	while (*cursor)
	{
		struct variant_override_key const *key;
		long value;

		if (!variant_override_next(&cursor, &key, &value, variant->game_engine_index, reason, reason_size))
			return FALSE;
	}
	return TRUE;
}

boolean variant_overrides_apply(
	struct game_variant *variant,
	char const *text)
{
	char const *cursor = text;
	char reason[96];

	while (*cursor)
	{
		struct variant_override_key const *key;
		long value;
		byte *field;

		if (!variant_override_next(&cursor, &key, &value, variant->game_engine_index, reason, sizeof(reason)))
			return FALSE;
		field = (byte *)variant + key->offset;
		switch (key->kind)
		{
		case _override_integer:
			*(long *)field = value;
			break;
		case _override_seconds:
			*(long *)field = value * TICKS_PER_OVERRIDE_SECOND;
			break;
		case _override_percent:
			*(real *)field = (real)value / 100.0f;
			break;
		case _override_boolean:
			*(boolean *)field = (boolean)(value != 0);
			break;
		case _override_inverted_boolean:
			*(boolean *)field = (boolean)(value == 0);
			break;
		case _override_flag:
			if (value)
				variant->universal_variant.flags |= 1UL << key->offset;
			else
				variant->universal_variant.flags &= ~(1UL << key->offset);
			break;
		}
	}
	return TRUE;
}
