"""DXT1 as ASTC 4x4 (port/linux/src/dxt1_astc.h, from upstream OpenCommunityEdition/OpenCE PR #456): the web build gives
a GPU with WEBGL_compressed_texture_astc and no S3TC (the Pixel's PowerVR) its DXT1 textures as ASTC blocks, the DXT1
bits moved rather than re-encoded. Here each converted block is decoded on the CPU by an ASTC decoder written from the
Khronos Data Format Specification's ASTC chapter (block mode, integer sequence encoding, endpoint modes 8 and 12, dual
planes) and compared texel by texel with the game's own DXT1 decode (xbox_textures.c's dxt_color_block, which the CPU
fallback draws): colors within the 64ths ASTC weights are in, alpha exact, and a transparent texel transparent."""

import random
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TEXTURES = ROOT / "port/linux/src/xbox_textures.c"
HEADER_DIRECTORY = ROOT / "port/linux/src"
FIXTURE = ROOT / "tools/fixtures/dxt1_astc.c"
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

pytestmark = pytest.mark.skipif(CC is None, reason="no C compiler")

# the integer sequence encoding's ranges: (largest value, trits, quints, bits)
RANGES = [(1, 0, 0, 1), (2, 1, 0, 0), (3, 0, 0, 2), (4, 0, 1, 0), (5, 1, 0, 1), (7, 0, 0, 3), (9, 0, 1, 1),
          (11, 1, 0, 2), (15, 0, 0, 4), (19, 0, 1, 2), (23, 1, 0, 3), (31, 0, 0, 5), (39, 0, 1, 3), (47, 1, 0, 4),
          (63, 0, 0, 6), (79, 0, 1, 4), (95, 1, 0, 5), (127, 0, 0, 7), (159, 0, 1, 5), (191, 1, 0, 6), (255, 0, 0, 8)]
WEIGHT_RANGES = {(0, 2): 1, (0, 3): 2, (0, 4): 3, (0, 5): 4, (0, 6): 5, (0, 7): 7,
                 (1, 2): 9, (1, 3): 11, (1, 4): 15, (1, 5): 19, (1, 6): 23, (1, 7): 31}


def range_of(largest):
    return next(r for r in RANGES if r[0] == largest)


def ise_bits(count, largest):
    _, trits, quints, bits = range_of(largest)
    return count * bits + (-(-8 * count // 5) if trits else 0) + (-(-7 * count // 3) if quints else 0)


def unpack_trits(t):
    if (t >> 2) & 7 == 7:
        c = (((t >> 5) & 7) << 2) | (t & 3)
        t4, t3 = 2, 2
    else:
        c = t & 0x1f
        if (t >> 5) & 3 == 3:
            t4, t3 = 2, (t >> 7) & 1
        else:
            t4, t3 = (t >> 7) & 1, (t >> 5) & 3
    if c & 3 == 3:
        t2, t1, t0 = 2, (c >> 4) & 1, (((c >> 3) & 1) << 1) | ((c >> 2) & 1 & ~(c >> 3) & 1)
    elif (c >> 2) & 3 == 3:
        t2, t1, t0 = 2, 2, c & 3
    else:
        t2, t1, t0 = (c >> 4) & 1, (c >> 2) & 3, (((c >> 1) & 1) << 1) | (c & 1 & ~(c >> 1) & 1)
    return [t0, t1, t2, t3, t4]


def ise_decode(bits, start, count, largest):
    """count values of the range from the bit list (beyond its end: zeros)"""
    _, trits, quints, m = range_of(largest)
    assert not quints, "no quints in these blocks"
    position = start

    def take(n):
        nonlocal position
        value = 0
        for i in range(n):
            if position + i < len(bits):
                value |= bits[position + i] << i
        position += n
        return value

    if not trits:
        return [take(m) for _ in range(count)]
    values = []
    for _ in range(0, count, 5):
        low, t = [], 0
        for i, tbits in enumerate([2, 2, 1, 2, 1]):
            low.append(take(m))
            t |= take(tbits) << [0, 2, 4, 5, 7][i]
        values += [(trit << m) | low[i] for i, trit in enumerate(unpack_trits(t))]
    return values[:count]


def unquantize_weight(value, largest):
    _, trits, _, m = range_of(largest)
    if not trits:
        w, filled = 0, 0
        while filled < 6:  # bit replication to 6 bits
            w = (w << m) | value
            filled += m
        w >>= filled - 6
    else:
        assert m == 0, "only bare trits in these blocks"
        w = [0, 32, 63][value]
    return w + 1 if w > 32 else w


def unquantize_endpoint(value, largest):
    _, trits, quints, m = range_of(largest)
    assert not trits and not quints, "endpoints of plain bits in these blocks"
    e, filled = 0, 0
    while filled < 8:
        e = (e << m) | value
        filled += m
    return e >> (filled - 8)


def blue_contract(r, g, b, a):
    return ((r + b) >> 1, (g + b) >> 1, b, a)


def astc_decode(block):
    """an ASTC 4x4 LDR block's 16 RGBA texels (8 bits each); only what a single-partition block with direct RGB(A)
    endpoints needs is decoded, and anything else fails the test"""
    bits = [(block[i >> 3] >> (i & 7)) & 1 for i in range(128)]
    field = lambda start, count: sum(bits[start + i] << i for i in range(count))
    mode = field(0, 11)
    assert mode & 0x1ff != 0x1fc, "void extent"
    assert mode & 3, "block mode layout not used here"
    r = ((mode & 3) << 1) | ((mode >> 4) & 1)
    a, b = (mode >> 5) & 3, (mode >> 7) & 3
    layout = (mode >> 2) & 3
    if layout == 0:
        width, height = b + 4, a + 2
    elif layout == 1:
        width, height = b + 8, a + 2
    elif layout == 2:
        width, height = a + 2, b + 8
    elif (mode >> 8) & 1:
        width, height = ((mode >> 7) & 1) + 2, a + 2
    else:
        width, height = a + 2, ((mode >> 7) & 1) + 6
    high, dual = (mode >> 9) & 1, (mode >> 10) & 1
    assert (width, height) == (4, 4), "a 4x4 weight grid (no infill)"
    weight_largest = WEIGHT_RANGES[(high, r)]
    planes = 2 if dual else 1
    weight_count = 16 * planes
    weight_bits = ise_bits(weight_count, weight_largest)
    assert 24 <= weight_bits <= 96, "an illegal weight bit count"
    assert field(11, 2) == 0, "one partition"
    cem = field(13, 4)
    assert cem in (8, 12), f"endpoint mode {cem}"
    value_count = 6 if cem == 8 else 8
    endpoint_end = 128 - weight_bits - (2 if dual else 0)
    available = endpoint_end - 17
    endpoint_largest = next(largest for largest, *_ in reversed(RANGES) if ise_bits(value_count, largest) <= available)
    assert endpoint_largest >= 5
    selector = field(endpoint_end, 2) if dual else None
    v = [unquantize_endpoint(x, endpoint_largest)
         for x in ise_decode(bits[:endpoint_end], 17, value_count, endpoint_largest)]
    if cem == 8:
        v += [255, 255]
    first, second = (v[0], v[2], v[4], v[6]), (v[1], v[3], v[5], v[7])
    if v[1] + v[3] + v[5] < v[0] + v[2] + v[4]:
        first, second = blue_contract(*second), blue_contract(*first)
    weights = [unquantize_weight(x, weight_largest)
               for x in ise_decode(list(reversed(bits)), 0, weight_count, weight_largest)]
    texels = []
    for texel in range(16):
        color = []
        for channel in range(4):
            w = weights[texel * planes + (1 if dual and channel == selector else 0)]
            c0, c1 = first[channel] * 257, second[channel] * 257
            c = (c0 * (64 - w) + c1 * w + 32) >> 6
            color.append(round(c * 255 / 65535))
        texels.append(tuple(color))
    return texels, {"dual": dual, "endpoint_largest": endpoint_largest, "weight_largest": weight_largest}


def dxt_block(c0, c1, indices):
    return struct.pack("<HHI", c0, c1, indices)


def cases():
    rng = random.Random(456)
    blocks = []
    # the edges: equal colors, black and white, every index, all transparent
    for c0, c1 in [(0, 0), (0xffff, 0), (0, 0xffff), (0xffff, 0xffff), (0xf800, 0x07e0), (0x07e0, 0xf800),
                   (0x001f, 0xf800), (0x0821, 0x0820), (0x0820, 0x0821), (0x1234, 0x1234)]:
        for indices in [0, 0x55555555, 0xaaaaaaaa, 0xffffffff, 0xe4e4e4e4, 0x1b1b1b1b, 0x000000ff, 0xc0000000]:
            blocks.append(dxt_block(c0, c1, indices))
    for _ in range(6000):
        c0, c1 = rng.randrange(65536), rng.randrange(65536)
        if rng.random() < 0.15:  # three colors with sums close together (blue contraction)
            c1 = c0 ^ rng.choice([1, 0x20, 0x800, 0x21, 0x801])
        blocks.append(dxt_block(c0, c1, rng.getrandbits(32)))
    return blocks


@pytest.fixture(scope="module")
def converted(tmp_path_factory):
    directory = tmp_path_factory.mktemp("dxt1_astc")
    source = TEXTURES.read_text(encoding="latin-1")
    parts = re.findall(r"^static unsigned long expand[56]\(.*$", source, re.M)
    for name in ["argb", "color565", "mix", "dxt_color_block"]:
        match = re.search(rf"^static \w+(?: \w+)* {name}\([^)]*\)\n\{{\n.*?^\}}\n", source, re.M | re.S)
        assert match, name
        parts.append(match.group(0))
    (directory / "dxt_decode.inc").write_text("\n".join(parts) + "\n")
    binary = directory / "dxt1_astc"
    subprocess.run([CC, "-std=gnu89", "-O1", "-w", f"-I{directory}", f"-I{HEADER_DIRECTORY}", str(FIXTURE), "-o",
                    str(binary)], check=True)
    blocks = cases()
    output = subprocess.run([str(binary)], input=b"".join(blocks), capture_output=True, check=True).stdout
    assert len(output) == len(blocks) * 80
    return [(blocks[i], output[i * 80:i * 80 + 16], output[i * 80 + 16:i * 80 + 80]) for i in range(len(blocks))]


def test_every_block_decodes_to_the_dxt1_decode(converted):
    kinds = {"four": 0, "three": 0, "transparent": 0}
    worst = 0
    for dxt, astc, game in converted:
        c0, c1, indices = struct.unpack("<HHI", dxt)
        four = c0 > c1
        transparent = not four and any((indices >> (2 * t)) & 3 == 3 for t in range(16))
        texels, info = astc_decode(astc)
        assert info["dual"] == transparent
        assert info["endpoint_largest"] == (127 if transparent else 255)
        assert info["weight_largest"] == (3 if four else 2)
        kinds["four" if four else "transparent" if transparent else "three"] += 1
        for t in range(16):
            b, g, r, a = game[t * 4:t * 4 + 4]
            got = texels[t]
            index = (indices >> (2 * t)) & 3
            if not four and index == 3:
                assert a == 0 and got[3] == 0, (dxt.hex(), t, got)
                continue
            assert a == 255 and got[3] == 255, (dxt.hex(), t, got)
            # four colors: ASTC's thirds are 21 and 43 sixty-fourths (at most 255/192 off, and the game's
            # division truncates); 7-bit endpoints are at most 1 off; three colors are otherwise exact but for
            # rounding
            limit = 3 if four or transparent else 1
            difference = max(abs(got[0] - r), abs(got[1] - g), abs(got[2] - b))
            worst = max(worst, difference)
            assert difference <= limit, (dxt.hex(), t, got, (r, g, b))
    assert all(kinds.values()), kinds


def test_the_end_colors_are_exact(converted):
    """a texel at c0 or c1 of an opaque block is the DXT1 color exactly"""
    for dxt, astc, game in converted:
        c0, c1, indices = struct.unpack("<HHI", dxt)
        if not c0 > c1 and any((indices >> (2 * t)) & 3 == 3 for t in range(16)):
            continue
        texels, _ = astc_decode(astc)
        for t in range(16):
            if (indices >> (2 * t)) & 3 in (0, 1):
                b, g, r, _ = game[t * 4:t * 4 + 4]
                assert texels[t][:3] == (r, g, b), (dxt.hex(), t)
