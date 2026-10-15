"""The real parse_unicode_string (draw_string.c): "|n" ends a line, any other '|' is a character and the one after it is
read, and a string ending in '|' ends at its terminator, not past it.

Ported from upstream OpenCommunityEdition/OpenCE PR #366 (tools/harness/tests/test_unicode_line_breaks.py). This fork
has no tools/harness, so the extraction is done here and the C side lives in tools/fixtures/unicode_line_breaks.c."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source/text/draw_string.c"
FIXTURE = ROOT / "tools/fixtures/unicode_line_breaks.c"
CHECK_FAILED = 3
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

CASES = ["bar-n-ends-a-line", "bar-is-a-character", "bar-at-the-end"]

# a fault in parse_unicode_string and the case that must catch it
NEGATIVE_CONTROLS = {
    "takes-the-next-character": (("if (string[state->string_index] == 'n')\n\t\t{\n\t\t\tstate->string_index++;",
                                  "if (string[state->string_index++] == 'n')\n\t\t{"), "bar-at-the-end"),
    "bar-keeps-the-last-result": (("\t\telse\n\t\t{\n\t\t\tstate->result = _parsed_character;\n\t\t}\n", ""),
                                  "bar-is-a-character"),
}

pytestmark = pytest.mark.skipif(CC is None, reason="no C compiler")


def parsed_enum(source):
    """the _parsed_* results from the constants enum"""
    names = re.findall(r"^\t(_parsed_\w+ = \d+),", source, re.M)
    assert "_parsed_end_of_string = 0" in names
    return "enum\n{\n" + "".join(f"\t{name},\n" for name in names) + "};\n"


def structure(source, name):
    match = re.search(rf"^struct {name}\n\{{\n.*?^\}};\n", source, re.M | re.S)
    assert match, name
    return match.group(0)


def function(source, name):
    match = re.search(rf"^static \w+ {name}\(\n[^;{{}}]*?\)\n\{{\n.*?^\}}\n", source, re.M | re.S)
    assert match, name
    return match.group(0)


def generated(directory, fault=None):
    source = SOURCE.read_text(encoding="latin-1")
    code = function(source, "parse_unicode_string")
    if fault:
        old, new = fault
        assert old in code, "negative control no longer applies to draw_string.c"
        code = code.replace(old, new, 1)
    (directory / "config.inc").write_text(parsed_enum(source) + structure(source, "parse_string_state"))
    (directory / "under_test.inc").write_text(code + "\n")
    binary = directory / "unicode_line_breaks"
    subprocess.run([CC, "-std=gnu99", "-fshort-wchar", "-w", "-I", str(directory), str(FIXTURE), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def run(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=30)
    return result.returncode, result.stdout + result.stderr


@pytest.mark.parametrize("case", CASES)
def test_case(case, tmp_path):
    status, output = run(generated(tmp_path), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, tmp_path):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(generated(tmp_path, fault), case)
    assert status == CHECK_FAILED, f"draw_string.c with a fault ({control}) passed '{case}': the test cannot see it"
