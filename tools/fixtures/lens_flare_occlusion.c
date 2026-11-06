/* tools/test_render_between_ticks.py: the real lens_flare_occlusion_test_index
and lens_flare_occlusion_visible_pixels (rasterizer_lights.c), extracted into
under_test.inc. The slot table's type comes from the source (config.inc).
Adapted from the cases of upstream OpenCommunityEdition/OpenCE PR #541; this
fork has no tools/harness, and the GPU numbering is stubbed. */

#include <stdio.h>
#include <string.h>

typedef unsigned char byte;
typedef int boolean;
enum { NONE = -1, TRUE = 1, FALSE = 0 };

#include "config.inc"

static struct lens_flare_occlusion_slot lens_flare_occlusion_slots[LENS_FLARE_OCCLUSION_SLOT_COUNT];
static short local_lens_flare_occlusion_tests[8];
static unsigned long lens_flare_occlusion_frame;

struct rasterizer_lens_flare_submit_parameters { short light_identifier; };
static struct rasterizer_lens_flare_submit_parameters params;
static byte visibility_storage[4];
static byte const *visibility_result = visibility_storage;
static struct { int lens_flares_occlusion; } rasterizer_debug_options = { 1 };
static long widget_pixels;
static unsigned long long tests_ended;
static unsigned long long tests_counted;

static struct rasterizer_lens_flare_submit_parameters *lens_flare_parameters_get(short lens_flare_index)
{
	(void)lens_flare_index;
	return &params;
}
static byte *lens_flare_occlusion_test_results_get(struct rasterizer_lens_flare_submit_parameters *lens_flare_parameters)
{
	(void)lens_flare_parameters;
	return (byte *)visibility_result;
}
static long rasterizer_widget_get_occlusion_test_result(long index)
{
	(void)index;
	return widget_pixels;
}
static unsigned long long xgpu_visibility_tests_ended(unsigned long index)
{
	(void)index;
	return tests_ended;
}
static unsigned long long xgpu_visibility_test_counted(unsigned long index)
{
	(void)index;
	return tests_counted;
}

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

static void occupy_every_slot_but_zero(void)
{
	int slot;

	for (slot = 1; slot < LENS_FLARE_OCCLUSION_SLOT_COUNT; slot++)
	{
		lens_flare_occlusion_slots[slot].visibility = visibility_storage + 1;
		lens_flare_occlusion_slots[slot].light_identifier = 1;
		lens_flare_occlusion_slots[slot].tested_frame = lens_flare_occlusion_frame;
	}
}

static int probe_hits_zero(byte const *visibility, short identifier)
{
	unsigned long hash = ((unsigned long)visibility ^ (unsigned short)identifier) * 2654435761UL;
	int probe;

	for (probe = 0; probe < LENS_FLARE_OCCLUSION_SLOT_PROBES; probe++)
	{
		if ((short)(((hash >> 16) + probe) & (LENS_FLARE_OCCLUSION_SLOT_COUNT - 1)) == 0)
			return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	short slot;
	short again;
	int identifier;

	params.light_identifier = 7;
	tests_ended = 5;
	tests_counted = 5;
	widget_pixels = 0;

	if (!strcmp(case_name, "same-slot"))
	{
		slot = lens_flare_occlusion_test_index(0);
		again = lens_flare_occlusion_test_index(0);
		CHECK(slot == again && slot != 0 && slot < LENS_FLARE_OCCLUSION_SLOT_COUNT,
			"the flare's slot changed, or was %d", slot);
		CHECK(lens_flare_occlusion_slots[0].visibility == 0, "slot 0 was used");
		return 0;
	}
	if (!strcmp(case_name, "not-until-counted"))
	{
		slot = lens_flare_occlusion_test_index(0);
		local_lens_flare_occlusion_tests[0] = slot;
		CHECK(lens_flare_occlusion_visible_pixels(0) == NONE,
			"a slot just taken was read as %ld", lens_flare_occlusion_visible_pixels(0));
		tests_counted = lens_flare_occlusion_slots[slot].first_test;
		widget_pixels = 40;
		CHECK(lens_flare_occlusion_visible_pixels(0) == 40, "the flare's own count was not read");
		return 0;
	}
	if (!strcmp(case_name, "no-slot"))
	{
		visibility_result = visibility_storage;
		for (identifier = 0; identifier < 4096; identifier++)
		{
			if (probe_hits_zero(visibility_result, (short)identifier))
				break;
		}
		CHECK(identifier < 4096, "no identifier probed slot 0");
		occupy_every_slot_but_zero();
		params.light_identifier = (short)identifier;
		slot = lens_flare_occlusion_test_index(0);
		CHECK(slot == LENS_FLARE_OCCLUSION_UNREAD_INDEX, "a flare with no slot was tested at %d", slot);
		local_lens_flare_occlusion_tests[0] = slot;
		widget_pixels = 64;
		CHECK(lens_flare_occlusion_visible_pixels(0) == NONE, "an unread test was read");
		CHECK(lens_flare_occlusion_slots[0].visibility == 0, "slot 0 was used");
		return 0;
	}
	if (!strcmp(case_name, "occlusion-off"))
	{
		slot = lens_flare_occlusion_test_index(0);
		local_lens_flare_occlusion_tests[0] = slot;
		rasterizer_debug_options.lens_flares_occlusion = 0;
		widget_pixels = 1;
		CHECK(lens_flare_occlusion_visible_pixels(0) == 1, "with the tests off the flare was not read as 1");
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
