#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent NumPy raster reference and hardware attachment validation.

PNG outputs for hardware checks contain only detiled GPU pixels. Reference
images are explicitly named reference-NN.png. Requires numpy and Pillow.
"""
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
import numpy as np
from PIL import Image

SIZE = 512
CLEAR = np.array([4, 5, 15, 255], dtype=np.uint8)
# The broad, clipped water plane changes depth by about 0.000642 per pixel.
# One 1/256-pixel subpixel step therefore corresponds to 2.51e-6 depth;
# allow that plus FP32 rounding. Keep observed errors in the report.
DEPTH_TOLERANCE = 3e-6


def read_mesh(path):
    raw = path.read_bytes()
    magic, vertices, count, reserved = struct.unpack_from('<4sIII', raw)
    assert magic == b'A9M1' and reserved == 0
    data = np.frombuffer(raw, '<f4', vertices * 9, 16).reshape(-1, 9)
    indices = np.frombuffer(raw, '<u4', count, 16 + vertices * 36).reshape(-1, 3)
    return data, indices


def camera(view, views):
    a = np.float32(.65) + np.float32(view) * (np.float32(2*math.pi) / np.float32(views))
    ca, sa = np.float32(math.cos(a)), np.float32(math.sin(a))
    ce, se = np.float32(math.cos(np.float32(.65))), np.float32(math.sin(np.float32(.65)))
    matrix = np.array([[ca, 0, -sa, 0], [-se*sa, ce, -se*ca, 0],
                       [-ce*sa*.25, -se*.25, -ce*ca*.25, 0], [0, 0, 0, 1]], dtype=np.float32)
    light = np.array([-.4, .8, .45, .28], dtype=np.float32)
    light[:3] /= np.sqrt(np.sum(light[:3]**2))
    return matrix, light


def clip_triangles(vertices, colors, indices):
    # Clip before subpixel snapping. In particular, the water base extends
    # beyond the viewport; snapping its original off-screen corners gives
    # different interpolation planes from snapping the clipped polygon.
    result_v, result_c, result_i = [], [], []
    for ids in indices:
        polygon = [np.r_[vertices[i], colors[i]].astype(np.float32) for i in ids]
        for axis, sign in [(0, 1), (0, -1), (1, 1), (1, -1)]:
            clipped = []
            for previous, current in zip(polygon[-1:] + polygon[:-1], polygon):
                before = previous[3] - sign * previous[axis]
                after = current[3] - sign * current[axis]
                if (before >= 0) != (after >= 0):
                    t = before / (before - after)
                    clipped.append(previous + (current - previous) * t)
                if after >= 0:
                    clipped.append(current)
            polygon = clipped
            if not polygon:
                break
        base = len(result_v)
        for vertex in polygon:
            result_v.append(vertex[:4])
            result_c.append(vertex[4:])
        for i in range(1, len(polygon) - 1):
            result_i.append([base, base + i, base + i + 1])
    return np.array(result_v), np.array(result_c), np.array(result_i)


def raster(data, indices, matrix, light, actual=None):
    # Emulate separately rounded FP32 multiply/add shader operations.
    pos = data[:, :3]
    transformed = np.empty((len(pos), 4), dtype=np.float32)
    for k in range(4):
        transformed[:, k] = ((pos[:, 0]*matrix[k, 0] + pos[:, 1]*matrix[k, 1]) + pos[:, 2]*matrix[k, 2]) + matrix[k, 3]
    dot = (data[:, 3]*light[0] + data[:, 4]*light[1]) + data[:, 5]*light[2]
    colors = np.sqrt(data[:, 6:9] * (light[3] + (np.float32(1)-light[3])*np.maximum(dot, 0))[:, None])
    assert np.all(transformed[:, 3] == 1), 'reference currently supports orthographic projection'
    transformed, colors, indices = clip_triangles(transformed, colors, indices)
    points = np.column_stack(((transformed[:, 0]+np.float32(1))*256,
                              (np.float32(1)-transformed[:, 1])*256,
                              (transformed[:, 2]+np.float32(1))*.5)).astype(float)
    assert np.all((points[:, 2] >= 0) & (points[:, 2] <= 1)), 'depth clipping not implemented in reference'
    points[:, :2] = np.floor(points[:, :2]*256+.5)/256
    rgba = np.broadcast_to(CLEAR, (SIZE, SIZE, 4)).copy()
    zbuf = np.ones((SIZE, SIZE))
    boundary = np.zeros((SIZE, SIZE), dtype=bool)
    candidates = np.zeros((SIZE, SIZE), dtype=bool)
    for ids in indices:
        v = points[ids]
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = v
        d = (by-cy)*(ax-cx)+(cx-bx)*(ay-cy)
        if abs(d) < 1e-12:
            continue
        lo = np.maximum(np.floor(v[:, :2].min(0)).astype(int)-1, 0)
        hi = np.minimum(np.ceil(v[:, :2].max(0)).astype(int)+1, SIZE)
        if np.any(lo >= hi):
            continue
        x0,y0 = lo; x1,y1 = hi
        y,x = np.mgrid[y0:y1,x0:x1].astype(float) + .5
        u = ((by-cy)*(x-cx)+(cx-bx)*(y-cy))/d
        w = ((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/d
        q = 1-u-w
        weights = np.array([u,w,q])
        lengths = np.array([math.hypot(bx-cx,by-cy), math.hypot(cx-ax,cy-ay), math.hypot(ax-bx,ay-by)])
        distances = weights*abs(d)/lengths[:,None,None]
        # Exclude only a 1/64-pixel band around projected triangle edges.
        edge = (distances.min(0) >= -1/64) & (abs(distances).min(0) < 1/64)
        boundary[y0:y1,x0:x1] |= edge
        depth = u*az + w*bz + q*cz
        mask = (weights.min(0) >= 0) & (depth < zbuf[y0:y1,x0:x1])
        zbuf[y0:y1,x0:x1][mask] = depth[mask]
        color = np.einsum('ihw,ic->hwc', weights, colors[ids])
        quantized = np.rint(np.clip(color,0,1)*255).astype('u1')
        rgba[y0:y1,x0:x1,:3][mask] = quantized[mask]
        if actual is not None:
            # Intersecting/coplanar surfaces can exchange visibility within
            # depth interpolation precision. Accept only a real covering
            # triangle whose shader color matches and whose depth is within
            # the same depth bound used for the nearest-surface depth check.
            gpu_color, gpu_depth = actual
            matches = (abs(quantized.astype(int)-gpu_color[y0:y1,x0:x1,:3]).max(2) <= 1) & (gpu_color[y0:y1,x0:x1,3] == 255)
            candidates[y0:y1,x0:x1] |= ((weights.min(0) >= 0) & matches &
                                       (abs(depth-gpu_depth[y0:y1,x0:x1]) <= DEPTH_TOLERANCE))
    return rgba,zbuf,boundary,candidates


def detile(path, depth=False):
    raw = path.read_bytes()
    assert len(raw) == SIZE*SIZE*4
    y,x = np.mgrid[:SIZE,:SIZE]
    morton = np.zeros_like(x)
    for bit in range(6):
        morton |= ((x>>bit)&1) << (2*bit)
        morton |= ((y>>bit)&1) << (2*bit+1)
    offsets = ((y//64)*(SIZE//64)+x//64)*4096+morton
    if depth:
        return np.frombuffer(raw,'<f4')[offsets]
    return np.frombuffer(raw,'u1').reshape(-1,4)[offsets][:,:,[2,1,0,3]]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('asset', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--preview', action='store_true')
    p.add_argument('--views', type=int, default=48)
    p.add_argument('--frame', type=int, help='check only this frame')
    a = p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    data,indices = read_mesh(a.asset/'mesh.bin')
    if a.preview:
        matrix, light = camera(a.frame or 0,a.views)
        expected,_,_,_ = raster(data,indices,matrix,light)
        Image.fromarray(expected).save(a.output/'reference-preview.png')
        print(a.output/'reference-preview.png')
        return
    records = np.frombuffer((a.output/'frames.bin').read_bytes(),'<f4').reshape(-1,20)
    assert len(records) > 0 and len(records) % 2 == 0
    assert len(list(a.output.glob('render-*-attachment-0.bin'))) == len(records)
    assert len(list(a.output.glob('render-*-attachment-1.bin'))) == len(records)
    reports=[]
    for frame in ([a.frame] if a.frame is not None else range(len(records))):
        record = records[frame]
        colorpath = a.output/f'render-{frame:04d}-attachment-0.bin'
        depthpath = a.output/f'render-{frame:04d}-attachment-1.bin'
        actual = detile(colorpath)
        actual_depth = detile(depthpath,True)
        expected,depth,edge,candidates = raster(data,indices if frame%2==0 else indices[::-1],record[:16].reshape(4,4).T,record[16:],(actual,actual_depth))
        Image.fromarray(actual).save(a.output/f'frame{frame:02d}.png')
        Image.fromarray(expected).save(a.output/f'reference-{frame:02d}.png')
        delta = abs(actual.astype(int)-expected.astype(int)).max(2)
        dz = abs(actual_depth-depth)
        coverage = np.any(actual != CLEAR,axis=2)
        mismatch = coverage != np.any(expected != CLEAR,axis=2)
        direct_bad = (delta>1)&~edge
        bad = direct_bad & ~candidates
        report = dict(frame=frame,covered_pixels=int(coverage.sum()),
                      coverage_errors=int((mismatch&~edge).sum()),
                      color_errors=int(bad.sum()),max_color_error=int(delta[~edge].max()),
                      depth_tie_pixels=int((direct_bad & candidates).sum()),
                      max_unambiguous_color_error=int(delta[~edge & ~(direct_bad & candidates)].max()),
                      depth_errors=int(((~np.isfinite(actual_depth)|(dz>DEPTH_TOLERANCE))&~edge).sum()),
                      max_depth_error=float(dz[~edge].max()),
                      boundary_differences=int(((delta>1)|mismatch)[edge].sum()),
                      boundary_pixels=int(edge.sum()),
                      attachment_sha256=hashlib.sha256(colorpath.read_bytes()).hexdigest(),
                      examples=[dict(x=int(x),y=int(y),actual=actual[y,x].tolist(),expected=expected[y,x].tolist()) for y,x in np.argwhere(bad)[:8]])
        if frame%2:
            report['order_color_differences'] = int(np.any(actual != detile(a.output/f'render-{frame-1:04d}-attachment-0.bin'),axis=2).sum())
            report['order_depth_differences'] = int(np.count_nonzero(actual_depth != detile(a.output/f'render-{frame-1:04d}-attachment-1.bin',True)))
        reports.append(report)
        print(json.dumps(report),flush=True)
    success = all(r['coverage_errors']==r['color_errors']==r['depth_errors']==0
                  and r.get('order_depth_differences', 0)==0 for r in reports)
    report = dict(success=success,frames=len(records),vertices=len(data),triangles=len(indices),
                  color_tolerance=1,depth_tolerance=DEPTH_TOLERANCE,edge_band_pixels=1/64,
                  reports=reports)
    name = 'validation.json' if a.frame is None else f'validation-{a.frame:02d}.json'
    (a.output/name).write_text(json.dumps(report,indent=2)+'\n')
    if not success:
        raise SystemExit(1)


if __name__=='__main__':
    main()
