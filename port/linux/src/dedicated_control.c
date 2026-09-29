/*
DEDICATED_CONTROL.C

The dedicated server's control channel (dedicated_control.h; the protocol
is in port/linux/README.md, "Dedicated server control"). A supervisor that
starts the server owns its pipes: it reads one JSON object a line from the
output (stdout, unless server.control_output_fd names another descriptor)
and writes commands, a line each, to the input (stdin, unless
server.control_input_fd). No socket is opened: only whoever holds the pipes
controls the server.

- The output carries nothing but events. When it is stdout, the channel
  keeps a descriptor of its own for it and points stdout at stderr, so the
  game's own printf output joins the human log (stderr, "halo-linux: ...")
  and cannot break a line.
- The server never waits for the supervisor: a pipe's descriptor is made
  non-blocking and lines it has no room for wait in a 1 MiB queue; beyond
  that they are dropped, and a "dropped" event says how many. A terminal
  or a file is written as usual.
- Commands are read between frames, on the main thread, when poll() says
  there is something: no thread, and no wait when there is nothing.

Linux and macOS-style POSIX systems only: on Windows and Android the channel
stays off (and the functions do nothing).
*/

#include "dedicated_control.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32) && !defined(HALO_ANDROID)
#define CONTROL_POSIX 1
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
/* (posix_files.c's: struct stat is not ours to use in the game's ABI) */
int posix_descriptor_is_stream(int descriptor);
int posix_descriptors_same_file(int first, int second);
#endif

/* the platform layer's (port_config.c, xbox_kernel.c) */
int config_boolean(const char *name);
long config_integer(const char *name);
const char *config_string(const char *name);
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

enum
{
	CONTROL_LINE_SIZE = 256 * 1024,
	CONTROL_QUEUE_SIZE = 1024 * 1024,
	CONTROL_INPUT_SIZE = 64 * 1024,
	CONTROL_MAXIMUM_DEPTH = 32,
	/* commands handled in one tick (the rest wait for the next) */
	CONTROL_COMMANDS_PER_TICK = 32,
	/* reads of the input in one tick */
	CONTROL_READS_PER_TICK = 16,
};

static struct
{
	int initialized;
	int output_fd;
	int input_fd;
	int input_ended;
	int input_end_reported;
	int input_discarding;
	unsigned long sequence;
	unsigned long dropped;
	int commands_this_tick;

	/* the event being written */
	int writing;
	int overflow;
	int depth;
	int after_key;
	unsigned char need_comma[CONTROL_MAXIMUM_DEPTH];
	size_t line_length;
	char line[CONTROL_LINE_SIZE];

	/* written lines the output had no room for yet */
	size_t queue_start;
	size_t queue_length;
	char queue[CONTROL_QUEUE_SIZE];

	size_t input_length;
	char input[CONTROL_INPUT_SIZE];
} control = { .output_fd = -1, .input_fd = -1 };

/* (the descriptor the signal handler writes to, as control.output_fd) */
static volatile int control_signal_fd = -1;

/* ---------- the output */

static double control_unix_time(void);

static void control_output_closed(const char *why)
{
	platform_log("dedicated server: control output closed (%s); events are no longer written", why);
	control.output_fd = -1;
	control_signal_fd = -1;
	control.queue_start = 0;
	control.queue_length = 0;
}

/* writes what it can of the queue; 1 when it is empty */
static int control_flush(void)
{
#ifdef CONTROL_POSIX
	while (control.queue_length > 0 && control.output_fd >= 0)
	{
		ssize_t written = write(control.output_fd, control.queue + control.queue_start, control.queue_length);

		if (written > 0)
		{
			control.queue_start += (size_t)written;
			control.queue_length -= (size_t)written;
		}
		else if (written < 0 && errno == EINTR)
		{
			continue;
		}
		else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			break;
		}
		else
		{
			control_output_closed(written < 0 ? strerror(errno) : "nothing written");
			break;
		}
	}
	if (!control.queue_length)
		control.queue_start = 0;
	else if (control.queue_start > CONTROL_QUEUE_SIZE / 2)
	{
		memmove(control.queue, control.queue + control.queue_start, control.queue_length);
		control.queue_start = 0;
	}
#endif
	return control.queue_length == 0;
}

static int control_queue_has_room(size_t length)
{
	if (control.queue_start + control.queue_length + length > CONTROL_QUEUE_SIZE && control.queue_start)
	{
		memmove(control.queue, control.queue + control.queue_start, control.queue_length);
		control.queue_start = 0;
	}
	return control.queue_length + length <= CONTROL_QUEUE_SIZE;
}

/* the "dropped" notice, once the queue is half empty again: queued ahead of
the next event (which takes the next seq), so seq still counts up */
static void control_queue_dropped_notice(void)
{
	char notice[128];
	int notice_length;

	if (!control.dropped || control.output_fd < 0)
		return;
	control_flush();
	notice_length = snprintf(notice, sizeof(notice), "{\"event\":\"dropped\",\"seq\":%lu,\"time\":%.3f,\"count\":%lu}\n",
		control.sequence, control_unix_time(), control.dropped);
	/* (not until the queue is half empty, or a slow reader would get a
	notice between every line) */
	if (notice_length > 0 && control.queue_length + (size_t)notice_length <= CONTROL_QUEUE_SIZE / 2 &&
		control_queue_has_room((size_t)notice_length))
	{
		memcpy(control.queue + control.queue_start + control.queue_length, notice, (size_t)notice_length);
		control.queue_length += (size_t)notice_length;
		control.sequence++;
		control.dropped = 0;
	}
}

static void control_queue_line(const char *line, size_t length)
{
	if (control.output_fd < 0)
		return;
	/* (lines stay in order: none is queued while a drop is not yet told) */
	if (!control.dropped && control_queue_has_room(length))
	{
		memcpy(control.queue + control.queue_start + control.queue_length, line, length);
		control.queue_length += length;
	}
	else
	{
		control.dropped++;
	}
	control_flush();
}

/* ---------- JSON */

static void control_append(const char *text, size_t length)
{
	if (control.line_length + length + 2 >= CONTROL_LINE_SIZE)
	{
		control.overflow = 1;
		return;
	}
	memcpy(control.line + control.line_length, text, length);
	control.line_length += length;
}

static void control_append_text(const char *text)
{
	control_append(text, strlen(text));
}

static void control_append_character(char character)
{
	control_append(&character, 1);
}

/* before a value: the comma between it and the one before (not after a
key, which has its own) */
static void control_value_prefix(void)
{
	if (control.after_key)
	{
		control.after_key = 0;
		return;
	}
	if (control.need_comma[control.depth])
		control_append_character(',');
	control.need_comma[control.depth] = 1;
}

/* a string's quoted, escaped text; UTF-8 passes as it is */
static void control_append_quoted(const char *text, size_t length)
{
	size_t index;

	control_append_character('"');
	for (index = 0; index < length; index++)
	{
		unsigned char character = (unsigned char)text[index];

		switch (character)
		{
		case '"': control_append_text("\\\""); break;
		case '\\': control_append_text("\\\\"); break;
		case '\n': control_append_text("\\n"); break;
		case '\r': control_append_text("\\r"); break;
		case '\t': control_append_text("\\t"); break;
		default:
			if (character < 0x20 || character == 0x7F)
			{
				char escape[8];

				snprintf(escape, sizeof(escape), "\\u%04x", character);
				control_append_text(escape);
			}
			else
			{
				control_append_character((char)character);
			}
			break;
		}
	}
	control_append_character('"');
}

void control_key(const char *key)
{
	if (!control.writing)
		return;
	if (control.need_comma[control.depth])
		control_append_character(',');
	control.need_comma[control.depth] = 1;
	control_append_quoted(key, strlen(key));
	control_append_character(':');
	control.after_key = 1;
}

void control_string(const char *value)
{
	if (!control.writing)
		return;
	control_value_prefix();
	if (value)
		control_append_quoted(value, strlen(value));
	else
		control_append_text("null");
}

static size_t control_utf8_encode(unsigned long code_point, char *out)
{
	if (code_point < 0x80)
	{
		out[0] = (char)code_point;
		return 1;
	}
	if (code_point < 0x800)
	{
		out[0] = (char)(0xC0 | (code_point >> 6));
		out[1] = (char)(0x80 | (code_point & 0x3F));
		return 2;
	}
	if (code_point < 0x10000)
	{
		out[0] = (char)(0xE0 | (code_point >> 12));
		out[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
		out[2] = (char)(0x80 | (code_point & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (code_point >> 18));
	out[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
	out[3] = (char)(0x80 | (code_point & 0x3F));
	return 4;
}

void control_utf16(const unsigned short *value, long maximum_length)
{
	char text[1024];
	size_t length = 0;
	long index;

	if (!control.writing)
		return;
	if (!value)
	{
		control_null();
		return;
	}
	for (index = 0; index < maximum_length && value[index] && length + 4 < sizeof(text); index++)
	{
		unsigned long code_point = value[index];

		if (code_point >= 0xD800 && code_point <= 0xDBFF && index + 1 < maximum_length &&
			value[index + 1] >= 0xDC00 && value[index + 1] <= 0xDFFF)
		{
			code_point = 0x10000 + ((code_point - 0xD800) << 10) + (value[index + 1] - 0xDC00);
			index++;
		}
		else if (code_point >= 0xD800 && code_point <= 0xDFFF)
		{
			code_point = 0xFFFD;
		}
		length += control_utf8_encode(code_point, text + length);
	}
	control_value_prefix();
	control_append_quoted(text, length);
}

void control_integer(long value)
{
	char text[32];

	if (!control.writing)
		return;
	control_value_prefix();
	snprintf(text, sizeof(text), "%ld", value);
	control_append_text(text);
}

void control_real(double value, int decimals)
{
	char text[64];

	if (!control.writing)
		return;
	control_value_prefix();
	if (!isfinite(value))
	{
		control_append_text("null");
		return;
	}
	snprintf(text, sizeof(text), "%.*f", decimals, value);
	control_append_text(text);
}

void control_boolean(int value)
{
	if (!control.writing)
		return;
	control_value_prefix();
	control_append_text(value ? "true" : "false");
}

void control_null(void)
{
	if (!control.writing)
		return;
	control_value_prefix();
	control_append_text("null");
}

static void control_container_begin(char opening)
{
	if (!control.writing)
		return;
	control_value_prefix();
	control_append_character(opening);
	if (control.depth + 1 < CONTROL_MAXIMUM_DEPTH)
		control.depth++;
	else
		control.overflow = 1;
	control.need_comma[control.depth] = 0;
}

static void control_container_end(char closing)
{
	if (!control.writing)
		return;
	control_append_character(closing);
	if (control.depth > 1)
		control.depth--;
	control.after_key = 0;
}

void control_object_begin(void) { control_container_begin('{'); }
void control_object_end(void) { control_container_end('}'); }
void control_array_begin(void) { control_container_begin('['); }
void control_array_end(void) { control_container_end(']'); }

void control_field_string(const char *key, const char *value) { control_key(key); control_string(value); }
void control_field_utf16(const char *key, const unsigned short *value, long maximum_length)
{
	control_key(key);
	control_utf16(value, maximum_length);
}
void control_field_integer(const char *key, long value) { control_key(key); control_integer(value); }
void control_field_real(const char *key, double value, int decimals) { control_key(key); control_real(value, decimals); }
void control_field_boolean(const char *key, int value) { control_key(key); control_boolean(value); }
void control_field_null(const char *key) { control_key(key); control_null(); }

void control_field_format(const char *key, const char *format, ...)
{
	char text[2048];
	va_list arguments;

	if (!control.writing)
		return;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	control_field_string(key, text);
}

static double control_unix_time(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void control_begin(const char *event)
{
	if (control.output_fd < 0)
		return;
	control_queue_dropped_notice();
	control.writing = 1;
	control.overflow = 0;
	control.line_length = 0;
	control.depth = 1;
	control.after_key = 0;
	control.need_comma[1] = 0;
	control_append_character('{');
	control_field_string("event", event);
	control_field_integer("seq", (long)control.sequence++);
	control_field_real("time", control_unix_time(), 3);
}

static void control_field_id(const struct control_command *command)
{
	control_key("id");
	if (!command || command->id_type == _control_id_none)
		control_null();
	else if (command->id_type == _control_id_number)
	{
		control_value_prefix();
		control_append_text(command->id);
	}
	else
	{
		control_string(command->id);
	}
}

void control_begin_reply(const char *event, const struct control_command *command)
{
	control_begin(event);
	control_field_id(command);
	if (command && command->name[0])
		control_field_string("command", command->name);
	else
		control_field_null("command");
}

void control_end(void)
{
	if (!control.writing)
		return;
	control.writing = 0;
	if (control.overflow)
	{
		char replacement[256];
		int length = snprintf(replacement, sizeof(replacement),
			"{\"event\":\"error\",\"seq\":%lu,\"time\":%.3f,\"id\":null,\"command\":null,\"code\":\"event_too_large\","
			"\"fatal\":false,\"message\":\"an event did not fit in %d bytes and was left out\"}\n",
			control.sequence++, control_unix_time(), (int)CONTROL_LINE_SIZE);

		if (length > 0)
			control_queue_line(replacement, (size_t)length);
		return;
	}
	control.line[control.line_length++] = '}';
	control.line[control.line_length++] = '\n';
	control_queue_line(control.line, control.line_length);
}

static void control_error_valist(const struct control_command *command, int fatal, const char *code,
	const char *format, va_list arguments)
{
	char message[1024];

	vsnprintf(message, sizeof(message), format, arguments);
	control_begin_reply("error", command);
	control_field_string("code", code);
	control_field_boolean("fatal", fatal);
	control_field_string("message", message);
	control_end();
}

void control_error(const struct control_command *command, const char *code, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	control_error_valist(command, 0, code, format, arguments);
	va_end(arguments);
}

void control_fatal(const char *code, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	control_error_valist(NULL, 1, code, format, arguments);
	va_end(arguments);
	dedicated_control_drain(2000);
}

/* ---------- the channel */

#ifdef CONTROL_POSIX
static int control_descriptor_open(int fd)
{
	return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

/* the output: its own descriptor for stdout, with stdout's other writers
sent to stderr; non-blocking if it is a pipe or a socket nobody else writes
through */
static int control_take_output(int fd)
{
	if (!control_descriptor_open(fd))
	{
		platform_log("dedicated server: server.control_output_fd %d is not open; no control events", fd);
		return -1;
	}
	if (fd == STDOUT_FILENO)
	{
		int copy;

		fflush(stdout);
		copy = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
		if (copy < 0)
		{
			platform_log("dedicated server: cannot keep stdout for control events (%s)", strerror(errno));
			return -1;
		}
		if (control_descriptor_open(STDERR_FILENO))
			dup2(STDERR_FILENO, STDOUT_FILENO);
		fd = copy;
	}
	else
	{
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
	if (posix_descriptor_is_stream(fd) && !posix_descriptors_same_file(fd, STDERR_FILENO))
	{
		int flags = fcntl(fd, F_GETFL);

		if (flags != -1)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	return fd;
}
#endif

int dedicated_control_initialize(void)
{
	if (control.initialized)
		return control.output_fd >= 0 || control.input_fd >= 0;
	control.initialized = 1;
#ifdef CONTROL_POSIX
	if (!config_boolean("server.control"))
	{
		platform_log("dedicated server: control channel off (server.control = false)");
		return 0;
	}
	{
		long output_fd = config_integer("server.control_output_fd");
		long input_fd = config_integer("server.control_input_fd");

		/* (a supervisor that goes away closes the pipe: a write then fails
		with EPIPE, and the events stop, rather than the signal ending the
		server; sdl_platform.c ignores it too) */
		signal(SIGPIPE, SIG_IGN);
		if (output_fd >= 0)
			control.output_fd = control_take_output((int)output_fd);
		control_signal_fd = control.output_fd;
		if (input_fd >= 0)
		{
			if (control_descriptor_open((int)input_fd))
			{
				control.input_fd = (int)input_fd;
				if (input_fd > STDERR_FILENO)
					fcntl((int)input_fd, F_SETFD, FD_CLOEXEC);
			}
			else
			{
				platform_log("dedicated server: server.control_input_fd %ld is not open; no control commands",
					input_fd);
			}
		}
		if (control.output_fd < 0 && control.input_fd < 0)
			return 0;
		platform_log("dedicated server: control channel: events to descriptor %ld, commands from %s", output_fd,
			control.input_fd >= 0 ? (input_fd ? "a descriptor" : "stdin") : "nowhere");
		control_begin("starting");
		control_field_integer("protocol", DEDICATED_CONTROL_PROTOCOL_VERSION);
		control_field_integer("pid", (long)getpid());
		control_field_string("name", config_string("server.name"));
		control_field_boolean("commands", control.input_fd >= 0);
		control_end();
		return 1;
	}
#else
	platform_log("dedicated server: no control channel on this platform");
	return 0;
#endif
}

int dedicated_control_output_active(void)
{
	return control.output_fd >= 0;
}

void dedicated_control_drain(long milliseconds)
{
#ifdef CONTROL_POSIX
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!control_flush() && control.output_fd >= 0)
	{
		struct pollfd descriptor = { control.output_fd, POLLOUT, 0 };
		long elapsed;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
		if (elapsed >= milliseconds)
			break;
		poll(&descriptor, 1, (int)(milliseconds - elapsed));
	}
#else
	(void)milliseconds;
#endif
}

void dedicated_control_signal_write(const char *line, unsigned long length)
{
#ifdef CONTROL_POSIX
	int fd = control_signal_fd;

	/* (a new line first, in case a line was part written) */
	if (fd >= 0)
	{
		(void)!write(fd, "\n", 1);
		(void)!write(fd, line, length);
	}
#else
	(void)line;
	(void)length;
#endif
}

/* ---------- the input */

#ifdef CONTROL_POSIX
static void control_read_input(void)
{
	int reads;

	for (reads = 0; reads < CONTROL_READS_PER_TICK && control.input_fd >= 0 && !control.input_ended &&
		control.input_length < CONTROL_INPUT_SIZE; reads++)
	{
		struct pollfd descriptor = { control.input_fd, POLLIN, 0 };
		int ready = poll(&descriptor, 1, 0);
		ssize_t count;

		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			break;
		if (descriptor.revents & POLLNVAL)
		{
			control.input_ended = 1;
			break;
		}
		count = read(control.input_fd, control.input + control.input_length, CONTROL_INPUT_SIZE - control.input_length);
		if (count > 0)
			control.input_length += (size_t)count;
		else if (count == 0)
			control.input_ended = 1;
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			control.input_ended = 1;
		else
			break;
	}
	if (control.input_ended && control.input_fd >= 0)
	{
		platform_log("dedicated server: the control input has ended; no more commands");
		control.input_fd = -1;
	}
}
#endif

void dedicated_control_poll(void)
{
	control.commands_this_tick = 0;
	control_flush();
#ifdef CONTROL_POSIX
	control_read_input();
#endif
}

int dedicated_control_input_ended(void)
{
	if (control.input_ended && !control.input_end_reported && !control.input_length)
	{
		control.input_end_reported = 1;
		return 1;
	}
	return 0;
}

/* ---------- commands */

const char *control_argument(const struct control_command *command, const char *key)
{
	int index;

	for (index = 0; index < command->argument_count; index++)
	{
		if (!strcmp(command->arguments[index].key, key))
			return command->pool + command->arguments[index].offset;
	}
	return NULL;
}

int control_argument_integer(const struct control_command *command, const char *key, long *value)
{
	const char *text = control_argument(command, key);
	char *end;
	double number;

	if (!text || !text[0])
		return 0;
	number = strtod(text, &end);
	if (*end || number != floor(number) || number < -2147483648.0 || number > 2147483647.0)
		return 0;
	*value = (long)number;
	return 1;
}

static int control_text_boolean(const char *text, int *value)
{
	if (!strcmp(text, "true") || !strcmp(text, "1") || !strcmp(text, "yes") || !strcmp(text, "on"))
		*value = 1;
	else if (!strcmp(text, "false") || !strcmp(text, "0") || !strcmp(text, "no") || !strcmp(text, "off"))
		*value = 0;
	else
		return 0;
	return 1;
}

int control_argument_boolean(const struct control_command *command, const char *key, int *value)
{
	const char *text = control_argument(command, key);

	return text && control_text_boolean(text, value);
}

const char *control_text(const struct control_command *command)
{
	return command->pool + command->text_offset;
}

/* a pool string: the offset of its first byte, the rest appended with
control_pool_append and closed with control_pool_close */
static int control_pool_append(struct control_command *command, const char *text, size_t length)
{
	if (command->pool_length + length + 1 > CONTROL_ARGUMENT_POOL_SIZE)
		return 0;
	memcpy(command->pool + command->pool_length, text, length);
	command->pool_length += (unsigned)length;
	return 1;
}

static int control_pool_close(struct control_command *command)
{
	if (command->pool_length + 1 > CONTROL_ARGUMENT_POOL_SIZE)
		return 0;
	command->pool[command->pool_length++] = 0;
	return 1;
}

static void control_command_clear(struct control_command *command)
{
	command->name[0] = 0;
	command->id[0] = 0;
	command->id_type = _control_id_none;
	command->json = 0;
	command->argument_count = 0;
	command->pool_length = 1;
	command->pool[0] = 0;
	command->text_offset = 0;
}

static int control_add_argument(struct control_command *command, const char *key, int type, unsigned offset)
{
	struct control_argument *argument;
	int index;

	/* (a key given twice: the last one) */
	for (index = 0; index < command->argument_count; index++)
	{
		if (!strcmp(command->arguments[index].key, key))
			break;
	}
	if (index == command->argument_count)
	{
		if (command->argument_count >= CONTROL_MAXIMUM_ARGUMENTS)
			return 0;
		command->argument_count++;
	}
	argument = &command->arguments[index];
	snprintf(argument->key, sizeof(argument->key), "%s", key);
	argument->type = type;
	argument->offset = offset;
	return 1;
}

/* ---------- commands: JSON */

struct control_parser
{
	const char *at;
	const char *end;
	const char *error;
};

static void control_skip_space(struct control_parser *parser)
{
	while (parser->at < parser->end &&
		(*parser->at == ' ' || *parser->at == '\t' || *parser->at == '\r' || *parser->at == '\n'))
	{
		parser->at++;
	}
}

static int control_hex4(const char *text, unsigned long *value)
{
	int index;

	*value = 0;
	for (index = 0; index < 4; index++)
	{
		char character = text[index];

		*value <<= 4;
		if (character >= '0' && character <= '9')
			*value |= (unsigned long)(character - '0');
		else if (character >= 'a' && character <= 'f')
			*value |= (unsigned long)(character - 'a' + 10);
		else if (character >= 'A' && character <= 'F')
			*value |= (unsigned long)(character - 'A' + 10);
		else
			return 0;
	}
	return 1;
}

/* a JSON string, decoded into the pool (not closed) */
static int control_parse_string(struct control_parser *parser, struct control_command *command)
{
	if (parser->at >= parser->end || *parser->at != '"')
	{
		parser->error = "a string was expected";
		return 0;
	}
	parser->at++;
	while (parser->at < parser->end && *parser->at != '"')
	{
		char character = *parser->at++;

		if ((unsigned char)character < 0x20)
		{
			parser->error = "a control character in a string";
			return 0;
		}
		if (character == '\\')
		{
			char escape;
			char text[4];

			if (parser->at >= parser->end)
				break;
			escape = *parser->at++;
			switch (escape)
			{
			case '"': case '\\': case '/': text[0] = escape; break;
			case 'b': text[0] = '\b'; break;
			case 'f': text[0] = '\f'; break;
			case 'n': text[0] = '\n'; break;
			case 'r': text[0] = '\r'; break;
			case 't': text[0] = '\t'; break;
			case 'u':
			{
				unsigned long code_point, low;

				if (parser->end - parser->at < 4 || !control_hex4(parser->at, &code_point))
				{
					parser->error = "a bad \\u escape";
					return 0;
				}
				parser->at += 4;
				if (code_point >= 0xD800 && code_point <= 0xDBFF)
				{
					if (parser->end - parser->at >= 6 && parser->at[0] == '\\' && parser->at[1] == 'u' &&
						control_hex4(parser->at + 2, &low) && low >= 0xDC00 && low <= 0xDFFF)
					{
						code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
						parser->at += 6;
					}
					else
					{
						code_point = 0xFFFD;
					}
				}
				else if (code_point >= 0xDC00 && code_point <= 0xDFFF)
				{
					code_point = 0xFFFD;
				}
				if (!code_point)
				{
					parser->error = "a NUL character in a string";
					return 0;
				}
				if (!control_pool_append(command, text, control_utf8_encode(code_point, text)))
				{
					parser->error = "the command is too long";
					return 0;
				}
				continue;
			}
			default:
				parser->error = "a bad escape in a string";
				return 0;
			}
			if (!control_pool_append(command, text, 1))
			{
				parser->error = "the command is too long";
				return 0;
			}
			continue;
		}
		if (!control_pool_append(command, &character, 1))
		{
			parser->error = "the command is too long";
			return 0;
		}
	}
	if (parser->at >= parser->end)
	{
		parser->error = "a string does not end";
		return 0;
	}
	parser->at++;
	return 1;
}

/* a number, true, false or null: its text into the pool (not closed) */
static int control_parse_scalar(struct control_parser *parser, struct control_command *command, int *type)
{
	static const char *const literals[] = { "true", "false", "null" };
	const char *start = parser->at;
	int index;

	for (index = 0; index < 3; index++)
	{
		size_t length = strlen(literals[index]);

		if ((size_t)(parser->end - parser->at) >= length && !memcmp(parser->at, literals[index], length))
		{
			parser->at += length;
			*type = index < 2 ? _control_value_boolean : _control_value_null;
			return index == 2 || control_pool_append(command, literals[index], length);
		}
	}
	/* -?digits[.digits][e[+-]digits] */
	if (parser->at < parser->end && *parser->at == '-')
		parser->at++;
	if (parser->at >= parser->end || *parser->at < '0' || *parser->at > '9')
	{
		parser->at = start;
		parser->error = "a value was expected";
		return 0;
	}
	while (parser->at < parser->end && *parser->at >= '0' && *parser->at <= '9')
		parser->at++;
	if (parser->at < parser->end && *parser->at == '.')
	{
		parser->at++;
		while (parser->at < parser->end && *parser->at >= '0' && *parser->at <= '9')
			parser->at++;
	}
	if (parser->at < parser->end && (*parser->at == 'e' || *parser->at == 'E'))
	{
		parser->at++;
		if (parser->at < parser->end && (*parser->at == '+' || *parser->at == '-'))
			parser->at++;
		while (parser->at < parser->end && *parser->at >= '0' && *parser->at <= '9')
			parser->at++;
	}
	*type = _control_value_number;
	if (!control_pool_append(command, start, (size_t)(parser->at - start)))
	{
		parser->error = "the command is too long";
		return 0;
	}
	return 1;
}

/* any value but an object; an array's values with commas between them */
static int control_parse_value(struct control_parser *parser, struct control_command *command, int *type)
{
	if (parser->at >= parser->end)
	{
		parser->error = "a value was expected";
		return 0;
	}
	if (*parser->at == '"')
	{
		*type = _control_value_string;
		return control_parse_string(parser, command);
	}
	if (*parser->at == '{')
	{
		parser->error = "objects are not taken as arguments";
		return 0;
	}
	if (*parser->at == '[')
	{
		int first = 1;

		parser->at++;
		control_skip_space(parser);
		if (parser->at < parser->end && *parser->at == ']')
		{
			parser->at++;
			*type = _control_value_array;
			return 1;
		}
		for (;;)
		{
			int element_type;

			if (!first && !control_pool_append(command, ",", 1))
			{
				parser->error = "the command is too long";
				return 0;
			}
			first = 0;
			control_skip_space(parser);
			if (parser->at < parser->end && (*parser->at == '[' || *parser->at == '{'))
			{
				parser->error = "arrays of arrays or objects are not taken";
				return 0;
			}
			if (!control_parse_value(parser, command, &element_type))
				return 0;
			control_skip_space(parser);
			if (parser->at < parser->end && *parser->at == ',')
			{
				parser->at++;
				continue;
			}
			if (parser->at < parser->end && *parser->at == ']')
			{
				parser->at++;
				break;
			}
			parser->error = "an array does not end";
			return 0;
		}
		*type = _control_value_array;
		return 1;
	}
	return control_parse_scalar(parser, command, type);
}

static int control_parse_json(const char *line, size_t length, struct control_command *command)
{
	struct control_parser parser = { line, line + length, NULL };

	command->json = 1;
	control_skip_space(&parser);
	parser.at++; /* ({) */
	control_skip_space(&parser);
	if (parser.at < parser.end && *parser.at == '}')
	{
		parser.at++;
	}
	else
	{
		for (;;)
		{
			char key[CONTROL_KEY_LENGTH];
			unsigned key_offset = command->pool_length;
			unsigned value_offset;
			int type;

			control_skip_space(&parser);
			if (!control_parse_string(&parser, command))
				break;
			snprintf(key, sizeof(key), "%.*s", (int)(command->pool_length - key_offset), command->pool + key_offset);
			command->pool_length = key_offset;
			control_skip_space(&parser);
			if (parser.at >= parser.end || *parser.at != ':')
			{
				parser.error = "a ':' was expected";
				break;
			}
			parser.at++;
			control_skip_space(&parser);
			value_offset = command->pool_length;
			if (!control_parse_value(&parser, command, &type) || !control_pool_close(command))
			{
				if (!parser.error)
					parser.error = "the command is too long";
				break;
			}
			if (!strcmp(key, "id"))
			{
				if (type == _control_value_string || type == _control_value_number)
				{
					snprintf(command->id, sizeof(command->id), "%s", command->pool + value_offset);
					command->id_type = type == _control_value_string ? _control_id_string : _control_id_number;
				}
				else if (type != _control_value_null)
				{
					parser.error = "\"id\" is to be a string or a number";
					break;
				}
				command->pool_length = value_offset;
			}
			else if (!strcmp(key, "cmd") || !strcmp(key, "command"))
			{
				if (type != _control_value_string)
				{
					parser.error = "\"cmd\" is to be a string";
					break;
				}
				snprintf(command->name, sizeof(command->name), "%s", command->pool + value_offset);
				command->pool_length = value_offset;
			}
			else if (!control_add_argument(command, key, type, value_offset))
			{
				parser.error = "too many arguments";
				break;
			}
			control_skip_space(&parser);
			if (parser.at < parser.end && *parser.at == ',')
			{
				parser.at++;
				continue;
			}
			if (parser.at < parser.end && *parser.at == '}')
			{
				parser.at++;
				break;
			}
			parser.error = "a ',' or '}' was expected";
			break;
		}
	}
	if (!parser.error)
	{
		control_skip_space(&parser);
		if (parser.at != parser.end)
			parser.error = "text after the object";
	}
	if (parser.error)
	{
		control_error(command, "bad_json", "%s (at byte %ld)", parser.error, (long)(parser.at - line));
		return 0;
	}
	return 1;
}

/* ---------- commands: text */

/* "name word word key=value ...": the key=value words are arguments, the
other words the text */
static int control_parse_text(const char *line, size_t length, struct control_command *command)
{
	const char *at = line;
	const char *end = line + length;
	size_t name_length = 0;
	int first_word = 1;
	unsigned text_offset = 0;

	while (at < end && (*at == ' ' || *at == '\t'))
		at++;
	while (at < end && *at != ' ' && *at != '\t')
	{
		if (name_length + 1 < sizeof(command->name))
			command->name[name_length++] = (char)(*at >= 'A' && *at <= 'Z' ? *at - 'A' + 'a' : *at);
		at++;
	}
	command->name[name_length] = 0;

	/* (the text first, in one piece in the pool) */
	{
		const char *scan = at;

		text_offset = command->pool_length;
		while (scan < end)
		{
			const char *word;
			size_t word_length;

			while (scan < end && (*scan == ' ' || *scan == '\t'))
				scan++;
			word = scan;
			while (scan < end && *scan != ' ' && *scan != '\t')
				scan++;
			word_length = (size_t)(scan - word);
			if (!word_length || memchr(word, '=', word_length))
				continue;
			if ((!first_word && !control_pool_append(command, " ", 1)) ||
				!control_pool_append(command, word, word_length))
			{
				control_error(command, "bad_request", "the command is too long");
				return 0;
			}
			first_word = 0;
		}
		if (!control_pool_close(command))
		{
			control_error(command, "bad_request", "the command is too long");
			return 0;
		}
		command->text_offset = text_offset;
	}
	while (at < end)
	{
		const char *word;
		const char *equals;
		size_t word_length;
		char key[CONTROL_KEY_LENGTH];
		unsigned value_offset;

		while (at < end && (*at == ' ' || *at == '\t'))
			at++;
		word = at;
		while (at < end && *at != ' ' && *at != '\t')
			at++;
		word_length = (size_t)(at - word);
		equals = word_length ? memchr(word, '=', word_length) : NULL;
		if (!equals || equals == word)
			continue;
		snprintf(key, sizeof(key), "%.*s", (int)(equals - word), word);
		value_offset = command->pool_length;
		if (!control_pool_append(command, equals + 1, (size_t)(at - equals - 1)) || !control_pool_close(command) ||
			!control_add_argument(command, key, _control_value_string, value_offset))
		{
			control_error(command, "bad_request", "too many arguments, or too long");
			return 0;
		}
	}
	return 1;
}

/* ---------- commands: lines */

static int control_valid_name(const char *name)
{
	const char *at;

	if (!name[0])
		return 0;
	for (at = name; *at; at++)
	{
		if (!((*at >= 'a' && *at <= 'z') || (*at >= '0' && *at <= '9') || *at == '_'))
			return 0;
	}
	return 1;
}

/* a line into the command: 1 when it is one (anything else is answered) */
static int control_parse_line(const char *line, size_t length, struct control_command *command)
{
	const char *at = line;

	control_command_clear(command);
	while (length && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t'))
		length--;
	while (at < line + length && (*at == ' ' || *at == '\t'))
		at++;
	/* (blank lines and # comments are nothing) */
	if (at == line + length || *at == '#')
		return 0;
	if (memchr(line, 0, length))
	{
		control_error(NULL, "bad_request", "a NUL byte in the line");
		return 0;
	}
	if (*at == '{')
	{
		if (!control_parse_json(line, length, command))
			return 0;
	}
	else if (*at == '[' || *at == '"')
	{
		control_error(NULL, "bad_json", "a command is a JSON object (or a line of text)");
		return 0;
	}
	else if (!control_parse_text(line, length, command))
	{
		return 0;
	}
	if (!command->name[0])
	{
		control_error(command, "bad_request", "no \"cmd\"");
		return 0;
	}
	if (!control_valid_name(command->name))
	{
		char name[CONTROL_NAME_LENGTH];

		snprintf(name, sizeof(name), "%s", command->name);
		command->name[0] = 0;
		control_error(command, "unknown_command", "no command \"%s\"", name);
		return 0;
	}
	return 1;
}

int dedicated_control_next_command(struct control_command *command)
{
	while (control.commands_this_tick < CONTROL_COMMANDS_PER_TICK && control.input_length)
	{
		char *newline = memchr(control.input, '\n', control.input_length);
		size_t line_length;
		size_t used;
		int parsed;

		if (!newline)
		{
			if (control.input_length == CONTROL_INPUT_SIZE)
			{
				/* (the rest of the line, when it comes, is left out too) */
				if (!control.input_discarding)
					control_error(NULL, "line_too_long", "a line of more than %d bytes was left out", CONTROL_INPUT_SIZE);
				control.input_discarding = 1;
				control.input_length = 0;
				return 0;
			}
			if (!control.input_ended)
				return 0;
			/* (the last line, with no newline) */
			line_length = control.input_length;
			used = control.input_length;
		}
		else
		{
			line_length = (size_t)(newline - control.input);
			used = line_length + 1;
		}
		if (control.input_discarding)
		{
			control.input_discarding = 0;
			parsed = 0;
		}
		else
		{
			control.commands_this_tick++;
			parsed = control_parse_line(control.input, line_length, command);
		}
		memmove(control.input, control.input + used, control.input_length - used);
		control.input_length -= used;
		if (parsed)
			return 1;
	}
	return 0;
}
