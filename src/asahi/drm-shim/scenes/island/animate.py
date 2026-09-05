#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble checked hardware frames into a lossless looping APNG (Pillow)."""
import argparse
import json
from pathlib import Path
from PIL import Image

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--duration', type=int, default=100, help='milliseconds per view')
a = p.parse_args()
report = json.loads((a.output/'validation.json').read_text())
assert report['success'] and len(report['reports']) == report['frames']
assert report['frames'] % 2 == 0
frames = [Image.open(a.output/f'frame{i:02d}.png').convert('RGBA')
          for i in range(0,report['frames'],2)]
path = a.output/'island-orbit.png'
frames[0].save(path,save_all=True,append_images=frames[1:],
               duration=a.duration,loop=0,disposal=0,blend=0)
with Image.open(path) as animation:
    assert animation.n_frames == len(frames)
    for i,frame in enumerate(frames):
        animation.seek(i)
        assert animation.convert('RGBA').tobytes() == frame.tobytes()
print(path)
