/* tools/test_render_between_ticks.py: the real zbias_slope (d3d8_gl.c),
extracted into under_test.inc. The slope of a z bias grows with the
target's scale. The depth units are not this function. */

#include <stdio.h>
#include <string.h>

static float target_scale[2];

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

static int near(float a, float b)
{
	float d = a - b;
	return d < 0.0001f && -d < 0.0001f;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	if (!strcmp(case_name, "xbox-size"))
	{
		target_scale[0] = 1.0f;
		target_scale[1] = 1.0f;
		CHECK(near(zbias_slope(-2.0f), -2.0f), "at the Xbox's size the slope changed");
		return 0;
	}
	if (!strcmp(case_name, "scaled"))
	{
		target_scale[0] = 2.25f;
		target_scale[1] = 2.25f;
		CHECK(near(zbias_slope(-2.0f), -4.5f), "the slope was not 2.25 times the Xbox's");
		return 0;
	}
	if (!strcmp(case_name, "wider-axis"))
	{
		target_scale[0] = 3.0f;
		target_scale[1] = 2.0f;
		CHECK(near(zbias_slope(-2.0f), -6.0f), "the slope used the smaller axis");
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
