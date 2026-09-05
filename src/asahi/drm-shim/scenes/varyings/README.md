# Linked vertex/fragment varyings

Apple9 supports up to **12 smooth FP32 user components**, in addition to the
four position components. Three vec4s, four vec3s, or mixtures of scalars and
vectors fit the same budget. This is the current compiler publication limit,
not a claimed hardware maximum.

The compiler records a component mask for each user location VAR0..VAR31 and
compacts the written components into scalar export slots. The FS is compiled
against that producer layout; its shader key includes the layout. Constant
array offsets, gaps between locations, and partial vectors are supported.
An input absent from the producer is rejected. Declaration order does not
define the cross-stage mapping.

The VS exports ordinary, unprojected values so homogeneous clipping can create
new vertices correctly. The driver requests native perspective coefficients
(shade 7), and the FS uses **coefficient-aware projective multiplication** with
reciprocal interpolated 1/W. This handles the rasterizer's primitive-constant
coefficient representation as well as varying values.

The earlier ordinary multiply incorrectly applied an extra W factor to constant
components. Its workaround—pre-dividing varyings in the VS and requesting linear
coefficients—passed unclipped and guard-band cases but corrupted attributes when
hardware clipping created vertices. The coefficient-aware multiply is reachable
from normal input lowering and uses ordinary register allocation; it replaces
that workaround. No captured shader sequence or fixed register assignment is
part of the compiler.

The driver generates the coefficient table and the related vertex/fragment
state from the linked count. In particular, bind0 +0x44 is the user scalar
count; it was previously hardcoded to three. The VDM output count includes
position, while coefficient counts include interpolated 1/W.

## Hardware checks

Reset and chainload m1n1 before **each** invocation, then run from the Asahi
workspace:

```sh
mesa-m1n1-shim/src/asahi/drm-shim/scenes/varyings/run.sh NEW_OUTPUT_DIRECTORY nine
```

Modes: `seven`, `eight`, `nine`, `twelve`, `perspective`, `clipped`, `zero`,
`depth-clipped`, `procedural`.
Each checks all pixels of two 512x512 hardware attachments against an
independent CPU reference and requires identical attachments. The indexed
quad reverses triangle submission order in the second frame. `perspective`
uses W=0.625 and 1.375 while preserving screen geometry. `clipped` also extends
the quad outside the viewport. `depth-clipped` crosses the near and far
clip planes with unequal W and both constant and nonconstant components, ensuring
actual hardware clipping rather than relying on the rasterizer guard band. `procedural` generates
the equivalent quad from vertex ID without VBOs or UBOs; its two frames repeat
the same draw. Cross-stage optimization may remove its constant outputs.

The nine-component test uses independently computed color, position-derived,
and quadratic vectors, declared in a different order in FS. Twelve adds a
fourth component to every vector. Seven/eight cover the coefficient allocation
boundary, and zero checks shaders with no user varyings. The compiler tests
also cover sparse locations, component holes, array offsets, missing producer
components, different producer layouts for the same FS, and the capacity
rejection.

## External runtime

The current opaque preload is outside Mesa:

`tmp/agx-apple9/render_buffers_varyings12_launch.bin`

SHA-256: `0d7c5eac10ecbd282c96f04785b3d7475274f4cdf8f2b26ed2ae1a4258fb3f7d`.
Its VS half is the complete 0xc0-byte launcher captured from our twelve-varying,
four-buffer Metal probe in EXP-M4-59. Its FS half is the unchanged EXP-M4-58
four-buffer launcher. The old VS launcher lost an output when all sixteen
publication registers were occupied. Neither launcher contains our API shader
main, and neither was disassembled or decompiled. Generated shader mains and
all linkage tables remain independently authored.

`tmp/agx-re/experiments/EXP-M4-59-varyings/extract_launcher.py` reproduces it
from the trusted `twelve.pkl.gz` capture and the retained
`tmp/agx-apple9/render_buffers_launch.bin`. The old blob is preserved.

Only pixel-center smooth FP32 interpolation is supported here. Flat/integer,
centroid/sample, and noperspective shader inputs remain unsupported. Graphics
control flow, textures, multiple render targets, and arbitrary raster state
are separate work. The four-buffer budget per stage and 32 draw-record limit
still apply; interfaces larger than four components also use that draw arena
when they have no buffers.

Validation on T8132, 2026-09-05: all eight cases pass both hardware frames with
no coverage or color failures (at most one channel byte of rounding error).
The indexed quad, depth-tested cube, 100-triangle scene and 2,476-triangle
island also pass: 24 hardware frames in total. All 190 compiler tests pass.
The complete reports are retained in EXP-M4-59 `VALIDATION.json`.

The clipping correction is validated by the nine-mode suite, including explicit
near/far-plane clipping, and by 16 sunset frames at 1024×1024 with enlarged
water triangles crossing the view volume. Source-correlated encoding evidence
and the before/after results are recorded in `tmp/apple9-render-bugs/RESULTS.md`.
The suite has 191 passing compiler tests after adding the projective-multiply
operand/encoding check.
