/*
POSIX_WEB_NET.C

The platform layer's sockets (posix.h, posix_socket_*) for the browser,
which has no UDP or TCP. xnet.c, the Xbox Winsock the game talks to, sits
on these unchanged.

Every socket is virtual. Traffic between this machine's own sockets
(127.0.0.1 or its own address: a host joining its own game) stays here.
Everything else is framed onto one WebSocket to the gateway
(port/gateway, docs/gateway.md), which owns the real sockets next to the
game servers:

  client -> gateway                      gateway -> client
  1 UDP    dst ip4, dst port2, src port2, payload
                                         1 UDP    src ip4, src port2, dst port2, payload
  2 OPEN   stream4, dst ip4, dst port2, src port2
                                         2 OPEN   stream4, src ip4, src port2, dst port2 (inbound)
  3 OPENED stream4, ok1                  3 OPENED stream4, ok1
  4 DATA   stream4, payload              4 DATA   stream4, payload
  5 CLOSE  stream4                       5 CLOSE  stream4
                                         6 HELLO  address ip4 (the address the gateway gave us)

Numbers are big-endian; addresses are in network byte order, as in a
sockaddr_in. A WebSocket message is exactly one frame.

This machine's address is HALO_WEB_ADDRESS, one of the gateway's client
addresses (127.64.x.y; the join token's "adr" claim pins it, and the
gateway's HELLO confirms it, docs/gateway.md). The gateway's URL is
HALO_WEB_GATEWAY (wss://host/gateway) and the join token HALO_WEB_TOKEN,
sent as a WebSocket subprotocol ("t.<token>"); on a reconnect the page's
HALO_WEB_TOKEN_URL gives a new one (web_library.js). Without a gateway,
only local traffic works. Broadcasts (the system link game search) go to
the gateway, which passes them to the servers the token allows.
*/

#include "posix.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include <pthread.h>
#include <time.h>

/* ---------- the JavaScript side (web_library.js) */

extern void webnet_connect(const char *url, const char *token, const char *token_url);
extern int webnet_send(const void *frame, int length);
extern int webnet_receive(void *frame, int capacity);

/* ---------- Winsock errors */

#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAEADDRINUSE 10048
#define WSAECONNRESET 10054
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061

#define SOCKET_BASE 0x4000
#define SOCKET_COUNT 64
#define QUEUE_BYTES 0x20000
#define ACCEPT_BACKLOG 8
#define FRAME_BYTES 0x10000

enum { FRAME_UDP = 1, FRAME_OPEN, FRAME_OPENED, FRAME_DATA, FRAME_CLOSE, FRAME_HELLO };

struct web_socket
{
	int used;
	int type; /* SOCK_DGRAM or SOCK_STREAM */
	int listening, connecting, connected, peer_closed, failed;
	uint16_t local_port; /* network order */
	uint32_t peer_ip;
	uint16_t peer_port;
	uint32_t stream; /* a gateway stream's id, 0 for none */
	int local_peer; /* the other end of a local stream, or -1 */
	int accepts[ACCEPT_BACKLOG];
	int accept_count;
	/* datagrams: records of ip4, port2, length2, bytes; streams: bytes */
	unsigned char queue[QUEUE_BYTES];
	int head, tail;
};

static struct web_socket sockets[SOCKET_COUNT];
static __thread int last_error;
static uint32_t local_address;
static uint16_t next_port = 49152;
static uint32_t next_stream = 1;
static int gateway_opened;
/* the thread that opened the WebSocket: its frames arrive only there (the
socket object is that worker's) */
static pthread_t owner_thread;

static int fail(int error)
{
	last_error = error;
	return -1;
}

/* sends refused for no usable socket (what the game reports as
write_endpoint() errors), for the page's overlay (sdl_platform.c) */
unsigned long web_net_send_errors;

static int send_fail(int error)
{
	web_net_send_errors++;
	return fail(error);
}

static struct web_socket *get(int descriptor)
{
	int index = descriptor - SOCKET_BASE;

	if (index < 0 || index >= SOCKET_COUNT || !sockets[index].used)
		return NULL;
	return &sockets[index];
}

static int queue_used(const struct web_socket *socket)
{
	return socket->tail - socket->head;
}

static int queue_push(struct web_socket *socket, const void *data, int length)
{
	if (socket->tail + length > QUEUE_BYTES)
	{
		memmove(socket->queue, socket->queue + socket->head, (size_t)queue_used(socket));
		socket->tail -= socket->head;
		socket->head = 0;
	}
	if (socket->tail + length > QUEUE_BYTES)
		return 0; /* full: dropped, as a datagram would be */
	memcpy(socket->queue + socket->tail, data, (size_t)length);
	socket->tail += length;
	return 1;
}

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32(unsigned char *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

/* network-order address and port to and from the frames' big-endian fields */
static uint32_t ip_to_wire(uint32_t ip) { return ntohl(ip); }
static uint32_t ip_from_wire(uint32_t wire) { return htonl(wire); }

static void net_initialize(void)
{
	static int initialized;
	const char *address, *gateway;

	if (initialized)
		return;
	initialized = 1;
	owner_thread = pthread_self();
	address = getenv("HALO_WEB_ADDRESS");
	local_address = htonl(0x7f400002); /* 127.64.0.2 */
	if (address && *address)
	{
		unsigned a, b, c, d;

		if (sscanf(address, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
			local_address = htonl(a << 24 | b << 16 | c << 8 | d);
	}
	gateway = getenv("HALO_WEB_GATEWAY");
	if (gateway && *gateway)
	{
		const char *token = getenv("HALO_WEB_TOKEN");
		const char *token_url = getenv("HALO_WEB_TOKEN_URL");

		webnet_connect(gateway, token ? token : "", token_url ? token_url : "");
		gateway_opened = 1;
	}
}

static int is_local(uint32_t ip)
{
	return ip == htonl(INADDR_LOOPBACK) || ip == local_address || ip == 0;
}

static struct web_socket *find_bound(int type, uint16_t port, int listening)
{
	int index;

	for (index = 0; index < SOCKET_COUNT; index++)
	{
		struct web_socket *socket = &sockets[index];

		if (socket->used && socket->type == type && socket->local_port == port &&
			(!listening || socket->listening))
			return socket;
	}
	return NULL;
}

static struct web_socket *find_stream(uint32_t stream)
{
	int index;

	for (index = 0; index < SOCKET_COUNT; index++)
		if (sockets[index].used && sockets[index].stream == stream)
			return &sockets[index];
	return NULL;
}

static int allocate(int type)
{
	int index;

	for (index = 0; index < SOCKET_COUNT; index++)
	{
		if (!sockets[index].used)
		{
			memset(&sockets[index], 0, sizeof(sockets[index]));
			sockets[index].used = 1;
			sockets[index].type = type;
			sockets[index].local_peer = -1;
			return index;
		}
	}
	return -1;
}

static void deliver_datagram(uint32_t source_ip, uint16_t source_port, uint16_t destination_port,
	const void *payload, int length)
{
	struct web_socket *socket = find_bound(SOCK_DGRAM, destination_port, 0);
	unsigned char header[8];

	if (!socket || length > 0xffff)
		return;
	memcpy(header, &source_ip, 4);
	memcpy(header + 4, &source_port, 2);
	header[6] = (unsigned char)length;
	header[7] = (unsigned char)(length >> 8);
	if (queue_used(socket) + 8 + length <= QUEUE_BYTES)
	{
		queue_push(socket, header, 8);
		queue_push(socket, payload, length);
	}
}

/* takes the gateway's frames that have arrived */
static void pump(void)
{
	static unsigned char frame[FRAME_BYTES];
	int length;

	net_initialize();
	if (!gateway_opened)
		return;
	while ((length = webnet_receive(frame, sizeof(frame))) > 0)
	{
		struct web_socket *socket;

		switch (frame[0])
		{
		case FRAME_UDP:
			if (length >= 9)
			{
				uint16_t source_port = htons(get16(frame + 5));
				uint16_t destination_port = htons(get16(frame + 7));

				deliver_datagram(ip_from_wire(get32(frame + 1)), source_port, destination_port, frame + 9, length - 9);
			}
			break;
		case FRAME_OPEN:
			if (length >= 13)
			{
				struct web_socket *listener = find_bound(SOCK_STREAM, htons(get16(frame + 11)), 1);
				unsigned char reply[6];
				int index;

				reply[0] = FRAME_OPENED;
				memcpy(reply + 1, frame + 1, 4);
				reply[5] = 0;
				if (listener && listener->accept_count < ACCEPT_BACKLOG && (index = allocate(SOCK_STREAM)) >= 0)
				{
					sockets[index].connected = 1;
					sockets[index].stream = get32(frame + 1);
					sockets[index].peer_ip = ip_from_wire(get32(frame + 5));
					sockets[index].peer_port = htons(get16(frame + 9));
					sockets[index].local_port = listener->local_port;
					listener->accepts[listener->accept_count++] = SOCKET_BASE + index;
					reply[5] = 1;
				}
				webnet_send(reply, sizeof(reply));
			}
			break;
		case FRAME_OPENED:
			if (length >= 6 && (socket = find_stream(get32(frame + 1))) != NULL)
			{
				socket->connecting = 0;
				socket->connected = frame[5] != 0;
				socket->failed = frame[5] == 0;
			}
			break;
		case FRAME_DATA:
			if (length >= 5 && (socket = find_stream(get32(frame + 1))) != NULL)
				queue_push(socket, frame + 5, length - 5);
			break;
		case FRAME_CLOSE:
			if (length >= 5 && (socket = find_stream(get32(frame + 1))) != NULL)
			{
				socket->peer_closed = 1;
				if (socket->connecting)
				{
					socket->connecting = 0;
					socket->failed = 1;
				}
			}
			break;
		case FRAME_HELLO:
			if (length >= 5)
				local_address = ip_from_wire(get32(frame + 1));
			break;
		}
	}
}

/* ---------- posix.h */

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	int index;

	(void)protocol;
	net_initialize();
	if (family != AF_INET || (type != SOCK_DGRAM && type != SOCK_STREAM))
		return fail(WSAEINVAL);
	index = allocate(type);
	if (index < 0)
		return fail(WSAEMFILE);
	last_error = 0;
	return SOCKET_BASE + index;
}

static void send_close(struct web_socket *socket)
{
	if (socket->stream && socket->local_peer < 0)
	{
		unsigned char frame[5];

		frame[0] = FRAME_CLOSE;
		put32(frame + 1, socket->stream);
		webnet_send(frame, sizeof(frame));
	}
	if (socket->local_peer >= 0)
	{
		struct web_socket *peer = get(socket->local_peer);

		if (peer)
		{
			peer->peer_closed = 1;
			peer->local_peer = -1;
		}
	}
}

int posix_socket_close(int descriptor)
{
	struct web_socket *socket = get(descriptor);

	if (!socket)
		return fail(WSAENOTSOCK);
	send_close(socket);
	socket->used = 0;
	last_error = 0;
	return 0;
}

int posix_socket_bind(int descriptor, const void *address, int address_length)
{
	struct web_socket *socket = get(descriptor);
	const struct sockaddr_in *in = address;
	uint16_t port;

	if (!socket || address_length < (int)sizeof(*in))
		return fail(socket ? WSAEINVAL : WSAENOTSOCK);
	port = in->sin_port;
	if (!port)
		port = htons(next_port++);
	else if (find_bound(socket->type, port, 0))
		return fail(WSAEADDRINUSE);
	socket->local_port = port;
	last_error = 0;
	return 0;
}

static void ensure_bound(struct web_socket *socket)
{
	if (!socket->local_port)
		socket->local_port = htons(next_port++);
}

int posix_socket_connect(int descriptor, const void *address, int address_length)
{
	struct web_socket *socket = get(descriptor);
	const struct sockaddr_in *in = address;

	if (!socket || address_length < (int)sizeof(*in))
		return fail(socket ? WSAEINVAL : WSAENOTSOCK);
	ensure_bound(socket);
	socket->peer_ip = in->sin_addr.s_addr;
	socket->peer_port = in->sin_port;
	if (socket->type == SOCK_DGRAM)
	{
		socket->connected = 1;
		last_error = 0;
		return 0;
	}
	if (socket->connected)
		return fail(WSAEISCONN);
	if (is_local(in->sin_addr.s_addr))
	{
		/* a stream to this machine's own listener: joined directly */
		struct web_socket *listener = find_bound(SOCK_STREAM, in->sin_port, 1);
		int index;

		if (!listener || listener->accept_count >= ACCEPT_BACKLOG || (index = allocate(SOCK_STREAM)) < 0)
			return fail(WSAECONNREFUSED);
		sockets[index].connected = 1;
		sockets[index].local_port = listener->local_port;
		sockets[index].peer_ip = htonl(INADDR_LOOPBACK);
		sockets[index].peer_port = socket->local_port;
		sockets[index].local_peer = descriptor;
		listener->accepts[listener->accept_count++] = SOCKET_BASE + index;
		socket->local_peer = SOCKET_BASE + index;
		socket->connected = 1;
		last_error = 0;
		return 0;
	}
	{
		unsigned char frame[13];

		socket->stream = next_stream++;
		socket->connecting = 1;
		frame[0] = FRAME_OPEN;
		put32(frame + 1, socket->stream);
		put32(frame + 5, ip_to_wire(in->sin_addr.s_addr));
		put16(frame + 9, ntohs(in->sin_port));
		put16(frame + 11, ntohs(socket->local_port));
		if (!webnet_send(frame, sizeof(frame)))
		{
			socket->connecting = 0;
			return fail(WSAECONNREFUSED);
		}
	}
	/* non-blocking, as the game's are: it selects for writeable */
	return fail(WSAEWOULDBLOCK);
}

int posix_socket_listen(int descriptor, int backlog)
{
	struct web_socket *socket = get(descriptor);

	(void)backlog;
	if (!socket || socket->type != SOCK_STREAM)
		return fail(socket ? WSAEINVAL : WSAENOTSOCK);
	ensure_bound(socket);
	socket->listening = 1;
	last_error = 0;
	return 0;
}

int posix_socket_accept(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket = get(descriptor);
	struct web_socket *accepted;
	int result;

	pump();
	if (!socket || !socket->listening)
		return fail(socket ? WSAEINVAL : WSAENOTSOCK);
	if (!socket->accept_count)
		return fail(WSAEWOULDBLOCK);
	result = socket->accepts[0];
	memmove(socket->accepts, socket->accepts + 1, (size_t)--socket->accept_count * sizeof(int));
	accepted = get(result);
	if (accepted && address && address_length && *address_length >= (int)sizeof(struct sockaddr_in))
	{
		struct sockaddr_in *in = address;

		memset(in, 0, sizeof(*in));
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = accepted->peer_ip;
		in->sin_port = accepted->peer_port;
		*address_length = sizeof(*in);
	}
	last_error = 0;
	return result;
}

static int send_datagram(struct web_socket *socket, const void *buffer, int length, uint32_t ip, uint16_t port)
{
	ensure_bound(socket);
	if (length > FRAME_BYTES - 9)
		return send_fail(WSAEMSGSIZE);
	if (is_local(ip) || ip == INADDR_BROADCAST)
		deliver_datagram(htonl(INADDR_LOOPBACK), socket->local_port, port, buffer, length);
	if (!is_local(ip) && gateway_opened)
	{
		static unsigned char frame[FRAME_BYTES];

		frame[0] = FRAME_UDP;
		put32(frame + 1, ip_to_wire(ip));
		put16(frame + 5, ntohs(port));
		put16(frame + 7, ntohs(socket->local_port));
		memcpy(frame + 9, buffer, (size_t)length);
		webnet_send(frame, 9 + length);
	}
	last_error = 0;
	return length;
}

int posix_socket_send(int descriptor, const void *buffer, int length, int flags)
{
	struct web_socket *socket = get(descriptor);

	(void)flags;
	if (!socket)
		return send_fail(WSAENOTSOCK);
	if (socket->type == SOCK_DGRAM)
		return send_datagram(socket, buffer, length, socket->peer_ip, socket->peer_port);
	if (!socket->connected || socket->peer_closed)
		return send_fail(socket->peer_closed ? WSAECONNRESET : WSAENOTCONN);
	if (socket->local_peer >= 0)
	{
		struct web_socket *peer = get(socket->local_peer);

		if (!peer || !queue_push(peer, buffer, length))
			return fail(WSAEWOULDBLOCK);
	}
	else
	{
		static unsigned char frame[FRAME_BYTES];
		int sent = 0;

		while (sent < length)
		{
			int chunk = length - sent > FRAME_BYTES - 5 ? FRAME_BYTES - 5 : length - sent;

			frame[0] = FRAME_DATA;
			put32(frame + 1, socket->stream);
			memcpy(frame + 5, (const char *)buffer + sent, (size_t)chunk);
			webnet_send(frame, 5 + chunk);
			sent += chunk;
		}
	}
	last_error = 0;
	return length;
}

int posix_socket_sendto(int descriptor, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct web_socket *socket = get(descriptor);
	const struct sockaddr_in *in = address;

	(void)flags;
	if (!socket)
		return send_fail(WSAENOTSOCK);
	if (!address || address_length < (int)sizeof(*in))
		return posix_socket_send(descriptor, buffer, length, 0);
	if (socket->type != SOCK_DGRAM)
		return send_fail(WSAEINVAL);
	return send_datagram(socket, buffer, length, in->sin_addr.s_addr, in->sin_port);
}

int posix_socket_recvfrom(int descriptor, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct web_socket *socket = get(descriptor);

	(void)flags;
	pump();
	if (!socket)
		return fail(WSAENOTSOCK);
	if (socket->type == SOCK_DGRAM)
	{
		unsigned char *record = socket->queue + socket->head;
		int size, copied;

		if (queue_used(socket) < 8)
			return fail(WSAEWOULDBLOCK);
		size = record[6] | record[7] << 8;
		copied = size < length ? size : length;
		memcpy(buffer, record + 8, (size_t)copied);
		if (address && address_length && *address_length >= (int)sizeof(struct sockaddr_in))
		{
			struct sockaddr_in *in = address;

			memset(in, 0, sizeof(*in));
			in->sin_family = AF_INET;
			memcpy(&in->sin_addr.s_addr, record, 4);
			memcpy(&in->sin_port, record + 4, 2);
			*address_length = sizeof(*in);
		}
		socket->head += 8 + size;
		if (copied < size)
			return fail(WSAEMSGSIZE);
		last_error = 0;
		return copied;
	}
	{
		int available = queue_used(socket);
		int copied = available < length ? available : length;

		if (!copied)
		{
			if (socket->peer_closed)
			{
				last_error = 0;
				return 0;
			}
			return fail(socket->connected ? WSAEWOULDBLOCK : WSAENOTCONN);
		}
		memcpy(buffer, socket->queue + socket->head, (size_t)copied);
		socket->head += copied;
		if (address && address_length && *address_length >= (int)sizeof(struct sockaddr_in))
		{
			struct sockaddr_in *in = address;

			memset(in, 0, sizeof(*in));
			in->sin_family = AF_INET;
			in->sin_addr.s_addr = socket->peer_ip;
			in->sin_port = socket->peer_port;
			*address_length = sizeof(*in);
		}
		last_error = 0;
		return copied;
	}
}

int posix_socket_recv(int descriptor, void *buffer, int length, int flags)
{
	return posix_socket_recvfrom(descriptor, buffer, length, flags, NULL, NULL);
}

int posix_socket_shutdown(int descriptor, int how)
{
	struct web_socket *socket = get(descriptor);

	(void)how;
	if (!socket)
		return fail(WSAENOTSOCK);
	last_error = 0;
	return 0;
}

int posix_socket_set_nonblocking(int descriptor, int nonblocking)
{
	(void)nonblocking;
	if (!get(descriptor))
		return fail(WSAENOTSOCK);
	last_error = 0;
	return 0;
}

int posix_socket_set_nodelay(int descriptor)
{
	if (!get(descriptor))
		return fail(WSAENOTSOCK);
	last_error = 0;
	return 0;
}

int posix_socket_bytes_available(int descriptor, posix_ulong *count)
{
	struct web_socket *socket = get(descriptor);

	pump();
	if (!socket)
		return fail(WSAENOTSOCK);
	if (socket->type == SOCK_DGRAM)
		*count = queue_used(socket) >= 8 ? (posix_ulong)(socket->queue[socket->head + 6] |
			socket->queue[socket->head + 7] << 8) : 0;
	else
		*count = (posix_ulong)queue_used(socket);
	last_error = 0;
	return 0;
}

int posix_socket_setsockopt(int descriptor, int level, int name, const void *value, int length)
{
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	if (!get(descriptor))
		return fail(WSAENOTSOCK);
	last_error = 0;
	return 0;
}

int posix_socket_getsockopt(int descriptor, int level, int name, void *value, int *length)
{
	struct web_socket *socket = get(descriptor);

	(void)level;
	if (!socket)
		return fail(WSAENOTSOCK);
	if (value && length && *length >= 4)
	{
		int result = 0;

		if (name == 0x1008) /* SO_TYPE */
			result = socket->type;
		else if (name == 0x1007) /* SO_ERROR */
			result = socket->failed ? WSAECONNREFUSED : 0;
		memcpy(value, &result, 4);
		*length = 4;
	}
	last_error = 0;
	return 0;
}

int posix_socket_getsockname(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket = get(descriptor);
	struct sockaddr_in *in = address;

	if (!socket || !address || !address_length || *address_length < (int)sizeof(*in))
		return fail(socket ? WSAEINVAL : WSAENOTSOCK);
	memset(in, 0, sizeof(*in));
	in->sin_family = AF_INET;
	in->sin_addr.s_addr = local_address;
	in->sin_port = socket->local_port;
	*address_length = sizeof(*in);
	last_error = 0;
	return 0;
}

int posix_socket_getpeername(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket = get(descriptor);
	struct sockaddr_in *in = address;

	if (!socket || !socket->connected || !address || !address_length || *address_length < (int)sizeof(*in))
		return fail(socket ? WSAENOTCONN : WSAENOTSOCK);
	memset(in, 0, sizeof(*in));
	in->sin_family = AF_INET;
	in->sin_addr.s_addr = socket->peer_ip;
	in->sin_port = socket->peer_port;
	*address_length = sizeof(*in);
	last_error = 0;
	return 0;
}

static int readable(struct web_socket *socket)
{
	if (socket->listening)
		return socket->accept_count > 0;
	if (socket->type == SOCK_DGRAM)
		return queue_used(socket) >= 8;
	return queue_used(socket) > 0 || socket->peer_closed;
}

static void keep(int *descriptors, int *count, int (*ready)(struct web_socket *), int *total)
{
	int index, kept = 0;

	for (index = 0; index < *count; index++)
	{
		struct web_socket *socket = get(descriptors[index]);

		if (socket && ready(socket))
			descriptors[kept++] = descriptors[index];
	}
	*count = kept;
	*total += kept;
}

static int writeable(struct web_socket *socket)
{
	return socket->type == SOCK_DGRAM || (socket->connected && !socket->connecting);
}

static int errored(struct web_socket *socket)
{
	return socket->failed;
}

/* never blocks: the game's selects poll (a zero timeout) or wait a frame at
most, and the frames of the gateway only arrive when this thread returns to
its event loop, which it does at every SDL_GL_SwapWindow */
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	int total = 0;

	int *read_list = NULL, *write_list = NULL, *error_list = NULL;
	int read_total = read_count ? *read_count : 0, write_total = write_count ? *write_count : 0;
	int error_total = error_count ? *error_count : 0;
	long long wait_us = infinite ? 1000000LL : (long long)timeout_seconds * 1000000LL + timeout_microseconds;

	net_initialize();
	/* A select with a timeout waits for it, in short steps: the owner thread
	of the WebSocket (the game's) yields to its event loop with
	emscripten_sleep (JSPI), which is when the gateway's frames arrive; the
	game's blocking connect (transport_endpoint_winsock.c) waits so for the
	stream to open. Other threads sleep. */
	if (wait_us > 0)
	{
		read_list = read && read_total ? malloc(sizeof(int) * (size_t)read_total) : NULL;
		write_list = write && write_total ? malloc(sizeof(int) * (size_t)write_total) : NULL;
		error_list = error && error_total ? malloc(sizeof(int) * (size_t)error_total) : NULL;
		if (read_list) memcpy(read_list, read, sizeof(int) * (size_t)read_total);
		if (write_list) memcpy(write_list, write, sizeof(int) * (size_t)write_total);
		if (error_list) memcpy(error_list, error, sizeof(int) * (size_t)error_total);
	}
	for (;;)
	{
		total = 0;
		pump();
		if (read)
			keep(read, read_count, readable, &total);
		if (write)
			keep(write, write_count, writeable, &total);
		if (error)
			keep(error, error_count, errored, &total);
		if (total > 0 || wait_us <= 0)
			break;
		{
			long long step = wait_us < 2000 ? wait_us : 2000;

			if (pthread_equal(pthread_self(), owner_thread))
			{
				emscripten_sleep((unsigned int)(step + 999) / 1000);
			}
			else
			{
				struct timespec pause = { 0, (long)(step * 1000) };

				nanosleep(&pause, NULL);
			}
			wait_us -= step;
		}
		/* the lists were cut down to the ready ones: start again from the copies */
		if (read_list) { memcpy(read, read_list, sizeof(int) * (size_t)read_total); *read_count = read_total; }
		if (write_list) { memcpy(write, write_list, sizeof(int) * (size_t)write_total); *write_count = write_total; }
		if (error_list) { memcpy(error, error_list, sizeof(int) * (size_t)error_total); *error_count = error_total; }
	}
	free(read_list);
	free(write_list);
	free(error_list);
	if (total > 0)
		last_error = 0;
	return total;
}

posix_ulong posix_local_ipv4_address(void)
{
	net_initialize();
	return local_address;
}
