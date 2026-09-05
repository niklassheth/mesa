#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble validated GPU frame PNGs into a lossless APNG (requires Pillow)."""
import json,sys
from pathlib import Path
from PIL import Image
out=Path(sys.argv[1]);report=json.loads((out/'validation.json').read_text())
assert report['mode']=='cube'
frames=[Image.open(out/f'frame{i:02d}.png').convert('RGBA') for i in range(0,report['frames'],2)]
frames[0].save(out/'rotation.png',save_all=True,append_images=frames[1:],
               duration=160,loop=0,disposal=0,blend=0)
# APNG must preserve every source pixel; no palette quantization.
with Image.open(out/'rotation.png') as animation:
 assert animation.n_frames==len(frames)
 for i,frame in enumerate(frames):
  animation.seek(i)
  assert animation.convert('RGBA').tobytes()==frame.tobytes()
print(out/'rotation.png')
