/*
DEDICATED_CONTROL.H

The dedicated server's control channel (dedicated_control.c): a supervising
process reads the server's events as JSON lines (stdout by default) and
sends it commands as lines (stdin by default). port/linux/README.md,
"Dedicated server control", has the protocol. The events and the commands
themselves are the dedicated server's (port/linux/game/dedicated_server.c);
this is the transport: the pipes, the JSON written and the lines read.

Plain C with no game types, so that the game's and the platform layer's
units can both include it (and a test can build it on its own,
tools/dedicated_control_test.py). Main thread only, except
dedicated_control_signal_write.
*/

#ifndef DEDICATED_CONTROL_H
#define DEDICATED_CONTROL_H

/* raised with any change that is not an addition (a new event, command or
field is not a change of version) */
#define DEDICATED_CONTROL_PROTOCOL_VERSION 1

enum
{
	CONTROL_NAME_LENGTH = 32,
	CONTROL_ID_LENGTH = 64,
	CONTROL_KEY_LENGTH = 32,
	CONTROL_MAXIMUM_ARGUMENTS = 16,
	CONTROL_ARGUMENT_POOL_SIZE = 16384,
};

enum control_value_type
{
	_control_value_string,
	_control_value_number,
	_control_value_boolean,
	_control_value_null,
	/* an array of strings, numbers or booleans: its values with commas
	between them */
	_control_value_array,
};

enum control_id_type
{
	_control_id_none,
	_control_id_string,
	_control_id_number,
};

struct control_argument
{
	char key[CONTROL_KEY_LENGTH];
	int type;
	/* in the command's pool, NUL-terminated (booleans are "true" or
	"false", null is "") */
	unsigned offset;
};

/* a command line, read: {"cmd": "kick", "id": 7, "player": 3} or, typed,
"kick player=3" / "kick 3" */
struct control_command
{
	char name[CONTROL_NAME_LENGTH];
	/* echoed in the reply: a string's text, or a number's digits */
	char id[CONTROL_ID_LENGTH];
	int id_type;
	/* the line was JSON (else text) */
	int json;
	int argument_count;
	struct control_argument arguments[CONTROL_MAXIMUM_ARGUMENTS];
	/* text: the words after the name that are not key=value */
	unsigned text_offset;
	unsigned pool_length;
	char pool[CONTROL_ARGUMENT_POOL_SIZE];
};

/* ---------- the channel */

/* reads server.control and its descriptors, takes the output descriptor
over (stdout's other writers then go to stderr) and writes the "starting"
event; once, from the dedicated server's start (sdl_platform.c); 1 when the
channel is on */
int dedicated_control_initialize(void);
/* 1 when events are being written */
int dedicated_control_output_active(void);
/* once a tick: writes what the pipe had no room for, and reads what
commands have arrived (never waits) */
void dedicated_control_poll(void);
/* the next command read, 1; 0 when there is none left this tick (lines
that are not commands are answered with an error event here) */
int dedicated_control_next_command(struct control_command *command);
/* 1 once, after the command input reached its end (the supervisor closed
it) */
int dedicated_control_input_ended(void);
/* writes everything waiting (at exit), waiting up to the milliseconds for a
full pipe */
void dedicated_control_drain(long milliseconds);
/* a whole line ("{...}\n") straight to the output, from a signal handler:
async-signal-safe, and ahead of anything still waiting */
void dedicated_control_signal_write(const char *line, unsigned long length);

/* ---------- a command's arguments */

/* the value's text, NULL when the command has no such argument */
const char *control_argument(const struct control_command *command, const char *key);
/* 1 and the value when the argument is a whole number (or text of one) */
int control_argument_integer(const struct control_command *command, const char *key, long *value);
/* 1 and the value when the argument is true or false (or 1, 0, yes, no,
on, off) */
int control_argument_boolean(const struct control_command *command, const char *key, int *value);
/* a typed command's words after its name (not key=value ones), "" none */
const char *control_text(const struct control_command *command);

/* ---------- writing an event: begin, fields, end */

/* {"event":<event>,"seq":<n>,"time":<unix seconds>, ... */
void control_begin(const char *event);
/* ... with the command's "id" (null if it had none) and "command" */
void control_begin_reply(const char *event, const struct control_command *command);
/* ...}: the line is queued, and written once the pipe has room */
void control_end(void);

void control_key(const char *key);
void control_string(const char *value);
/* UTF-16 text of up to maximum_length units (or its NUL), as the game's
wide strings are */
void control_utf16(const unsigned short *value, long maximum_length);
void control_integer(long value);
/* null if not finite */
void control_real(double value, int decimals);
void control_boolean(int value);
void control_null(void);
void control_object_begin(void);
void control_object_end(void);
void control_array_begin(void);
void control_array_end(void);

void control_field_string(const char *key, const char *value);
void control_field_utf16(const char *key, const unsigned short *value, long maximum_length);
void control_field_integer(const char *key, long value);
void control_field_real(const char *key, double value, int decimals);
void control_field_boolean(const char *key, int value);
void control_field_null(const char *key);
void control_field_format(const char *key, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* a whole error event: the reply to the command (or NULL: of the server's
own), with its code and message ("fatal": false) */
void control_error(const struct control_command *command, const char *code, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
/* the error the server stops for ("fatal": true), written out before it
does */
void control_fatal(const char *code, const char *format, ...) __attribute__((format(printf, 2, 3)));

#endif
