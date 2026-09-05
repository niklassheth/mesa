#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check every requested frame and primitive-order invariance."""
import json,os,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);mode=sys.argv[2];scene=Path(__file__).resolve().parent
states=bool(os.getenv('T8132_GLES_DEPTH_STATES'))
frames=int(os.getenv('T8132_GLES_FRAMES', '10' if states else '28' if mode=='cube' else '2'))
assert len(list(out.glob('render-*-attachment-0.bin')))==frames
reports=[]
for frame in range(frames):
 args=[sys.executable,str(scene/'readback.py'),str(out/f'render-{frame:04d}-attachment-0.bin'),
       '--mode',mode,'--frame',str(frame),'--output',str(out/f'frame{frame:02d}')]
 if mode!='quad':args+=['--depth-attachment',str(out/f'render-{frame:04d}-attachment-1.bin')]
 if states:args+=['--depth-states']
 if os.getenv('T8132_GLES_FOUR_BUFFERS'):args+=['--four-buffers']
 result=subprocess.run(args,capture_output=True,text=True)
 if result.returncode:
  print(result.stdout);print(result.stderr,file=sys.stderr);result.check_returncode()
 report=json.loads(result.stdout);reports.append(report)
 if frame%2 and (not states or frame//2 not in (2,3)):
  assert report['attachment_sha256']==reports[frame-1]['attachment_sha256'], 'primitive order changed color'
  if mode!='quad':
   assert (out/f'render-{frame:04d}-attachment-1.bin').read_bytes()==(out/f'render-{frame-1:04d}-attachment-1.bin').read_bytes(), 'primitive order changed depth'
summary=dict(mode=mode,frames=frames,vertices=reports[0]['vertices'],triangles=reports[0]['triangles'],
             max_color_error=max(r['max_interior_channel_error'] for r in reports),
             max_depth_error=max(r['max_depth_error'] for r in reports),
             boundary_differences=sum(r['boundary_differences'] for r in reports),reports=reports)
(out/'validation.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='reports'}))
