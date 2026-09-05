#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode the raw 512x512 GPU attachment and check an independent math oracle.

PNG pixels come exclusively from hardware. The CPU reference is used only for
validation. This carrier's clear color is fixed; it does not honor glClearColor.
"""
import argparse
import hashlib
import json
import math
import struct
import zlib
from pathlib import Path


def smooth(a, b, x):
    t = max(0, min(1, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    return [x + (y - x) * t for x, y in zip(a, b)]


def shade(p):
    u, v = p[2] - p[1], p[0]
    edge = min(p)
    bend = 3 * u * (1 - u * u) + v * v
    wave = math.sin(27 * v + 12 * u + 2.8 * bend)
    tide = .5 + .5 * wave
    fire = smooth(-.4, .75, u + .3 * bend)
    cold = mix((.015, .028, .12), (.02, .72, .62), tide * tide)
    warm = mix((.18, .018, .14), (1, .34, .075), tide)
    pigment = mix(cold, warm, fire)
    thread = 1 - smooth(.015, .11, abs(wave))
    phase = 22 * edge + .23 * bend
    contour = phase - math.floor(phase)
    etching = 1 - smooth(.008, .045, min(contour, 1 - contour))
    inset = smooth(.018, .045, edge)
    pigment = [x * (.3 + .7 * inset) + thread * inset * y + etching * inset * z
               for x, y, z in zip(pigment, (.32, .58, .52), (.12, .15, .23))]
    rim = 1 - smooth(.0025, .008, edge)
    inner = 1 - smooth(.0015, .004, abs(edge - .023))
    color = mix((.2, .95, .95), (1, .65, .23), fire)
    return [round(255 * max(0, min(1, x + (rim + .55 * inner) * y)))
            for x, y in zip(pigment, color)] + [255]


def chunk(kind, data):
    return (struct.pack('>I', len(data)) + kind + data +
            struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('attachment', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
raw = args.attachment.read_bytes()
size = 512
if len(raw) != size * size * 4:
    parser.error('expected one uncompressed 512x512 BGRA8 attachment')
linear = bytearray(len(raw))
coverage_errors = color_errors = covered = max_error = 0
examples = []
ax, ay, bx, by, cx, cy = -.22, .88, -.82, -.72, .90, -.45
denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
for y in range(size):
    for x in range(size):
        morton = sum((((x >> bit) & 1) << (2 * bit)) |
                     (((y >> bit) & 1) << (2 * bit + 1)) for bit in range(6))
        offset = (((y // 64) * 8 + x // 64) * 4096 + morton) * 4
        bgra = raw[offset:offset + 4]
        rgba = bytes((bgra[2], bgra[1], bgra[0], bgra[3]))
        linear[(y * size + x) * 4:(y * size + x + 1) * 4] = rgba
        sx, sy = (x + .5) / 256 - 1, 1 - (y + .5) / 256
        a = ((by - cy) * (sx - cx) + (cx - bx) * (sy - cy)) / denominator
        b = ((cy - ay) * (sx - cx) + (ax - cx) * (sy - cy)) / denominator
        c = 1 - a - b
        inside = min(a, b, c) > 0
        actual_inside = bgra != bytes.fromhex('0f0504ff')
        coverage_errors += inside != actual_inside
        if not inside:
            continue
        covered += 1
        perspective = (a, b / 1.35, c / .85)
        total = sum(perspective)
        expected = shade([v / total for v in perspective])
        error = max(abs(v - w) for v, w in zip(rgba, expected))
        max_error = max(error, max_error)
        if error > 2:
            color_errors += 1
            if len(examples) < 8:
                examples.append(dict(x=x, y=y, actual=list(rgba), expected=expected))

report = dict(pixels=size * size, covered=covered, coverage_errors=coverage_errors,
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
assert coverage_errors == 0 and color_errors == 0
