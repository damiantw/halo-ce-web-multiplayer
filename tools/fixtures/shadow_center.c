/* tools/test_render_between_ticks.py: the real
render_interpolation_object_point (render_interpolation.c), extracted into
under_test.inc. A point on the object moves with the drawn root. Adapted
from upstream OpenCommunityEdition/OpenCE PR #542; this fork has no
tools/harness. */

#include <stdio.h>
#include <string.h>

typedef float real;
typedef int boolean;
enum { NONE = -1, TRUE = 1, FALSE = 0 };

typedef struct { real x, y, z; } real_point3d;
typedef struct { real i, j, k; } real_vector3d;
typedef struct
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
} real_matrix4x3;

struct object_datum
{
	struct
	{
		struct { short size; short offset; } node_matrices;
	} object;
};

static boolean interpolation_rendering;
static struct object_datum object_data;
static real_matrix4x3 drawn_root;
static real_matrix4x3 simulated_root;
static int object_alive;

#define object_try_and_get(index) ((index) == 7 && object_alive ? &object_data : 0)
#define object_header_block_get(index, reference) ((void *)&simulated_root)

static real_matrix4x3 *render_interpolation_object_node_matrices(long object_index)
{
	(void)object_index;
	return interpolation_rendering ? &drawn_root : 0;
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

static int near(real a, real b)
{
	real d = a - b;
	return d < 0.0001f && -d < 0.0001f;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	real_point3d center;

	object_alive = 1;
	object_data.object.node_matrices.size = (short)sizeof(real_matrix4x3);
	simulated_root.position.x = 10.0f;
	simulated_root.position.y = 20.0f;
	simulated_root.position.z = 3.0f;
	drawn_root.position.x = 9.5f;
	drawn_root.position.y = 20.25f;
	drawn_root.position.z = 3.0f;
	center.x = 10.0f;
	center.y = 20.0f;
	center.z = 3.0f;

	if (!strcmp(case_name, "centered-where-drawn"))
	{
		interpolation_rendering = TRUE;
		render_interpolation_object_point(7, &center);
		CHECK(near(center.x, 9.5f) && near(center.y, 20.25f) && near(center.z, 3.0f),
			"the center is %g %g %g, not where the object is drawn", center.x, center.y, center.z);
		return 0;
	}
	if (!strcmp(case_name, "at-the-tick"))
	{
		interpolation_rendering = FALSE;
		render_interpolation_object_point(7, &center);
		CHECK(near(center.x, 10.0f) && near(center.y, 20.0f) && near(center.z, 3.0f),
			"with interpolation off the center moved to %g %g %g", center.x, center.y, center.z);
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
