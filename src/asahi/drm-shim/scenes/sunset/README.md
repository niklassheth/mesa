# Sunset island orbit

Real Mesa-generated vertex and fragment shader mains, running on T8132 M4
through the m1n1 DRM shim. This extends the original island demo with per-pixel
sun lighting, material-dependent Blinn–Phong highlights, hemispherical ambient
lighting, distance/height fog, and a procedural sunset sky. The camera uses a
45-degree perspective projection at a low elevation, keeping water and sky
visible together. There is no floating crystal or point light.

The sun disk and directional light stay aligned as the view orbits. This is
equivalent to turning the island beneath a fixed camera and sky. The original
faceted mesh normals are preserved. Lighting, highlights, fog, and the sky are
calculated in the fragment shader. Color is encoded with the existing approximate
gamma-2 convention for the plain RGBA8 attachment.

The source asset is **Low-Poly Floating Island**, by **Ram Surya**, under the
supplied **Sketchfab Standard** license. See [the original island documentation](../island/README.md)
for attribution, source hash, and importer details. Asset data remains outside
Mesa. This variant retains all 2,476 model triangles, including the water base,
and adds one sky triangle. The cyan display base is extended into a water
backdrop; its large triangles are now clipped directly by the GPU. Other island geometry retains its authored positions.

## Run

Use the original full prepared asset, including Object_2 (the water base):

```sh
uv run --with numpy python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/prepare.py \
  low-poly_floating_island.glb tmp/apple9-island-work/asset-framed --radius 11.5
```

Reset through the controller and build/chainload m1n1 as required by the workspace
instructions before each new GPU process. Then:

```sh
mesa-m1n1-shim/src/asahi/drm-shim/scenes/sunset/run.sh \
  tmp/apple9-island-work/asset-framed tmp/apple9-sunset-work/orbit
uv run --with numpy --with pillow python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/sunset/check.py \
  tmp/apple9-island-work/asset-framed tmp/apple9-sunset-work/orbit
uv run --with pillow python \
  mesa-m1n1-shim/src/asahi/drm-shim/scenes/island/animate.py \
  tmp/apple9-sunset-work/orbit
```

Default output is **1024×1024**, 48 views, two renders per view with opposite
triangle submission order. `T8132_GLES_FRAMES=2` runs a short check;
`T8132_GLES_VIEWS` changes orbit sampling. Width/height environment variables can
select another square, 64-pixel-aligned size for the checker. This is an offline
GPU-rendered animation: APNG playback timing is not a performance measurement.
`frameNN.png` and `island-orbit.png` contain actual detiled hardware pixels.
`reference-NN.png` contains separately computed CPU reference pixels.

## Implementation and limits

Ten smooth FP32 varying components carry position, normal, RGB and a material
scalar. The scalar shares the color attribute's fourth component, keeping the
three VBO attribute arguments plus VS uniforms within the four-argument limit.
The sky uses the same pipeline and draw as the island, selected by a negative
material scalar. Its normal varying carries screen coordinates. No shader
branches, textures, blending, captured shader mains, or fixed compiler register
assignments are introduced. Existing opaque runtime helpers remain external.
The initial demo exposed two driver issues, both now fixed:

* The shim placed heap-control metadata at tile-map offset 0x1000. A full
  1024×1024 tile map reaches past that offset, corrupting the metadata. The
  metadata now has its own page in the kernel allocation.
* VS-side pre-division of varyings did not survive hardware clipping. Mesa now
  exports unprojected values and uses native perspective coefficients with a
  coefficient-aware projective multiply. The camera-fitted water workaround
  has been removed; see [varying linkage](../varyings/README.md).

The original failed captures remain under `tmp/apple9-sunset-work/`; the reduced
failures, fixed cases, and regression results are in `tmp/apple9-render-bugs/`.

There are no cast shadows, texture detail, reflection rendering, transparency,
bloom, or post-processing. Water highlights are direct specular lighting, not
reflections of the island.

## Validation

The fixture records exact FP32 geometry, camera matrix, light and eye uniforms
before submitting each frame. `check.py` independently clips triangles against
all six clip planes using double-precision intersections of FP32 vertex outputs,
then rasterizes at pixel centers with 1/256-pixel vertex snapping,
perspective-interpolates the ten components, and evaluates the fragment math.
It compares color within one 8-bit level. Depth uses a base tolerance of 3e-6
plus `(abs(dz/dx) + abs(dz/dy)) / 256` for the winning reference triangle. This
accounts for one subpixel rounding step on steep perspective surfaces; the
observed error, maximum local bound, and count needing this allowance are
reported separately. The original fixed-tolerance comparison is retained as
`validation-fixed-depth.json`. As in the original
checker, only a 1/64-pixel edge band and explicitly verified depth-tie candidates
receive special treatment. Reversed order must leave depth exactly unchanged.
Per-frame errors, edge/tie counts and hashes are saved in `validation.json`.

Initial demo validation on T8132 M4 on 2026-09-05: **96 renders / 48 views at 768×768**
completed without partial renders or TVB growth. All final comparisons pass.
Maximum color error was one 8-bit level; maximum depth error was
7.830967e-06. Ten depth pixels across the 96 frames needed the
triangle-slope subpixel allowance. There were 54 verified color depth-tie pixels;
reversed order changed 53 color pixels across the orbit and zero depth pixels.
A negative control changing one water pixel's color and its depth by 5e-5
was rejected with exactly one color error and one depth error. The 48-frame
lossless APNG was decoded and compared byte-for-byte with its GPU source PNGs.
The GLES fixture passes `-Wall -Wextra -Werror`, and Python/shell syntax and
`git diff --check` pass.

After the allocation and interpolation fixes, **16 renders / 8 viewpoints at
1024×1024** with the enlarged water mesh pass all final comparisons. Maximum
color error is one byte and maximum depth error is 4.202966e-6; four pixels use
the documented slope allowance, and reversing submission order changes no depth
pixels. The CPU clipper uses double precision for its intersections to avoid
reference-only cancellation on the large triangles crossing W=0. Detailed
reports are in `tmp/apple9-render-bugs/sunset1024/validation.json`.
