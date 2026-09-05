#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Flatten an untextured GLB scene into FP32 position/normal/color + u32 indices.

Requires numpy. The source asset stays caller-owned, outside the Mesa tree.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import numpy as np


def load_glb(path, excluded=()):
    raw = path.read_bytes()
    magic, version, length = struct.unpack_from('<4sII', raw)
    if magic != b'glTF' or version != 2 or length != len(raw):
        raise ValueError('expected a complete GLB 2 file')
    chunks = {}
    cursor = 12
    while cursor < length:
        size, kind = struct.unpack_from('<II', raw, cursor)
        cursor += 8
        if size % 4 or cursor + size > length:
            raise ValueError('invalid GLB chunk')
        chunks[kind] = raw[cursor:cursor + size]
        cursor += size
    doc = json.loads(chunks[0x4e4f534a])
    blob = chunks[0x004e4942]
    if doc.get('extensionsRequired') or doc.get('skins') or doc.get('animations'):
        raise ValueError('this static demo does not support required extensions, skins or animation')

    def accessor(index):
        a = doc['accessors'][index]
        if 'sparse' in a:
            raise ValueError('sparse accessors are unsupported')
        view = doc['bufferViews'][a['bufferView']]
        if view['buffer'] != 0:
            raise ValueError('external buffers are unsupported')
        dtype = np.dtype({5120: 'i1', 5121: 'u1', 5122: '<i2', 5123: '<u2',
                          5125: '<u4', 5126: '<f4'}[a['componentType']])
        components = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        stride = view.get('byteStride', components * dtype.itemsize)
        relative = a.get('byteOffset', 0)
        extent = (a['count'] - 1) * stride + components * dtype.itemsize
        offset = view.get('byteOffset', 0) + relative
        if a['count'] <= 0 or stride < components * dtype.itemsize or relative + extent > view['byteLength'] or offset + extent > len(blob):
            raise ValueError('accessor outside its buffer view')
        result = np.ndarray((a['count'], components), dtype=dtype, buffer=blob,
                            offset=offset, strides=(stride, dtype.itemsize)).copy()
        if a.get('normalized') and dtype.kind in 'iu':
            result = np.maximum(result.astype(float) / np.iinfo(dtype).max, -1)
        return result

    vertices, triangles, objects = [], [], []
    vertex_count = 0

    def visit(index, parent, ancestry):
        nonlocal vertex_count
        if index in ancestry:
            raise ValueError('cyclic node tree')
        node = doc['nodes'][index]
        if node.get('name') in excluded:
            return
        if 'matrix' in node:
            local = np.array(node['matrix']).reshape(4, 4).T
        else:
            x, y, z, w = node.get('rotation', [0, 0, 0, 1])
            rotation = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                                 [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                                 [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
            local = np.eye(4)
            local[:3, :3] = rotation @ np.diag(node.get('scale', [1, 1, 1]))
            local[:3, 3] = node.get('translation', [0, 0, 0])
        world = parent @ local
        if 'mesh' in node:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                if primitive.get('mode', 4) != 4 or primitive.get('targets'):
                    raise ValueError('expected static triangle lists')
                attrs = primitive['attributes']
                pos = accessor(attrs['POSITION']).astype(float)
                normals = accessor(attrs['NORMAL']).astype(float)
                if pos.shape != normals.shape or pos.shape[1] != 3:
                    raise ValueError('expected matching vec3 positions and normals')
                pos = pos @ world[:3, :3].T + world[:3, 3]
                normals = normals @ np.linalg.inv(world[:3, :3])
                lengths = np.linalg.norm(normals, axis=1)
                if np.any(lengths < 1e-12):
                    raise ValueError('zero-length normal')
                normals /= lengths[:, None]
                material = doc.get('materials', [{}])[primitive.get('material', 0)] if 'material' in primitive else {}
                pbr = material.get('pbrMetallicRoughness', {})
                if any('Texture' in key for key in pbr) or any('Texture' in key for key in material) or material.get('alphaMode', 'OPAQUE') != 'OPAQUE':
                    raise ValueError('this demo requires opaque, untextured materials')
                factor = np.array(pbr.get('baseColorFactor', [1, 1, 1, 1]))
                colors = np.ones((len(pos), 4))
                if 'COLOR_0' in attrs:
                    source = accessor(attrs['COLOR_0'])
                    colors[:, :source.shape[1]] = source
                colors *= factor
                if np.any(colors[:, 3] != 1):
                    raise ValueError('nonopaque vertex colors are unsupported')
                indices = accessor(primitive['indices']).reshape(-1) if 'indices' in primitive else np.arange(len(pos))
                if len(indices) % 3 or np.any(indices < 0) or np.any(indices >= len(pos)):
                    raise ValueError('invalid triangle indices')
                indices = indices.reshape(-1, 3)
                if np.linalg.det(world[:3, :3]) < 0:
                    indices = indices[:, [0, 2, 1]]
                vertices.append(np.column_stack((pos, normals, colors[:, :3])))
                triangles.append(indices + vertex_count)
                objects.append(dict(node=node.get('name', str(index)), vertices=len(pos),
                                    triangles=len(indices), bounds=[pos.min(0).tolist(), pos.max(0).tolist()],
                                    material=material.get('name'), color=factor.tolist()))
                vertex_count += len(pos)
        for child in node.get('children', []):
            visit(child, world, ancestry | {index})

    for root in doc['scenes'][doc.get('scene', 0)]['nodes']:
        visit(root, np.eye(4), set())
    if not vertices:
        raise ValueError('empty scene')
    return np.concatenate(vertices), np.concatenate(triangles), objects, doc['asset'], raw


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--exclude-node', action='append', default=[])
    p.add_argument('--radius', type=float, help='orthographic half-width in source world units')
    a = p.parse_args()
    v, indices, objects, asset, raw = load_glb(a.source, a.exclude_node)
    lo, hi = v[:, :3].min(0), v[:, :3].max(0)
    center = (lo + hi) / 2
    radius = a.radius or float(np.linalg.norm(hi-lo) / 2 * 1.12)
    if radius <= 0 or not np.isfinite(radius):
        raise ValueError('invalid framing radius')
    v[:, :3] = (v[:, :3] - center) / radius
    if not np.isfinite(v).all():
        raise ValueError('nonfinite vertex data')
    v = v.astype('<f4')
    indices = indices.astype('<u4')
    a.output.mkdir(parents=True, exist_ok=True)
    payload = struct.pack('<4sIII', b'A9M1', len(v), indices.size, 0) + v.tobytes() + indices.tobytes()
    (a.output / 'mesh.bin').write_bytes(payload)
    report = dict(source=str(a.source.resolve()), source_sha256=hashlib.sha256(raw).hexdigest(),
                  asset=asset, vertices=len(v), triangles=len(indices), objects=objects,
                  excluded_nodes=a.exclude_node, center=center.tolist(), radius=radius,
                  mesh_sha256=hashlib.sha256(payload).hexdigest())
    (a.output / 'asset.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
