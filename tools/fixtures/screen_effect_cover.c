/* tools/test_render_between_ticks.py: the real
rasterizer_screen_effect_cover_target (rasterizer_xbox_screen_effect.c),
extracted into under_test.inc. A pass's quad, once the shader translator has
moved it half a game pixel right and down, covers the whole viewport, and
each place on it samples where it did before. Adapted from upstream
OpenCommunityEdition/OpenCE PR #536 (tools/harness/tests/screen_effect_cover.c);
this fork has no tools/harness. */

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef float real;
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;

#include "config.inc"

static long screen_width = 854;
static long halo_screen_width(void) { return screen_width; }

#include "under_test.inc"

enum { SCREEN_HEIGHT = 480 };

struct viewport { real x, y, width, height; };

static real screen_x(real ndc, struct viewport const *viewport)
{
	return viewport->x + (ndc + 1.0f) * 0.5f * viewport->width + 0.5f;
}
static real screen_y(real ndc, struct viewport const *viewport)
{
	return viewport->y + (1.0f - ndc) * 0.5f * viewport->height + 0.5f;
}

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

static int checked_quad(
	short destination,
	short viewport_width,
	short viewport_height,
	real_rectangle2d stock,
	struct viewport const *viewport,
	real left,
	real top,
	real right,
	real bottom)
{
	real_rectangle2d bounds = stock;
	real shift = rasterizer_screen_effect_cover_target(destination, viewport_width, viewport_height, &bounds);
	real x0 = screen_x(bounds.x0, viewport), x1 = screen_x(bounds.x1, viewport);
	real y0 = screen_y(bounds.y0, viewport), y1 = screen_y(bounds.y1, viewport);
	real stock_x0 = screen_x(stock.x0, viewport), stock_x1 = screen_x(stock.x1, viewport);
	real stock_y0 = screen_y(stock.y0, viewport), stock_y1 = screen_y(stock.y1, viewport);
	int step;

	CHECK(fabsf(x0 - left) < 0.001f && fabsf(x1 - right) < 0.001f,
		"the quad covers columns %g to %g, not %g to %g", x0, x1, left, right);
	CHECK(fabsf(y0 - top) < 0.001f && fabsf(y1 - bottom) < 0.001f,
		"the quad covers lines %g to %g, not %g to %g", y0, y1, top, bottom);
	for (step = 0; step <= 8; step++)
	{
		real x = left + (right - left) * (real)step / 8.0f;
		real y = top + (bottom - top) * (real)step / 8.0f;
		real u = -shift + (x - x0) / (x1 - x0) * (real)viewport_width;
		real v = -shift + (y - y0) / (y1 - y0) * (real)viewport_height;
		real stock_u = (x - stock_x0) / (stock_x1 - stock_x0) * (real)viewport_width;
		real stock_v = (y - stock_y0) / (stock_y1 - stock_y0) * (real)viewport_height;

		CHECK(fabsf(u - stock_u) < 0.001f, "column %g samples %g, not the stock quad's %g", x, u, stock_u);
		CHECK(fabsf(v - stock_v) < 0.001f, "line %g samples %g, not the stock quad's %g", y, v, stock_v);
	}
	return 0;
}

static real_rectangle2d whole_viewport(void)
{
	real_rectangle2d bounds;

	bounds.x0 = bounds.y1 = -1.0f;
	bounds.x1 = bounds.y0 = 1.0f;
	return bounds;
}

static real_rectangle2d window_in_surface(short x0, short y0, short x1, short y1)
{
	real_rectangle2d bounds;

	bounds.x0 = 2 * x0 * (1.0f / (real)halo_screen_width()) - 1.0f;
	bounds.x1 = 2 * x1 * (1.0f / (real)halo_screen_width()) - 1.0f;
	bounds.y0 = -2 * y0 * (1.0f / 480.0f) + 1.0f;
	bounds.y1 = -2 * y1 * (1.0f / 480.0f) + 1.0f;
	return bounds;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	int status;

	if (!strcmp(case_name, "covers-the-screen"))
	{
		struct viewport screen = { 0.0f, 0.0f, 854.0f, 480.0f };

		status = checked_quad(_rasterizer_target_screen_effect, 854, 480, whole_viewport(), &screen, 0, 0, 854, 480);
		if (status)
			return status;
		return checked_quad(0, 854, 480, whole_viewport(), &screen, 0, 0, 854, 480);
	}
	if (!strcmp(case_name, "covers-at-the-xbox-size"))
	{
		struct viewport screen = { 0.0f, 0.0f, 640.0f, 480.0f };

		screen_width = 640;
		status = checked_quad(_rasterizer_target_screen_effect, 640, 480, whole_viewport(), &screen, 0, 0, 640, 480);
		if (status)
			return status;
		return checked_quad(0, 640, 480, whole_viewport(), &screen, 0, 0, 640, 480);
	}
	if (!strcmp(case_name, "split-screen"))
	{
		struct viewport surface = { 0.0f, 0.0f, 854.0f, 480.0f };
		struct viewport window = { 427.0f, 240.0f, 427.0f, 240.0f };

		status = checked_quad(_rasterizer_target_screen_effect, 427, 240, window_in_surface(427, 240, 854, 480),
			&surface, 427, 240, 854, 480);
		if (status)
			return status;
		return checked_quad(0, 427, 240, whole_viewport(), &window, 427, 240, 854, 480);
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
