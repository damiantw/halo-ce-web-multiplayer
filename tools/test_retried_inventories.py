"""A client's inventories from the host whose unit or weapons have not come yet (port/linux/game/network_objects.c):
kept, the newest of a unit's, and applied once they have; let go of EMPTY_INVENTORY_TICKS after the newest came.

Ported from upstream OpenCommunityEdition/OpenCE PR #466 (tools/harness/tests/test_retried_inventories.py). This fork
has no tools/harness, so the extraction is done here and the C side lives in tools/fixtures/retried_inventories.c."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tools/fixtures/retried_inventories.c"
CHECK_FAILED = 1
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

pytestmark = pytest.mark.skipif(CC is None, reason="no C compiler")

CASES = ["unit-late", "weapon-late", "newest", "full", "forget", "expiry"]
CONTROLS = [
    ("else\n\t\tdistributed_client_retry_inventory(inventory);", "else\n\t\t(void)0;", "unit-late"),
    ("distributed_client_apply_inventory(&inventory);", "(void)inventory;", "unit-late"),
    ("if (!distributed_client_weapon_here(inventory->weapon_indices[weapon_slot]))", "if (FALSE)", "weapon-late"),
    ("if (objects_client_retried_inventories[index].inventory.unit_index == inventory->unit_index)", "if (FALSE)",
     "newest"),
    (".time < objects_client_retried_inventories[oldest].time", ".time > objects_client_retried_inventories[oldest].time",
     "full"),
    ("objects_client_retried_inventory_count < MAXIMUM_CLIENT_RETRIED_INVENTORIES",
     "objects_client_retried_inventory_count < MAXIMUM_CLIENT_RETRIED_INVENTORIES - 1", "full"),
    ("objects_client_retried_inventories[index] =\n\t\t\t\tobjects_client_retried_inventories[--objects_client_retried_inventory_count];",
     "--objects_client_retried_inventory_count;", "forget"),
    ("distributed_client_forget_retried_inventory(inventory->unit_index);\n\telse", "(void)0;\n\telse", "forget"),
    ("- retried->time >= EMPTY_INVENTORY_TICKS", "- retried->time > EMPTY_INVENTORY_TICKS", "expiry"),
    ("\tretried->inventory = *inventory;\n\tretried->time = game_time_get();",
     "\tif (retried->inventory.unit_index != inventory->unit_index)\n\t\tretried->time = game_time_get();\n"
     "\tretried->inventory = *inventory;", "expiry"),
]


def read(relative):
    return (ROOT / relative).read_text(encoding="latin-1")


def function(source, name):
    """a function's definition, taken by matching braces"""
    match = re.search(r"^(?:static |__inline )?[\w *]+?\b" + re.escape(name) + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def enum_with(source, member):
    """the whole enum that declares a member"""
    at = source.index(member)
    start = source.rindex("enum", 0, at)
    return source[start:source.index("};", at) + 2]


def structure(source, name):
    """a named structure's definition"""
    match = re.search(r"^struct\s+" + re.escape(name) + r"\s*\{", source, re.M)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():source.index(";", end) + 1]


def constant(source, name):
    """an enum constant's or #define's integer value, following a name it is set to"""
    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*(\w+)", source) or \
        re.search(r"#define\s+" + re.escape(name) + r"\s+(\w+)", source)
    assert match, name
    value = match.group(1)
    if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", value):
        return int(value, 0)
    return constant(source + read("port/linux/include/halo_port_capacity.h"), value)


def declaration(source, first, last):
    """the text from one marker to the end of another, as a static's declaration with its structure"""
    start = source.index(first)
    return source[start:source.index(last, start) + len(last)]


def build(directory, fault=None):
    network = read("port/linux/game/network_objects.c")
    units = read("source/units/units.h")
    limits = read("port/linux/game/network_distributed.h") + read("port/linux/include/halo_port_limits.h")
    config = "\n".join([
        "#define TICKS_PER_SECOND %d" % constant(read("source/cseries/cseries.h"), "TICKS_PER_SECOND"),
        "#define MAXIMUM_OBJECTS_PER_MAP %d" % constant(read("source/objects/objects.h"), "MAXIMUM_OBJECTS_PER_MAP"),
        "#define MAXIMUM_TRACKED_PLAYERS %d" % constant(limits, "MAXIMUM_TRACKED_PLAYERS"),
        re.search(r"^#define AGE_SCALE .*$", network, re.M).group(0),
        enum_with(read("source/objects/object_types.h"), "_object_mask_unit"),
        enum_with(read("source/objects/objects.h"), "_object_dead_bit"),
        enum_with(read("source/game/players.h"), "MAXIMUM_LOCAL_PLAYERS"),
        enum_with(units, "MAXIMUM_WEAPONS_PER_UNIT ="),
        enum_with(units, "NUMBER_OF_UNIT_GRENADE_TYPES\n"),
        enum_with(units, "_unit_grenade_throw_idle ="),
        enum_with(network, "EMPTY_INVENTORY_TICKS ="),
        structure(network, "distributed_inventory"),
    ]) + "\n"
    code = "\n".join([
        declaration(network, "static struct distributed_own_inventory\n",
                    "objects_client_own_inventories[MAXIMUM_LOCAL_PLAYERS];"),
        declaration(network, "static struct distributed_retried_inventory\n",
                    "static short objects_client_retried_inventory_count;"),
        function(network, "distributed_client_weapon_here"),
        function(network, "distributed_client_inventory_whole"),
        function(network, "distributed_client_retry_inventory"),
        function(network, "distributed_client_forget_retried_inventory"),
        function(network, "distributed_client_apply_inventory"),
        function(network, "distributed_client_apply_retried_inventories"),
    ])
    if fault:
        before, after = fault
        assert code.count(before) == 1, "negative control no longer applies: %r" % before
        code = code.replace(before, after, 1)
    (directory / "config.inc").write_text(config)
    (directory / "under_test.inc").write_text(code + "\n")
    binary = directory / "retried_inventories"
    subprocess.run([CC, "-std=gnu99", "-w", f"-I{directory}", str(FIXTURE), "-o", str(binary), "-lm"], check=True)
    return binary


def run(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


@pytest.mark.parametrize("case", CASES)
def test_case(tmp_path, case):
    status, output = run(build(tmp_path), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(tmp_path, before, after, case):
    status, output = run(build(tmp_path, (before, after)), case)
    assert status == CHECK_FAILED, output


def test_wired():
    # Each client tick applies the kept ones; a new game keeps none.
    network = read("port/linux/game/network_objects.c")
    assert "distributed_client_apply_retried_inventories();" in function(network, "network_objects_client_tick")
    assert "objects_client_retried_inventory_count = 0;" in function(network, "network_objects_new_game")
