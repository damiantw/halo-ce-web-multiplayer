/* tools/test_render_between_ticks.py: the real contrail_point_dt
(effects/contrails.c), extracted into under_test.inc. Points are made and
aged by the ticks' own time. Adapted from the cases of upstream
OpenCommunityEdition/OpenCE PR #546; this fork has no tools/harness. */

#include <stdio.h>
#include <string.h>

typedef float real;
typedef int boolean;
enum { NONE = -1, TICKS_PER_SECOND = 30 };

static long now;
static int fast;

static long game_time_get(void) { return now; }
static boolean game_players_are_double_speed(void) { return fast; }

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

static int near(real a, real b)
{
	real d = a - b;
	return d < 0.00001f && -d < 0.00001f;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	now = 10;
	CHECK(near(contrail_point_dt(), 1.0f / 30.0f), "the first tick was %g", contrail_point_dt());
	/* the call above consumed the tick; ask again for the value just returned */
	if (!strcmp(case_name, "one-tick"))
	{
		now = 11;
		CHECK(near(contrail_point_dt(), 1.0f / 30.0f), "one tick was not 1/30");
		return 0;
	}
	if (!strcmp(case_name, "same-tick"))
	{
		CHECK(contrail_point_dt() == 0.0f, "a frame of the same tick aged the points");
		return 0;
	}
	if (!strcmp(case_name, "two-ticks"))
	{
		now = 12;
		CHECK(near(contrail_point_dt(), 2.0f / 30.0f), "two ticks were not 2/30");
		return 0;
	}
	if (!strcmp(case_name, "jump"))
	{
		now = 10 + 1000;
		CHECK(near(contrail_point_dt(), 1.0f / 30.0f), "a jump was not one tick");
		return 0;
	}
	if (!strcmp(case_name, "backwards"))
	{
		now = 9;
		CHECK(near(contrail_point_dt(), 1.0f / 30.0f), "time gone back was not one tick");
		return 0;
	}
	if (!strcmp(case_name, "double-speed"))
	{
		fast = 1;
		now = 11;
		CHECK(near(contrail_point_dt(), 0.5f / 30.0f), "double speed was not half a tick");
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
