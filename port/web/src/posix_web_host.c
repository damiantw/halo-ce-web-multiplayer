/*
POSIX_WEB_HOST.C

C library pieces the platform layer's posix_*.c expect from glibc that
Emscripten's musl lacks, and the UPnP entry points (posix.h) the web build
has no use for.
*/

#include "posix.h"

#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
	unsigned char *cursor = buffer;
	size_t left = size;

	(void)flags;
	while (left)
	{
		size_t chunk = left > 256 ? 256 : left;

		if (getentropy(cursor, chunk) != 0)
			return -1;
		cursor += chunk;
		left -= chunk;
	}
	return (ssize_t)size;
}

int posix_upnp_forward_udp(unsigned short port, posix_ulong *external_address, unsigned short *external_port,
	char *error, int error_size)
{
	(void)port;
	(void)external_address;
	(void)external_port;
	if (error && error_size)
		error[0] = 0;
	errno = ENOSYS;
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}
