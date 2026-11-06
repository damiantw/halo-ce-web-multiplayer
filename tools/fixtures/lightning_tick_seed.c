/* tools/test_render_between_ticks.py: the real lightning_tick_seed
(objects/widgets/lightning.c), extracted into under_test.inc. A bolt keeps
its shape through the frames of a tick. Adapted from the cases of upstream
OpenCommunityEdition/OpenCE PR #552; this fork has no tools/harness. */

#include <stdio.h>
#include <string.h>

typedef int boolean;
#define TRUE 1
#define FALSE 0

enum { LIGHTNING_TICK_SEED_LIGHTNINGS = 256, LIGHTNING_TICK_SEED_WINDOWS = 4 };
static struct
{
	long tick;
	unsigned long seed;
} lightning_tick_seeds[LIGHTNING_TICK_SEED_LIGHTNINGS][LIGHTNING_TICK_SEED_WINDOWS];

static long now;
static long game_time_get(void) { return now; }
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(datum_index) ((datum_index) & 0xFFFF)

#include "under_test.inc"

#define CHECK(condition, ...) \
	do \
	{ \
		if (!(condition)) \
		{ \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
			return 1; \
		} \
	} while (0)

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	unsigned long seed;
	boolean drawn;

	now = 10;
	seed = 1000;
	drawn = lightning_tick_seed(3, 0, &seed);
	CHECK(!drawn && seed == 1000, "the first frame of a tick changed the seed to %lu", seed);
	CHECK(lightning_tick_seeds[3][0].seed == 1000, "the tick's seed was not kept");

	if (!strcmp(case_name, "same-tick"))
	{
		seed = 5555;
		drawn = lightning_tick_seed(3, 0, &seed);
		CHECK(drawn && seed == 1000, "a later frame drew seed %lu, not the tick's 1000", seed);
		seed = 7777;
		drawn = lightning_tick_seed(3, 1, &seed);
		CHECK(!drawn && seed == 7777, "another window reused window 0's bolt");
		return 0;
	}
	if (!strcmp(case_name, "next-tick"))
	{
		now = 11;
		seed = 2222;
		drawn = lightning_tick_seed(3, 0, &seed);
		CHECK(!drawn && seed == 2222, "the next tick kept the old shape (%lu)", seed);
		return 0;
	}
	if (!strcmp(case_name, "out-of-range"))
	{
		seed = 9;
		drawn = lightning_tick_seed(3, 4, &seed);
		CHECK(!drawn && seed == 9, "a window this does not keep changed the seed");
		seed = 9;
		drawn = lightning_tick_seed(256, 0, &seed);
		CHECK(!drawn && seed == 9, "a lightning this does not keep changed the seed");
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
