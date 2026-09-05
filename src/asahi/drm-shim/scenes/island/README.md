# Directionally lit island orbit

For the newer perspective demo with per-pixel sunlight, water highlights, fog,
and a procedural sky, see [Sunset island orbit](../sunset/README.md).

An ordinary GLES indexed mesh demo, with Mesa-generated vertex and fragment
shader mains running on T8132 M4 through the m1n1 DRM shim. The GPU performs
matrix transformation, Lambert directional lighting, approximate gamma-2 color
encoding, interpolation, and depth testing. The CPU moves an orthographic camera
around a stationary model and uploads its matrix. The sun remains fixed in world
space. There are no baked lighting images, textures, or model-specific compiler
paths.

## Asset

Use the caller-downloaded **Low-Poly Floating Island** by **Ram Surya**:
<https://sketchfab.com/3d-models/low-poly-floating-island-9700497248364520bd2a9e61272d6b62>.
The supplied GLB identifies its license as **SKETCHFAB Standard** and retains the
source URL and attribution. Source SHA-256:
`144e991a3c6be413cbf85a6443133f35ab5bbb930013917228f777f721e74858`.
The source and converted geometry remain outside the Mesa tree; this directory
contains only the importer, authored shaders, GLES fixture, and checking tools.

Despite the page's “Vertex Colour only” description, this GLB uses nine solid
material colors, with no COLOR_0 attributes or textures. `prepare.py` turns
material factors into per-vertex RGB, preserves the authored normals, flattens
node transforms (including inverse-transpose normal transforms), and combines
all nine objects into one mesh: **3,541 attribute vertices and 2,476 triangles**.
The larger vertex count than the website's welded count preserves normal/UV
seams in the downloaded asset. The water/base geometry is retained. A half-width
of 11.5 source units frames the island closely; the larger water base extends
past the image boundary and exercises viewport clipping.

The importer supports embedded, static GLB triangle lists with normals and
opaque untextured colors. It rejects unsupported required extensions, skinning,
animation, sparse accessors, textures, and transparency. It is intentionally a
small asset preparation tool, not a complete glTF renderer.

## Run

From the Asahi workspace, prepare the externally stored asset:

```sh
uv run --with numpy python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/prepare.py \
  low-poly_floating_island.glb tmp/apple9-island-work/asset-framed --radius 11.5
```

Reset the target via the controller Mac and chainload the intended m1n1 build
using the workspace instructions. Each new hardware process needs this reset.
Then run:

```sh
mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/run.sh \
  tmp/apple9-island-work/asset-framed tmp/apple9-island-work/orbit
uv run --with numpy --with pillow python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/check.py \
  tmp/apple9-island-work/asset-framed tmp/apple9-island-work/orbit
uv run --with pillow python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/animate.py \
  tmp/apple9-island-work/orbit
```

The runner refuses an output directory already containing hardware captures.
`T8132_GLES_VIEWS` sets the number of unique views (default 48).
Each view is rendered twice, with forward and reversed triangle order.
`T8132_GLES_FRAMES=2` performs a short initial check.
The fixture submits one indexed draw per frame, using one interleaved FP32
vertex buffer, a u32 index buffer, and uniforms. Three vertex elements plus the
uniform data use the currently supported four VS buffer arguments.

The animation is `island-orbit.png`, a lossless APNG assembled from completed
hardware attachments, with duplicate order-check frames omitted. Every decoded
animation frame is verified against the source PNG. Its 100 ms playback timing
is presentation timing, not a GPU performance measurement.

## Validation

`frames.bin` records the FP32 camera and light uniforms used by each GLES draw.
`check.py` implements the shader math and an independent CPU triangle rasterizer,
then compares them against the detiled RGBA8 and Depth32Float attachments. It
checks color within one 8-bit level and depth within 3e-6, excluding only a
1/64-pixel band around projected triangle edges. The broad clipped water plane
has a depth slope of approximately 0.000642 per pixel, so one 1/256-pixel
subpixel step is 2.51e-6 in depth. The allowance includes that scale of
interpolation difference plus FP32 rounding; observed errors are retained.

At intersecting/coplanar surfaces, different triangles may win at nearly equal
depth. A color mismatch is accepted only if another actual covering triangle
produces that color (within one level) at a depth within 3e-6 of the GPU depth.
The GPU depth must independently match the nearest reference surface within
3e-6. These depth-tie pixels, boundary differences, and forward/reverse order
color differences are reported explicitly. Reversing order must still leave
the depth attachment exactly unchanged.

`frameNN.png` contains hardware pixels. `reference-NN.png` and
`reference-preview.png` are explicitly separate CPU reference outputs.
`validation.json` records numerical results and attachment hashes. To inspect
framing before using hardware:

```sh
uv run --with numpy --with pillow python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/check.py \
  tmp/apple9-island-work/asset-framed tmp/apple9-island-work/preview --preview
```

This demo uses the existing opaque four-buffer runtime preload documented in
`../mesh/README.md`. Both shader mains are authored GLSL compiled by Mesa; no
proprietary code was disassembled or reproduced for this demo. This milestone
uses orthographic projection, vertex lighting, and a single color varying;
shadows, perspective projection, and interactive presentation are separate work.

Validated on T8132 M4 on 2026-09-05: all **96 renders / 48 orbit views**
completed, with no partial render or TVB growth. All frames passed the final
scene checks. Maximum unambiguous color error was one 8-bit level; maximum
depth error was **2.146803e-6**. There were 13 accepted depth-tie pixels and
162 boundary differences across all 96 comparisons. Reversing triangle order
changed 28 color pixels in total but **zero depth pixels**; the color changes
are confined to the checked depth ties and boundary regions. The original
strict comparison and the final bounded comparison are retained separately in
`tmp/apple9-island-work/`. A negative control corrupting one water pixel's color
and depth was rejected with exactly one error in each check.

The fixture compiled with `-Wall -Wextra -Werror`, Python and shell syntax checks
passed, and the lossless APNG was decoded and compared pixel-for-pixel with all
48 hardware source frames. No compiler or driver implementation changes were
needed for this scene.
