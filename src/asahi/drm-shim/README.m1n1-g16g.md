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
  Independent vector lanes are joined by a pre-allocation `COLLECT` pseudo,
  coalesced when already adjacent, and otherwise copied after allocation;
  vector stores never borrow an untracked scratch tuple after allocation.
  Spilling is not implemented and excess pressure fails compilation.
- Gallium owns the compute archive, compiler state, resource records, carrier
  installation, CDM command, BOs, VM bindings, and ordinary
  `drm_asahi_cmd_compute` submission.
- One eight-buffer superset carrier serves every currently supported shader.
  A compiled main may use any prefix of one through eight package resources;
  changing that active count no longer selects a different launch program.
- The shared fixed-USC archive is append-only between installations. A
  screen-wide timeline serializes physical ownership changes, while each
  logical DRM VM keeps its own persistent root in the m1n1 backend.

The supported NIR surface is intentionally limited. It includes arbitrary
u32 constants; integer add/subtract, negate, multiply, AND/OR/XOR/NOT, shifts,
signed and unsigned min/max; core float arithmetic, accurate FP32 reciprocal,
and FMA; comparisons, straight-line select, and recursively nested structured
if/else with multiple blocks and sibling regions; scalar/vector device loads
and stores; general constant, affine, and runtime buffer indexing; 8/16/32-bit
memory formats; and the measured system values and dense dispatch geometries
used by the fixtures.

The control-flow emitter recursively walks structured NIR and serializes its
implicit execution-mask stack. Every pending asynchronous result is copied to
a durable GPR before its NIR block ends, so no pending result crosses a nested
push, else transition, or reconvergence pop. The load sequence's native
`HAS_NEXT` bit still follows linear issue order across those transitions, while
the formerly named `FIRST` bit is now modeled correctly as the raw `get_sr`
index address form. Arm-local stores use the native compare, execution-mask
push, else-transition, and pop sequence. Scalar and 1--4 component Boolean or
8/16/32-bit phis use the same compiler architecture as Apple8: register
allocation assigns one merge destination per component, and each predecessor
performs an ordinary bit-copy into that destination while its lane mask is
active. Merge destinations remain disjoint from every edge source, making each
edge's copy set parallel-copy safe without post-allocation scratch registers.
The merge pseudo itself emits no hardware instruction.
Native select formation remains a future optimization; keeping it disabled for
CFG phis makes this correctness-first slice exercise masked arm computation and
reconvergence directly.

This early bring-up path deliberately does not implement robust buffer access.
The compiler does not infer resource access bounds, and dispatch does not
preflight a shader-derived byte span. Buffer indices are passed through to the
generated address calculation; callers are responsible for binding enough
storage for every access the shader can make.

Loops, `break`, `continue`, early return, division/modulo, spilling, and
unmeasured package/resource forms reject rather than falling back to
capture-assigned registers or opaque native mains.

## External development inputs

The repository deliberately does not contain captured Metal package blobs.
During this bring-up phase, three recapturable inputs from one own-source
eight-buffer carrier are loaded from hardcoded paths under
`/home/nsheth/Projects/asahi/tmp/agx-apple9`:

```text
/home/nsheth/Projects/asahi/tmp/agx-apple9/carrier8/constant.bin
/home/nsheth/Projects/asahi/tmp/agx-apple9/carrier8/launch.bin
/home/nsheth/Projects/asahi/tmp/agx-apple9/carrier8/division.bin
```

The carrier resource record has three hidden entries followed by eight visible
buffer entries and a zero sentinel. For direct dispatch, q0 points to the
record's total-thread tuple and q1 to `{1,1,1}`. For indirect dispatch, q0 is
the caller's raw group-count pointer and q1 points to the record's local-size
scale tuple. The unchanged launch program derives the common execution
geometry from that tagged representation. The third hidden entry points to the
relocated 8-KiB integer-division helper table in `division.bin`.
Unused visible entries are padded with a valid mapped pointer that the compiled
main cannot reference. The full one-through-eight prefix has passed exact
hardware output and input/guard-preservation checks through ordinary GLSL,
Gallium, the DRM UAPI, and the G16 shim.

File lengths are taken from the files. The driver validates only the regions
it actually installs or patches; it does not require an exact whole-file size.

Two optional, currently non-gated render research inputs use the same
directory:

```text
/home/nsheth/Projects/asahi/tmp/agx-apple9/g16_render_package.bin.zst
/home/nsheth/Projects/asahi/tmp/agx-apple9/render_interleaved_vbo_launch.bin
```

They do not restore the removed render compiler. They are retained outside Git
only so future packaging work can reuse the current semantic relocation and
archive experiments without putting opaque Metal data in repository history.

The current local T8132 input hashes are:

```text
a3586e009bd675feb6b67d72b6f8b9500bde15487584c1a054925ac9af2d75ce  carrier8/constant.bin
62e85e9dd6cca4dd033cb101ad860da28934d8f92d7498d7bd11b42eff0957c3  carrier8/launch.bin
fbb72c3f6ffb8e4a2fd17c9155d5ae32d7e704eeb5c0c906bef6d315c7299e80  carrier8/division.bin
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
  --print-errorlogs

/home/nsheth/Projects/asahi/mesa-m1n1-build/src/asahi/drm-shim/\
t8132_apple9_compute_runner --list
```

## Run on T8132

Build and chainload the matching `m1n1-m4-agx` tree first, leaving the target
at its proxy prompt with a cold GPU ASC. The native compute tests enter through
ordinary GLES 3.1, the normal Mesa driver, and the DRM UAPI; they do not
construct NIR or m1n1 work records directly.

Run the default supported compute regression set directly in one process and
EGL context:

```sh
src/asahi/drm-shim/piglit/run.sh direct
```

The default one-boot sequence omits `archive-cross-program-sequence`,
`program-lifecycle-stress`, and the 24-program `two-source-comparisons` matrix
to reduce pressure on the temporary append-only 64 KiB shader archive. As the
ordinary compiler corpus grows, a complete default run can still reach that
temporary limit; run the named control-flow and lifecycle batches separately
after fresh chainloads when it does. Archive exhaustion is a packaging-harness
limit, not a shader result. The two lifecycle cases test program-publication
lifetime; the comparison matrix is the exhaustive predicate correctness gate.

Or record each named case as a Piglit subtest while retaining the same
single-process execution model:

```sh
src/asahi/drm-shim/piglit/run.sh piglit
```

Success means exact complete-buffer comparison, not merely command retirement.
The native cases also verify immutable inputs, poison-filled gaps, leading and
trailing guards, multiple bindings and dependent addresses. Dedicated cases
exercise repeated range dispatch, program publication/retirement pressure, and
ordered reuse of independently compiled programs.
`simple-divergent-if-else` checks unsigned, signed, and floating-point `<`,
`>=`, equality, inequality, and composed Boolean forms with mixed, all-true,
and all-false lane populations. Its floating-point inputs include NaNs and both
signed zeros, so `>=` is validated as an IEEE ordered comparison rather than a
naive inversion of `<`. Each of its 36 submissions verifies both predicated arm
stores and a post-reconvergence value merge across complete 16,384-word outputs,
while inactive outputs retain poison and surrounding guards remain unchanged.
`two-source-comparisons` independently computes both operands and covers every
canonical signed, unsigned, and floating-point equality/order relation,
including relations normalized by reversing operands. It runs mixed, all-true,
and all-false populations; the mixed floating-point population includes NaNs,
infinities, adjacent finite values, and both signed zeros. Additional short and
extended predicates keep neither, either, or both sources live after the
comparison. `compare-register-pressure` repeats a retained-source comparison
with 16 independently loaded values live across the region.
`single-region-shapes` covers empty, then-only, else-only, and two-sided arms,
with exact stores before, inside, and after the region. `multiple-phi-vectors`
checks a scalar phi and a four-component phi from ordinary GLSL through exact
scalar and vector-buffer outputs.
`branch-local-device-loads` uses seven SSBOs and three executions to cover
mixed, all-true, and all-false lane populations. It performs distinct affine
loads in both arms, fans each loaded value into an arm-local side effect and a
32-bit phi, then combines the merged value with a post-reconvergence load. All
three complete 16,384-word output buffers, inactive poison values, and guard
regions are checked exactly.
`nested-short-circuit-if-else` covers nested conditionals in both outer arms,
multi-level value reconvergence, sibling and asymmetric regions, and observable
right-hand-side stores for short-circuit AND and OR. Seven complete 16,384-word
outputs verify both final values and that skipped right-hand sides leave poison
unchanged. `nested-branch-local-loads` issues two distinct pending loads in
each of four nested leaf regions, resolves their values through both inner and
outer phis, and checks two complete guarded outputs. The two tests also pass
back-to-back in one process and persistent VM.
`reciprocal-direct`, `reciprocal-retain`, and `reciprocal-materialized` cover
pending-load handoff, source lifetime, and ordinary-GPR input paths with exact
FP32 output oracles. `reciprocal-denominators-1-1024` converts every integer
denominator in the permitted local-size domain with the compiler's ordinary
U2F and reciprocal path, then checks the returned FP32 values against
`abs(D * r - 1) <= 2^-18`. On T8132 the worst observed denominator is 981,
with residual `7.1362592279911041e-8`; no Newton refinement is required for
the ceiling-division lowering. `ceil-div-grid-domain` executes the compiler's
complete U2F/reciprocal/FMA/F2U/multiply/compare/correct sequence for eight
numerators at every denominator, including zero, remainder boundaries, and
the maximum legal `65535 * D`; all 8,192 exact integer outputs pass on T8132.
`superset-1` through `superset-8` specifically compile distinct ordinary GLSL
programs and exercise every active-resource occupancy of the shared carrier.

The indirect-dispatch geometry cases exercise the actual GLES API with CPU-
and GPU-authored records, nonzero offsets, zero dimensions, and asymmetric 2D
and 3D grids. `gl_NumWorkGroups` is compiled from the hidden q0/q1 package
contract and checked in every dimension for direct and indirect execution. It
does not consume a visible buffer binding or require CPU inspection of the
indirect record. Direct dispatch computes `ceil(global_threads / local_size)`
with the measured FP32 reciprocal sequence and one exact integer correction;
this covers short final workgroups and fixed or runtime local sizes. Indirect
dispatch returns the caller-provided group count directly.

## Bring-up boundary

This branch is an investigative compiler and packaging harness, not a complete
Mesa driver. The immediate productive direction is to keep expanding semantic
compute lowering and replace each external wrapper field as it becomes
understood. Render should resume from compiler-generated stage code and a
verified packaging model; the old capture-matched render compiler should not
be resurrected as a fallback.
