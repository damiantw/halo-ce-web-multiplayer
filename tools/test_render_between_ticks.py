"""Rendering between ticks, ported from upstream OpenCommunityEdition/OpenCE pull requests that
reached main in #562. This fork has no tools/harness, so each test compiles the real function
(taken from the source) with the stubs it needs.

The cases are the ones upstream's harness checks, cut down to the functions as they are here:
a scope quad covers its viewport (#536), a z bias's slope follows the target's scale (#540, the
slope only), a lens flare reads its own occlusion slot (#541), a shadow center moves with the
drawn object (#542), contrail points take the ticks' own time (#546), and a lightning bolt keeps
its shape through a tick (#552).
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CHECK_FAILED = 1
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

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


def read(path):
    return (ROOT / path).read_text(encoding="latin-1")


def build(directory, fixture, code, config=""):
    (directory / "under_test.inc").write_text(code + "\n")
    (directory / "config.inc").write_text(config)
    binary = directory / "render_between_ticks"
    subprocess.run(
        [CC, "-std=gnu89", "-w", f"-I{directory}", str(ROOT / "tools/fixtures" / fixture), "-lm",
         "-o", str(binary)],
        check=True,
    )
    return binary


def run(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def mutated(code, before, after):
    assert code.count(before) == 1, "negative control no longer applies: %r" % before
    return code.replace(before, after, 1)


SCREEN = "source/rasterizer/xbox/rasterizer_xbox_screen_effect.c"
CONTRAIL = "source/effects/contrails.c"
LIGHTNING = "source/objects/widgets/lightning.c"
SHADOW = "port/linux/game/render_interpolation.c"
ZBIAS = "port/linux/src/d3d8_gl.c"
FLARE = "source/rasterizer/rasterizer_lights.c"


def screen_config(source):
    match = re.search(r"_rasterizer_target_screen_effect\s*=\s*(\d+)", source)
    assert match
    return "enum { _rasterizer_target_screen_effect = %s };\n" % match.group(1)


def flare_config(source):
    enum = re.search(r"enum\s*\{[^}]*LENS_FLARE_OCCLUSION_SLOT_COUNT[^}]*\};", source, re.S)
    struct = re.search(r"struct lens_flare_occlusion_slot\s*\{.*?\n\};", source, re.S)
    assert enum and struct
    return enum.group(0) + "\n" + struct.group(0) + "\n"


@pytest.mark.parametrize("case", ["covers-the-screen", "covers-at-the-xbox-size", "split-screen"])
def test_screen_effect_cover(tmp_path, case):
    source = read(SCREEN)
    status, output = run(build(tmp_path, "screen_effect_cover.c", function(source, "rasterizer_screen_effect_cover_target"),
                                screen_config(source)), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", [
    ("half_pixel_x = 1.0f / (real)viewport_width;", "half_pixel_x = 0.0f;", "covers-the-screen"),
    ("return 0.5f;", "return 0.0f;", "covers-the-screen"),
])
def test_screen_effect_cover_fault(tmp_path, before, after, case):
    source = read(SCREEN)
    code = mutated(function(source, "rasterizer_screen_effect_cover_target"), before, after)
    status, output = run(build(tmp_path, "screen_effect_cover.c", code, screen_config(source)), case)
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", ["xbox-size", "scaled", "wider-axis"])
def test_zbias_slope(tmp_path, case):
    status, output = run(build(tmp_path, "zbias_slope.c", function(read(ZBIAS), "zbias_slope")), case)
    assert status == 0, output


def test_zbias_slope_fault(tmp_path):
    code = mutated(function(read(ZBIAS), "zbias_slope"), "return slope * scale;", "return slope;")
    status, output = run(build(tmp_path, "zbias_slope.c", code), "scaled")
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", ["one-tick", "same-tick", "two-ticks", "jump", "backwards", "double-speed"])
def test_contrail_point_dt(tmp_path, case):
    status, output = run(build(tmp_path, "contrail_point_dt.c", function(read(CONTRAIL), "contrail_point_dt")), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", [
    ("point_dt *= 0.5f;", "point_dt *= 1.0f;", "double-speed"),
    ("ticks = 1;", "ticks = 100;", "jump"),
])
def test_contrail_point_dt_fault(tmp_path, before, after, case):
    code = mutated(function(read(CONTRAIL), "contrail_point_dt"), before, after)
    status, output = run(build(tmp_path, "contrail_point_dt.c", code), case)
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", ["same-tick", "next-tick", "out-of-range"])
def test_lightning_tick_seed(tmp_path, case):
    source = read(LIGHTNING)
    assert "LIGHTNING_TICK_SEED_LIGHTNINGS = 256" in source
    assert "LIGHTNING_TICK_SEED_WINDOWS = 4" in source
    status, output = run(build(tmp_path, "lightning_tick_seed.c", function(source, "lightning_tick_seed")), case)
    assert status == 0, output


def test_lightning_tick_seed_fault(tmp_path):
    code = mutated(function(read(LIGHTNING), "lightning_tick_seed"),
                   "tick == game_time_get() + 1", "tick == game_time_get() + 2")
    status, output = run(build(tmp_path, "lightning_tick_seed.c", code), "same-tick")
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", ["centered-where-drawn", "at-the-tick"])
def test_shadow_center(tmp_path, case):
    status, output = run(build(tmp_path, "shadow_center.c", function(read(SHADOW), "render_interpolation_object_point")), case)
    assert status == 0, output


def test_shadow_center_fault(tmp_path):
    code = mutated(
        function(read(SHADOW), "render_interpolation_object_point"),
        "point->x += drawn[0].position.x - simulated[0].position.x;\n"
        "\tpoint->y += drawn[0].position.y - simulated[0].position.y;\n"
        "\tpoint->z += drawn[0].position.z - simulated[0].position.z;",
        "")
    status, output = run(build(tmp_path, "shadow_center.c", code), "centered-where-drawn")
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", ["same-slot", "not-until-counted", "no-slot", "occlusion-off"])
def test_lens_flare_occlusion(tmp_path, case):
    source = read(FLARE)
    code = function(source, "lens_flare_occlusion_test_index") + "\n" + function(source, "lens_flare_occlusion_visible_pixels")
    status, output = run(build(tmp_path, "lens_flare_occlusion.c", code, flare_config(source)), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", [
    ("xgpu_visibility_test_counted((unsigned long)test_index) < lens_flare_occlusion_slots[test_index].first_test",
     "0", "not-until-counted"),
    ("if (slot_index == 0)\n\t\t\tcontinue;", "", "no-slot"),
])
def test_lens_flare_occlusion_fault(tmp_path, before, after, case):
    source = read(FLARE)
    code = function(source, "lens_flare_occlusion_test_index") + "\n" + function(source, "lens_flare_occlusion_visible_pixels")
    status, output = run(build(tmp_path, "lens_flare_occlusion.c", mutated(code, before, after), flare_config(source)), case)
    assert status == CHECK_FAILED, output
