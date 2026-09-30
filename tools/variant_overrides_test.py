#!/usr/bin/env python3
"""
VARIANT_OVERRIDES_TEST.PY

Tests port/linux/game/variant_overrides.c, the rule changes of a dedicated
server's rotation entry ("<map>:<game type>+key=value+key=value"), without
game data: the file and a small driver are built with the Linux build's own
compile command (from build.ninja, so configure.py must have run) and linked
on their own, and the driver checks validation (unknown keys, another game
type's keys, ranges, missing values) and what apply writes into the variant
(seconds to ticks, health percentages, the universal flags, the inverted
slayer switches).

Usage: tools/variant_overrides_test.py
"""

import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJECT = "build/linux/obj/port/linux/game/variant_overrides.o"
SOURCE = "port/linux/game/variant_overrides.c"

DRIVER = r"""
#include "cseries.h"
#include "game/game_engine.h"
#include "../port/linux/game/variant_overrides.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { printf("FAIL %d: %s\n", __LINE__, #condition); failures++; } } while (0)

static void make(struct game_variant *variant, long engine)
{
	memset(variant, 0, sizeof(*variant));
	variant->game_engine_index = engine;
	variant->universal_variant.health = 1.0f;
}

static int valid(long engine, char const *text, char const *expected_reason)
{
	struct game_variant variant;
	char reason[96] = "";
	int result;

	make(&variant, engine);
	result = variant_overrides_validate(&variant, text, reason, sizeof(reason));
	if (expected_reason && !strstr(reason, expected_reason))
	{
		printf("FAIL: \"%s\": reason \"%s\", expected \"%s\"\n", text, reason, expected_reason);
		failures++;
	}
	return result;
}

int main(void)
{
	struct game_variant variant;

	CHECK(valid(2, "score=25", NULL));
	CHECK(valid(2, "score=25+lives=3+respawn=10+health=150+no_shields=1+weapons=4", NULL));
	CHECK(valid(1, "flag_time=60+assault=true+flag_at_home=off", NULL));
	CHECK(valid(3, "balls=4+ball_type=2+ball_speed=2+trait_with_ball=1", NULL));
	CHECK(valid(4, "moving_hill=yes", NULL));
	CHECK(valid(5, "race_type=1+team_scoring=2", NULL));
	CHECK(!valid(2, "", "no rules"));
	CHECK(!valid(2, "nope=1", "no rule \"nope\""));
	CHECK(!valid(2, "flag_time=10", "not this game type's"));
	CHECK(!valid(1, "moving_hill=1", "not this game type's"));
	CHECK(!valid(2, "score", "has no value"));
	CHECK(!valid(2, "score=", "takes 1 to 999"));
	CHECK(!valid(2, "score=0", "takes 1 to 999"));
	CHECK(!valid(2, "score=1000", "takes 1 to 999"));
	CHECK(!valid(2, "health=10", "takes 25 to 400"));
	CHECK(!valid(2, "weapons=11", "takes 0 to 10"));
	CHECK(!valid(2, "score=5x", "takes"));
	CHECK(!valid(2, "score=5+", "empty rule"));
	CHECK(!valid(2, "score=5++lives=1", "empty rule"));
	CHECK(!valid(2, "+score=5", "empty rule"));
	CHECK(!valid(2, "score=99999999999999999999", "takes"));

	make(&variant, 2);
	variant.universal_variant.flags = 1; /* radar on */
	variant.game_engine_variant.slayer.no_death_bonus = 0;
	CHECK(variant_overrides_apply(&variant,
		"score=7+lives=2+respawn=3+respawn_growth=1+suicide_penalty=5+health=250+vehicles=2+goal_radar=1"
		"+radar=0+no_shields=1+invisible=true+odd_man_out=1+death_bonus=false+kill_penalty=true+kill_in_order=1"));
	CHECK(variant.universal_variant.score_to_win == 7);
	CHECK(variant.universal_variant.lives == 2);
	CHECK(variant.universal_variant.respawn_time == 90);
	CHECK(variant.universal_variant.respawn_time_growth == 30);
	CHECK(variant.universal_variant.suicide_penalty == 150);
	CHECK(variant.universal_variant.health > 2.49f && variant.universal_variant.health < 2.51f);
	CHECK(variant.universal_variant.vehicle_set == 2);
	CHECK(variant.universal_variant.goal_radar == 1);
	CHECK(variant.universal_variant.odd_man_out == 1);
	CHECK(variant.universal_variant.flags == ((1 << 3) | (1 << 4)));
	CHECK(variant.game_engine_variant.slayer.no_death_bonus == 1);
	CHECK(variant.game_engine_variant.slayer.no_kill_penalty == 0);
	CHECK(variant.game_engine_variant.slayer.kill_in_order == 1);

	make(&variant, 1);
	CHECK(variant_overrides_apply(&variant, "flag_time=45+flag_must_reset=1+assault=1"));
	CHECK(variant.game_engine_variant.ctf.single_flag_time == 45 * 30);
	CHECK(variant.game_engine_variant.ctf.flag_must_reset == 1);
	CHECK(variant.game_engine_variant.ctf.assault == 1);

	make(&variant, 3);
	CHECK(variant_overrides_apply(&variant, "balls=3+ball_type=1+trait_without_ball=2"));
	CHECK(variant.game_engine_variant.oddball.ball_spawn_count == 3);
	CHECK(variant.game_engine_variant.oddball.oddball_ball_type == 1);
	CHECK(variant.game_engine_variant.oddball.trait_without_ball == 2);

	make(&variant, 2);
	CHECK(!variant_overrides_apply(&variant, "flag_time=10"));

	printf(failures ? "variant overrides: %d failed\n" : "variant overrides: ok\n", failures);
	return failures != 0;
}
"""


# the game's library functions the two files call, over the C library's
SHIMS = r"""
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void *csmemset(void *buffer, long c, unsigned long size) { return memset(buffer, (int)c, size); }
void *csmemcpy(void *destination, const void *source, unsigned long size) { return memcpy(destination, source, size); }
long csstrcmp(const char *a, const char *b) { return strcmp(a, b); }
long csstrncmp(const char *a, const char *b, unsigned long size) { return strncmp(a, b, size); }
unsigned long csstrlen(const char *s) { return strlen(s); }
int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}
int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vprintf(format, arguments);
	va_end(arguments);
	return result;
}
"""


def compile_command():
	commands = subprocess.run(["ninja", "-t", "commands", OBJECT], cwd=ROOT, check=True,
		capture_output=True, text=True).stdout.strip().splitlines()
	return shlex.split(commands[-1])


# configure.py --compiler-launcher (CI: ccache) puts the launcher before the
# compiler in build.ninja's commands; the test calls the compiler itself
LAUNCHERS = ("ccache", "sccache", "distcc")


def main():
	command = compile_command()
	if os.path.basename(command[0]) in LAUNCHERS:
		command = command[1:]
	compiler = command[0]
	with tempfile.TemporaryDirectory() as directory:
		driver = os.path.join(directory, "driver.c")
		with open(driver, "w") as handle:
			handle.write(DRIVER)
		objects = []
		for source in (SOURCE, driver):
			output = os.path.join(directory, os.path.basename(source) + ".o")
			arguments = []
			skip = False
			for argument in command[1:]:
				if skip:
					skip = False
					continue
				if argument in ("-o", "-MF", "-MT", "-MQ"):
					skip = True
					continue
				if argument in ("-MD", "-MMD") or argument.endswith(".c"):
					continue
				arguments.append(argument)
			subprocess.run([compiler] + arguments + ["-Wno-error", "-c", source, "-o", output], cwd=ROOT, check=True)
			objects.append(output)
		shims = os.path.join(directory, "shims.c")
		with open(shims, "w") as handle:
			handle.write(SHIMS)
		objects.append(shims + ".o")
		subprocess.run([compiler, "-m32", "-c", shims, "-o", shims + ".o"], check=True)
		program = os.path.join(directory, "test")
		subprocess.run([compiler, "-m32", "-no-pie", "-fuse-ld=lld"] + objects + ["-o", program], cwd=ROOT, check=True)
		result = subprocess.run([program])
	return result.returncode


if __name__ == "__main__":
	sys.exit(main())
