/*
GL_FUNCTIONS.C

Run-time resolution of the OpenGL entry points listed in gl.h.
*/

#include "platform.h"
#define GL_FUNCTIONS_DEFINE
#include "gl.h"

#include <SDL3/SDL.h>
#include <string.h>

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DEFINE_FUNCTION)

int gl_functions_load(void)
{
	int success = TRUE;

#ifdef HALO_WEB
/* OpenGL ES 3.2 functions WebGL 2 lacks; d3d8_gl.c only calls them when
xgpu_capabilities says the context has them */
#define GL_OPTIONAL_FUNCTION(name) (!strcmp(name, "glCopyImageSubData") || !strcmp(name, "glDrawElementsBaseVertex"))
#else
#define GL_OPTIONAL_FUNCTION(name) 0
#endif
#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name && !GL_OPTIONAL_FUNCTION(#name)) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = FALSE; \
	}
	GL_FUNCTIONS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	return success;
}
