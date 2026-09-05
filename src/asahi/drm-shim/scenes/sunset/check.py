#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CPU raster/fragment reference for the sunset demo; images are GPU readbacks.

Input geometry/uniforms are recorded by the fixture before submission. Coverage,
depth and fragment math are evaluated independently here. Boundary and depth-tie
rules match the original island checker. Requires NumPy and Pillow.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import numpy as np
from PIL import Image
SIZE = 512
CLEAR = np.array([4,5,15,255],dtype=np.uint8)
DEPTH_TOLERANCE = 3e-6

def shade(data, light):
    world, normal, material = data[...,:3], data[...,3:6], data[...,6:10]
    def normalize(x):
        return x / np.sqrt(np.maximum(np.sum(x*x,axis=-1,keepdims=True),1e-30))
    def dot(x,y):
        return np.sum(x*y,axis=-1,keepdims=True)
    n = normalize(normal)
    l = light[:3]
    view = normalize(light[4:7]-world)
    halfway = normalize(l+view)
    spec = np.maximum(dot(n,halfway),0)**16
    t = n[...,1:2]*.5+.5
    ambient = np.array([.13,.055,.055])*(1-t)+np.array([.10,.17,.29])*t
    lit = material[...,:3]*(ambient+np.array([1,.46,.20])*np.maximum(dot(n,l),0)*.85)
    lit += np.array([1,.65,.32])*spec*np.clip(material[...,3:4],0,1)*1.4
    fog = np.clip(np.linalg.norm(light[4:7]-world,axis=-1,keepdims=True)*.08
        +np.maximum(-world[...,1:2]-.1,0)*.28-.17,0,.65)
    lit = lit*(1-fog)+np.array([.43,.20,.25])*fog
    height = np.clip(normal[...,1:2]*.5+.5,0,1)
    sky = np.array([.8,.30,.13])*(1-height)+np.array([.065,.055,.18])*height
    delta = normal[...,:2]-np.array([-.52,.65])
    r2 = dot(delta,delta)
    sky += np.array([1,.55,.18])*(.025/(r2+.035))
    sky += np.array([1,.8,.45])*np.clip((.009-r2)*500,0,1)
    t = np.clip(-material[...,3:4],0,1)
    return np.sqrt(np.clip(lit*(1-t)+sky*t,0,1))

def clip_triangles(vertices, colors, indices):
    # Clip before subpixel snapping. In particular, the water base extends
    # beyond the viewport; snapping its original off-screen corners gives
    # different interpolation planes from snapping the clipped polygon.
    result_v, result_c, result_i = [], [], []
    for ids in indices:
        # Preserve the FP32 shader outputs, but evaluate the mathematical
        # clip intersections in double precision. Repeated FP32 clipping of
        # a large triangle crossing W=0 amplified cancellation into a false
        # depth-plane offset at 1024 pixels.
        polygon = [np.r_[vertices[i], colors[i]].astype(float) for i in ids]
        for axis, sign in [(0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)]:
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
    colors = data
    transformed, colors, indices = clip_triangles(transformed, colors, indices)
    reciprocal_w = 1/transformed[:,3]
    colors = colors * reciprocal_w[:,None]
    transformed = transformed * reciprocal_w[:,None]
    points = np.column_stack(((transformed[:, 0]+np.float32(1))*(SIZE/2),
                              (np.float32(1)-transformed[:, 1])*(SIZE/2),
                              (transformed[:, 2]+np.float32(1))*.5)).astype(float)
    assert np.all((points[:, 2] >= -1e-6) & (points[:, 2] <= 1+1e-6))
    points[:, :2] = np.floor(points[:, :2]*256+.5)/256
    rgba = np.broadcast_to(CLEAR, (SIZE, SIZE, 4)).copy()
    zbuf = np.ones((SIZE, SIZE))
    depth_bounds = np.full((SIZE, SIZE), DEPTH_TOLERANCE)
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
        # Perspective projection can place a vertex on either side of a
        # 1/256-pixel rounding boundary. Bound the resulting depth change
        # using this triangle's screen-space gradient, not a global epsilon.
        dzdx = ((by-cy)*(az-cz)+(cy-ay)*(bz-cz))/d
        dzdy = ((cx-bx)*(az-cz)+(ax-cx)*(bz-cz))/d
        bound = DEPTH_TOLERANCE + (abs(dzdx)+abs(dzdy))/256
        depth_bounds[y0:y1,x0:x1][mask] = bound
        interpolated = np.einsum('ihw,ic->hwc', weights, colors[ids])
        interpolated /= np.einsum('ihw,i->hw',weights,reciprocal_w[ids])[...,None]
        color = shade(interpolated, light)
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
    return rgba,zbuf,boundary,candidates,depth_bounds


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
    p.add_argument('asset',type=Path)
    p.add_argument('output',type=Path)
    a = p.parse_args()
    global SIZE
    width,height = map(int,(a.output/'dimensions.txt').read_text().split())
    assert width == height and width % 64 == 0
    SIZE = width
    meta = json.loads((a.asset/'asset.json').read_text())
    vertices = meta['vertices']+3
    raw = (a.asset/'mesh.bin').read_bytes()
    indices = np.frombuffer(raw,'<u4',meta['triangles']*3,16+meta['vertices']*36)
    indices = np.r_[indices,np.arange(meta['vertices'],vertices)].reshape(-1,3)
    records = np.fromfile(a.output/'frames.bin',dtype='<f4').reshape(-1,23+vertices*10)
    assert len(list(a.output.glob('render-*-attachment-0.bin'))) == len(records)
    reports = []
    for frame,record in enumerate(records):
        actual = detile(a.output/f'render-{frame:04d}-attachment-0.bin')
        depth = detile(a.output/f'render-{frame:04d}-attachment-1.bin',True)
        data = record[23:].reshape(-1,10)
        ref,z,boundary,candidates,depth_bounds = raster(data,indices if frame%2==0 else indices[::-1],record[:16].reshape(4,4).T,record[16:23],(actual,depth))
        delta = np.abs(actual.astype(int)-ref.astype(int)).max(2)
        dz = np.abs(depth-z)
        bad = (delta>1)&~boundary&~candidates
        badz = (dz>depth_bounds)&~boundary
        report = dict(frame=frame,color_errors=int(bad.sum()),depth_errors=int(badz.sum()),
            max_color_error=int(delta[~boundary&~((delta>1)&candidates)].max(initial=0)),
            max_depth_error=float(dz[~boundary].max(initial=0)),
            depth_subpixel_pixels=int(((dz>DEPTH_TOLERANCE)&~boundary&~badz).sum()),
            max_depth_bound=float(depth_bounds[~boundary].max(initial=0)),
            boundary_pixels=int(boundary.sum()),depth_tie_pixels=int(((delta>1)&candidates&~boundary).sum()),
            sha256=hashlib.sha256(actual.tobytes()).hexdigest())
        if frame%2:
            report['order_color_differences']=int(np.any(actual!=previous,axis=2).sum())
            report['order_depth_differences']=int(np.count_nonzero(depth!=previous_depth))
        previous,previous_depth=actual,depth
        Image.fromarray(actual).save(a.output/f'frame{frame:02d}.png')
        Image.fromarray(ref).save(a.output/f'reference-{frame:02d}.png')
        reports.append(report)
        print(json.dumps(report),flush=True)
    success = all(not r['color_errors'] and not r['depth_errors'] and not r.get('order_depth_differences',0) for r in reports)
    (a.output/'validation.json').write_text(json.dumps(dict(success=success,frames=len(records),reports=reports),indent=2)+'\n')
    if not success: raise SystemExit('sunset validation failed')

if __name__ == '__main__':
    main()
