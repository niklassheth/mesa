# Tidal prism

An own-source GLSL artwork rendered by Mesa's Apple9 compiler through the m1n1
DRM shim on T8132. Both shader mains are generated normally. No textures,
vertex buffers, uniform buffers, captured shader mains, or CPU-generated
pixels are used for the artwork.

The vertex shader creates an asymmetric triangle with vertex W values
1, 1.35, 0.85 and exports three basis components. The fragment shader combines
perspective interpolation, a cubic warp, sine, floor, min/max, smoothstep,
and color arithmetic to make luminous flowing bands and nested contour lines.

After resetting and chainloading m1n1 using the workspace procedure, run:

```sh
src/asahi/drm-shim/scenes/tidal-prism/run.sh /absolute/path/to/output
```

The runner writes a hardware log, two raw attachments, a PNG, and an oracle
report. `readback.py` only detiles/swizzles hardware bytes for the PNG. Its
independent CPU reference checks coverage, perspective coordinates and shader
math; update the reference when changing the artwork. It does not generate
the displayed pixels.

The fixture also accepts arbitrary shader files through
`T8132_GLES_VERTEX_SOURCE` and `T8132_GLES_FRAGMENT_SOURCE`. These use ordinary
GLSL compilation; no scene-specific lowering exists in Mesa.

## Hardware result

2026-09-04, M4 Mac mini / T8132, two standalone 512x512 frames:

- Vertex main: 658 bytes; fragment main: 11,306 bytes.
- 84,841 covered pixels; zero coverage mismatches.
- Every covered pixel matches the independent oracle within one channel byte.
- Attachment SHA-256:
  `dc120bed622344665e61333ac8141750c14f26b3ca15bfc64631f7fd0db0cb01`.
- Both TA and fragment queues completed through the normal shim.

The first version with three sine expressions exceeded the carrier's remaining
shader space. The final version uses one sine and floor-based contours. This is
a code-size limitation of the current carrier and unoptimized full-range trig
lowering, not a triangle-specific compiler restriction. The scene also exposed
missing `fsat` lowering from GLSL smoothstep; this now lowers generally to the
existing FP32 min/max operations.

The fixed-function background still uses the opaque carrier's clear color.
The optional shim attachment dump runs after normal completion and BO
synchronization; it changes no mapping, command, or shader state. General
`glReadPixels` is not used by this gate.
