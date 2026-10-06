"""Exercise d3d8_gl.c's vertex constant tracking at its serials' limits.

The real constants_store and prepare_draw's constant upload are cut out of
port/linux/src/d3d8_gl.c and compiled against a recording glUniform4fv: no
game data, GL context or browser is needed. Each case checks that a program's
uploaded registers match the device's after the draw (a wrong-matrix draw
otherwise), and the timeout catches an upload loop that never ends.

    python3 tools/test_renderer_constants.py            # native compiler
    python3 tools/test_renderer_constants.py --emcc     # wasm32 under node,
                                                        # where long is 32-bit
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = Path(os.environ.get("HALO_D3D8_SOURCE", ROOT / "port/linux/src/d3d8_gl.c"))


def block(source, marker, start=0):
    """the text from marker to the end of the brace block after it"""
    begin = source.index(marker, start)
    end = source.index("{", begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


def extract(source):
    entry = block(source, "struct program_entry\n")
    serial_type = re.search(r"\n\s*([a-z ]+?)\s+constants_serial;", entry).group(1)
    declarations = []
    for pattern in (
        r"static [a-z ]+ constant_serials\[XGPU_VERTEX_CONSTANT_COUNT\];",
        r"static [a-z ]+ constants_serial;",
        r"#define CONSTANT_LOG_SIZE \d+",
        r"static unsigned char constant_log\[CONSTANT_LOG_SIZE\];",
    ):
        declarations.append(re.search(pattern, source).group(0))
    store = block(source, "static void constants_store(")
    prepare = source.index("static struct program_entry *prepare_draw(")
    upload = block(source, "if (entry->constants >= 0 && entry->constants_serial != constants_serial)", prepare)
    return serial_type, "\n".join(declarations), store, upload


PRELUDE = r'''
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int GLint, GLsizei, BOOL;
#define TRUE 1
#define FALSE 0
#define XGPU_VERTEX_CONSTANT_COUNT 192
#define PROGRAMS 3
static struct { float constants[XGPU_VERTEX_CONSTANT_COUNT][4]; } device;
static float gpu[PROGRAMS][XGPU_VERTEX_CONSTANT_COUNT][4];
static unsigned uploads;
/* (a program's constants are at location program * 512) */
static void glUniform4fv(GLint location, GLsizei count, const float *values)
{
	int program = location / 512, first = location % 512;

	if (program < 0 || program >= PROGRAMS || count < 0 || first + count > XGPU_VERTEX_CONSTANT_COUNT)
		abort();
	memcpy(gpu[program][first], values, (size_t)count * sizeof(gpu[0][0]));
	uploads++;
}
'''

TESTS = r'''
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static struct program_entry programs[PROGRAMS];

static void upload_constants(struct program_entry *entry)
{
	UPLOAD
}

static void reset(unsigned long long serial)
{
	int p;

	memset(&device, 0, sizeof(device));
	memset(gpu, 0, sizeof(gpu));
	memset(programs, 0, sizeof(programs));
	memset(constant_log, 0, sizeof(constant_log));
	uploads = 0;
	constants_serial = serial;
	for (p = 0; p < XGPU_VERTEX_CONSTANT_COUNT; p++)
		constant_serials[p] = serial;
	for (p = 0; p < PROGRAMS; p++)
	{
		programs[p].constants = p * 512;
		programs[p].constant_count = XGPU_VERTEX_CONSTANT_COUNT;
		programs[p].constants_consecutive = TRUE;
		programs[p].constants_serial = serial;
	}
	programs[2].constants_consecutive = FALSE;
}

static void store(unsigned long index, float value)
{
	float values[4] = { value, value + 1, value + 2, value + 3 };

	constants_store(index, values, 1);
}

/* a draw: the program's registers on the "GPU" are the device's after it */
static void draw(int p)
{
	unsigned long i;

	upload_constants(&programs[p]);
	for (i = 0; i < programs[p].constant_count; i++)
		CHECK(!memcmp(gpu[p][i], device.constants[i], sizeof(gpu[p][i])));
	CHECK(programs[p].constants_serial == constants_serial);
}

/* a skinned draw's node matrices: count registers from first, all changed */
static void skin(unsigned long first, unsigned long count, float base)
{
	unsigned long i;

	for (i = 0; i < count; i++)
		store(first + i, base + (float)i);
}

/* the serial crosses limit with the programs a little behind (the log walk)
and far behind (the full scan) */
static void cross(unsigned long long start, const char *name)
{
	int round;

	reset(start);
	for (round = 0; round < 8; round++)
	{
		skin(8, 132, 1000.0f * (float)(round + 1));
		draw(0);
		skin(8, 4, 50.0f * (float)(round + 1));
		draw(1);
		if (round % 3 == 2)
			draw(2);
	}
	draw(2);
	printf("ok   %s: serial 0x%llx -> 0x%llx, %u uploads\n", name, start,
		(unsigned long long)constants_serial, uploads);
}

int main(void)
{
	printf("serial type: %s (%u bytes), long %u bytes\n", SERIAL_TYPE,
		(unsigned)sizeof(constants_serial), (unsigned)sizeof(long));
	cross(1, "ordinary");
	/* 32-bit wrap: a program current to 0xffffffff and registers changed
	after it took serials 0 and 1, which the log walk and full scan both
	took for older than the program */
	cross(0xffffffffULL - 100, "across 2^32");
	cross(0xffffffffULL - 2, "at 2^32");
	/* the largest serial: the upload loop must still end (upstream #108) */
	reset(ULLONG_MAX - 2);
	store(5, 5.0f);
	store(9, 9.0f);
	CHECK((unsigned long long)constants_serial == ULLONG_MAX || sizeof(constants_serial) < 8);
	draw(0);
	CHECK(uploads == 1);
	draw(0);
	CHECK(uploads == 1);
	printf("ok   upload at the largest serial ends\n");
	printf("PASS\n");
	return 0;
}
'''


def build(workdir, use_emcc):
    source = SOURCE.read_text()
    serial_type, declarations, store, upload = extract(source)
    entry = (
        "struct program_entry\n{\n\tGLint constants;\n\tunsigned long constant_count;\n"
        "\tBOOL constants_consecutive;\n\t%s constants_serial;\n};\n" % serial_type
    )
    program = (
        PRELUDE
        + entry
        + declarations
        + "\n"
        + store
        + "\n"
        + TESTS.replace("UPLOAD", upload).replace("SERIAL_TYPE", '"%s"' % serial_type)
    )
    c_path = Path(workdir) / "renderer_constants.c"
    c_path.write_text(program)
    if use_emcc:
        output = Path(workdir) / "renderer_constants.js"
        compiler = [os.environ.get("EMCC", "emcc"), "-O2", "-sENVIRONMENT=node"]
        run = [os.environ.get("NODE", "node"), str(output)]
    else:
        output = Path(workdir) / "renderer_constants"
        compiler = [os.environ.get("CC", "cc"), "-O2"]
        run = [str(output)]
    subprocess.run(compiler + ["-std=gnu99", "-Wall", "-o", str(output), str(c_path)], check=True)
    return run


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emcc", action="store_true", help="build with emcc, run under node (wasm32)")
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    if args.emcc and not shutil.which(os.environ.get("EMCC", "emcc")):
        sys.exit("emcc not found (source emsdk_env.sh)")
    with tempfile.TemporaryDirectory() as workdir:
        run = build(workdir, args.emcc)
        try:
            result = subprocess.run(run, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print("FAIL: the constant upload did not finish (a loop that never ends)")
            return 1
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
