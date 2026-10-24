"""The scenery and devices whose damage every machine shares (port/linux/game/network_damage.c): a body to destroy, or a
shield to break (Rat Race's energy shields, a map's tripmines), the damage that is such an object's own (a tripmine going
off), and the nearest copy of one made in the game (static_target_near).

Ported from upstream OpenCommunityEdition/OpenCE PR #450 (tools/harness/tests/test_static_target.py). This fork has no
tools/harness, so the extraction is done here and the C side lives in tools/fixtures/static_target.c."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/game/network_damage.c"
FIXTURE = ROOT / "tools/fixtures/static_target.c"
CHECK_FAILED = 1
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
FUNCTIONS = ("static_target", "static_target_damage", "distributed_distance_squared", "static_target_near")

pytestmark = pytest.mark.skipif(CC is None, reason="no C compiler")


def function(source, name):
    """a function's definition, taken by matching braces"""
    match = re.search(r"^(?:static )?[\w *]+?\b" + re.escape(name) + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def build(directory, fault=None):
    source = SOURCE.read_text(encoding="latin-1")
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        before, after = fault
        assert code.count(before) == 1, "negative control no longer applies: %r" % before
        code = code.replace(before, after, 1)
    (directory / "under_test.inc").write_text(code + "\n")
    binary = directory / "static_target"
    subprocess.run([CC, "-std=gnu89", "-w", f"-I{directory}", str(FIXTURE), "-o", str(binary)], check=True)
    return binary


def run(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


@pytest.mark.parametrize("case", ["body", "shield", "neither", "damage", "near"])
def test_case(tmp_path, case):
    status, output = run(build(tmp_path), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    # (the shield-only objects left out, as before: Rat Race's shields, a map's tripmines)
    (("||\n\t\t\tobject_get_maximum_shield_vitality(object_index, TRUE) > 0.0f", ""), "shield"),
    (("STATIC_TARGET_TYPES)", "STATIC_TARGET_TYPES | _object_mask_unit)"), "neither"),
    (("object_get_maximum_body_vitality(object_index, TRUE) > 0.0f ||", ""), "body"),
    (("damage->owner_object_index != NONE && static_target(damage->owner_object_index)", "FALSE"), "damage"),
    (("distance < nearest_distance", "distance > nearest_distance"), "near"),
    (("if (!static_target(iterator.index))\n\t\t\tcontinue;", ""), "near"),
])
def test_negative_control(tmp_path, fault, case):
    status, output = run(build(tmp_path, fault), case)
    assert status == CHECK_FAILED, output
