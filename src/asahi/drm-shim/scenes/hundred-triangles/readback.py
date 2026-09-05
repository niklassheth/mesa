#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check 100 distinct primitives from one 300-vertex draw against a CPU pixel oracle.

The PNG uses only detiled hardware bytes. The reference checks the complete
coverage mask and each triangle's independently interpolated vertex colors.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

CLEAR = bytes((4, 5, 15, 255))  # Current carrier's fixed clear, in RGBA order.


def reference(x, y):
    # Work in framebuffer coordinates, independently of the GLSL ID arithmetic.
    column, row = (x - 16) // 48, (y - 16) // 48
    if not (0 <= column < 10 and 0 <= row < 10):
        return None, CLEAR
    px = x + .5 - (16.25 + 48 * column)
    py = y + .5 - (16 + 48 * row)
    apex = (40 - py) / 34
    right = (px - 6 - 18 * apex) / 36
    left = 1 - apex - right
    if min(left, right, apex) <= 0:
        return None, CLEAR
    color = [round(255 * (column + 1) / 12),
             round(255 * (row + 1) / 12),
             round(255 * (.2 * left + .5 * right + .8 * apex)), 255]
    return row * 10 + column, color


def chunk(kind, body):
    return (struct.pack('>I', len(body)) + kind + body +
            struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('attachment', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
raw = args.attachment.read_bytes()
size = 512
if len(raw) != size * size * 4:
    parser.error('expected an uncompressed 512x512 BGRA8 attachment')
linear = bytearray(len(raw))
covered = [0] * 100
coverage_errors = color_errors = max_error = 0
examples = []
for y in range(size):
    for x in range(size):
        morton = sum((((x >> bit) & 1) << (2 * bit)) |
                     (((y >> bit) & 1) << (2 * bit + 1)) for bit in range(6))
        offset = (((y // 64) * 8 + x // 64) * 4096 + morton) * 4
        bgra = raw[offset:offset + 4]
        rgba = bytes((bgra[2], bgra[1], bgra[0], bgra[3]))
        linear[(y * size + x) * 4:(y * size + x + 1) * 4] = rgba
        primitive, expected = reference(x, y)
        if primitive is not None:
            covered[primitive] += 1
        coverage_errors += (primitive is not None) != (rgba != CLEAR)
        error = max(abs(a - b) for a, b in zip(rgba, expected))
        max_error = max(max_error, error)
        if error > 1:
            color_errors += 1
            if len(examples) < 8:
                examples.append(dict(x=x, y=y, primitive=primitive,
                                     actual=list(rgba), expected=list(expected)))

report = dict(vertices=300, triangles=100, draw_calls=1, pixels=size * size,
              covered_per_triangle=covered, coverage_errors=coverage_errors,
              color_errors=color_errors, max_channel_error=max_error,
              examples=examples, attachment_sha256=hashlib.sha256(raw).hexdigest())
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(
    b'\0' + linear[y * size * 4:(y + 1) * size * 4] for y in range(size))))
png += chunk(b'IEND', b'')
args.output.with_suffix('.png').write_bytes(png)
print(json.dumps(report))
assert all(covered) and coverage_errors == color_errors == 0
