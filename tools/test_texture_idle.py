"""The texture cache (xbox_textures.c) drops a texture not drawn for half a minute of time, at any frame rate: a sweep
every 10 seconds of SDL_GetTicks drops what was not drawn since the sweep three sweeps before, so at 144 or 360 frames a
second a texture out of sight is kept 30 to 40 seconds, as it was at 60, instead of 1800 frames.

Ported from upstream OpenCommunityEdition/OpenCE PR #376 (tools/harness/tests/test_texture_idle.py). This fork has no
tools/harness, so the extraction is done here and the C side lives in tools/fixtures/texture_idle.c."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/src/xbox_textures.c"
FIXTURE = ROOT / "tools/fixtures/texture_idle.c"
CHECK_FAILED = 3
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

CASES = ["fast", "steady", "late"]

# a fault in xgpu_texture_cache_begin_frame (or its constants) and the case that must catch it
NEGATIVE_CONTROLS = {
    "a-sweep-every-frame": (("texture_next_sweep = now + TEXTURE_SWEEP_SECONDS * 1000;", "texture_next_sweep = now;"),
                            None, "fast"),
    "the-sweeps-own-frame": ((
        "\tidle_since = texture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT];\n"
        "\ttexture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;\n",
        "\ttexture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;\n"
        "\tidle_since = texture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT];\n"), None, "fast"),
    "sweeps-not-counted": (("\ttexture_sweep_count++;\n", "\n"), None, "late"),
    "a-minute-idle": (None, ("#define TEXTURE_IDLE_SECONDS 30", "#define TEXTURE_IDLE_SECONDS 60"), "steady"),
}

pytestmark = pytest.mark.skipif(CC is None, reason="no C compiler")


def function(source, name):
    match = re.search(rf"^void {name}\(void\)\n\{{\n.*?^\}}\n", source, re.M | re.S)
    assert match, name
    return match.group(0)


def mutated(text, fault):
    old, new = fault
    assert old in text, "negative control no longer applies to xbox_textures.c"
    return text.replace(old, new, 1)


def generated(directory, fault=None, config_fault=None):
    source = SOURCE.read_text(encoding="latin-1")
    config = "\n".join(re.findall(r"^#define TEXTURE_\w+.*$", source, re.M) + re.findall(
        r"^static (?:unsigned long|Uint64) texture_(?:frame|drop_serial|sweep_\w+|next_sweep)\b.*;$", source, re.M))
    code = function(source, "xgpu_texture_cache_begin_frame")
    if config_fault:
        config = mutated(config, config_fault)
    if fault:
        code = mutated(code, fault)
    (directory / "config.inc").write_text(config + "\n")
    (directory / "under_test.inc").write_text(code + "\n")
    binary = directory / "texture_idle"
    subprocess.run([CC, "-std=gnu99", "-w", "-I", str(directory), str(FIXTURE), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def run(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


@pytest.mark.parametrize("case", CASES)
def test_case(case, tmp_path):
    status, output = run(generated(tmp_path), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, tmp_path):
    fault, config_fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(generated(tmp_path, fault, config_fault), case)
    assert status == CHECK_FAILED, f"xbox_textures.c with a fault ({control}) passed '{case}': the test cannot see it"
