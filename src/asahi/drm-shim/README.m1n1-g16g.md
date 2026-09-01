# Apple9 compute through the m1n1 DRM shim

This development tree runs Mesa's Asahi Gallium driver against a real T8132
through the modern Asahi DRM UAPI. The preload library presents a render node,
embeds the m1n1 Python backend, and forwards device parameters, VM/GEM
operations, queues, synchronization, and submissions to the source-built G16
firmware backend.

The current Mesa milestone is **compute**. The earlier capture-matched Apple9
render compiler was removed because it was no longer representative of the
compiler and packaging model. Render packaging helpers and the GLES triangle
fixture remain useful research scaffolding, but a render shader currently
fails during compilation and the triangle is not a regression gate.

## Current compute model

- T8132 is exposed as G16G and T8140 as G17P. Apple9 has a distinct shader-ISA
  key and is not silently compiled as Apple8.
- G16 and G17 use the same fixed 1-TiB USC base, `0x10000000000`. The former
  relocatable G17 behavior was a shim mistake, not a hardware capability.
- GLSL compute shaders travel through Gallium and NIR into a semantic Apple9
  VIR, register allocator, scheduler, and machine encoder. Accepted shaders do
  not select a captured whole-main byte sequence.
- Scoreboard slots are allocated as compiler state. Pending asynchronous
  producers carry a slot; compatible consumers name that slot and release or
  retain it according to liveness. Ordinary ALU results are ordinary GPR
  values, not a second provenance system.
- The allocator supports the measured r0-r63 bank, scalar and adjacent tuple
  classes, copies around constrained instructions, and last-use release.
  Spilling is not implemented and excess pressure fails compilation.
- Gallium owns the compute archive, compiler state, resource records, launch
  wrapper selection, CDM command, BOs, VM bindings, and ordinary
  `drm_asahi_cmd_compute` submission.
- The shared fixed-USC archive is append-only between installations. A
  screen-wide timeline serializes physical ownership changes, while each
  logical DRM VM keeps its own persistent root in the m1n1 backend.

The supported NIR surface is intentionally bounded. It includes arbitrary
u32 constants; integer add/subtract, negate, multiply, AND/OR/XOR/NOT, shifts,
signed and unsigned min/max; core float arithmetic and FMA; comparisons and
straight-line select; scalar/vector device loads and stores; general constant,
affine, and runtime buffer indexing; 8/16/32-bit memory formats; and the
measured system values and dense dispatch geometries used by the fixtures.

Structured conditional side effects, phis, loops, `break`, `continue`,
division/modulo, spilling, and unmeasured package/resource forms reject rather
than falling back to capture-assigned registers or opaque native mains.

## External development inputs

The repository deliberately does not contain captured Metal package blobs.
During this bring-up phase, five recapturable compute wrapper inputs are loaded
from hardcoded paths under `/tmp/agx-apple9`:

```text
/tmp/agx-apple9/launch_ssbo0_u32.bin
/tmp/agx-apple9/constant_ssbo3_state_u6.bin
/tmp/agx-apple9/launch_ssbo2_integer_u32.bin
/tmp/agx-apple9/launch_ssbo3_state_u6.bin
/tmp/agx-apple9/launch_ssbo4_mix_u32.bin
```

Their lengths are taken from the files. The driver checks only offsets it must
patch or inspect, so a recaptured G17P wrapper is not rejected merely because
its complete size differs from the current T8132 input.

Two optional, currently non-gated render research inputs use the same
directory:

```text
/tmp/agx-apple9/g16_render_package.bin.zst
/tmp/agx-apple9/render_interleaved_vbo_launch.bin
```

They do not restore the removed render compiler. They are retained outside Git
only so future packaging work can reuse the current semantic relocation and
archive experiments without putting opaque Metal data in repository history.

The current local T8132 input hashes are:

```text
0fe7ae20c8761022b6940efde92e2b90085a87ad96d7a817d14c96370a56a80a  launch_ssbo0_u32.bin
9baa760c5185b9e5645bd1299e5ec948674258d6cbb0dc68b1394f1e45f3fd27  constant_ssbo3_state_u6.bin
b49a0d82230637faa29d92d55fb659efbc396fc339c93dd80a8f1114b90711e6  launch_ssbo2_integer_u32.bin
86b23ba7a8e7b030d6e813846461d6695bf98c60233a44e49a78fb34b36a9f0c  launch_ssbo3_state_u6.bin
631476197ad6b0fd8ac0b2d50b1ca07096a59650f0b247576ba95762ec196541  launch_ssbo4_mix_u32.bin
9c7912148f4d4b48b59ba8e720e9dc94d0988f191294394af79849b21fb99cfe  g16_render_package.bin.zst
da8e9c9df75305fb8d11cd8d468e8cf0f35bb5172e7465777b6d258f243b145b  render_interleaved_vbo_launch.bin
```

The compute files are development inputs, not stable ABI. Recapture notes
should record their source workload, OS/build, package role, and hash outside
the Mesa repository until their remaining fields are replaced by semantic
builders.

## Build

The Asahi driver needs Mesa's generated internal CLC programs. On Arch-based
hosts the additional package is `spirv-llvm-translator`.

```sh
meson setup /home/nsheth/Projects/asahi/mesa-m1n1-build \
  --prefix /home/nsheth/Projects/asahi/mesa-m1n1-install/usr/local \
  -Dgallium-drivers=asahi \
  -Degl=enabled \
  -Dplatforms=[] \
  -Degl-native-platform=surfaceless \
  -Dgles1=disabled \
  -Dgles2=enabled \
  -Dglx=disabled \
  -Dgbm=disabled \
  -Dvulkan-drivers=[] \
  -Dllvm=enabled \
  -Dbuild-tests=true \
  -Dtools=drm-shim

ninja -C /home/nsheth/Projects/asahi/mesa-m1n1-build
```

Useful host-only gates are:

```sh
meson test -C /home/nsheth/Projects/asahi/mesa-m1n1-build \
  agx_tests \
  t8132_gallium_compute_xyz_oracle \
  t8132_gallium_compute_xy_oracle \
  t8132_gallium_compute_device_index_oracle \
  t8132_gallium_compute_device_graph_oracle \
  --print-errorlogs
```

## Run on T8132

Build and chainload the matching `m1n1-m4-agx` tree first, leaving the target
at its proxy prompt with a cold GPU ASC. The Gallium tests enter through the
normal Mesa driver and DRM UAPI; they do not construct m1n1 work records.

Representative exact-output gates are:

```sh
src/asahi/drm-shim/run_t8132_gallium_compute_dag3.sh --case fconst3
src/asahi/drm-shim/run_t8132_gallium_compute_dag3.sh --state-append-after-use
src/asahi/drm-shim/run_t8132_gallium_compute_dag3.sh --state-slab-boundary

T8132_DUAL_VM_ROUNDS=32 \
  src/asahi/drm-shim/run_t8132_gallium_compute_dag3.sh --dual-vm-state-alias

src/asahi/drm-shim/run_t8132_gallium_compute_xy.sh --two-dispatch
src/asahi/drm-shim/run_t8132_gallium_compute_xyz.sh
src/asahi/drm-shim/run_t8132_gallium_compute_device_index.sh
src/asahi/drm-shim/run_t8132_gallium_compute_device_graph.sh
src/asahi/drm-shim/run_t8132_gallium_compute_mix4.sh --suite --two-dispatch
```

The GLES compiler/package harness offers broader shader selection:

```sh
src/asahi/drm-shim/run_t8132_gles_compute.sh constant
src/asahi/drm-shim/run_t8132_gles_compute.sh gid
src/asahi/drm-shim/run_t8132_gles_compute.sh mad batch-two
src/asahi/drm-shim/run_t8132_gles_compute.sh dag-suite batch-two
src/asahi/drm-shim/run_t8132_gles_compute.sh cache-suite batch-two
src/asahi/drm-shim/run_t8132_gles_compute.sh single-boot-suite
src/asahi/drm-shim/run_t8132_gles_compute.sh bulk-suite
src/asahi/drm-shim/run_t8132_gles_compute_add3.sh 2 4 bringup-suite
```

Success means exact complete-buffer comparison, not merely command retirement.
The larger fixtures also verify immutable inputs, poison-filled gaps, leading
and trailing guards, multiple dispatches, append-only queue counters, and
ordered completion timestamps.

## Bring-up boundary

This branch is an investigative compiler and packaging harness, not a complete
Mesa driver. The immediate productive direction is to keep expanding semantic
compute lowering and replace each external wrapper field as it becomes
understood. Render should resume from compiler-generated stage code and a
verified packaging model; the old capture-matched render compiler should not
be resurrected as a fallback.
