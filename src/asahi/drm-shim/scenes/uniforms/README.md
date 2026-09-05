# Graphics uniforms

Two draws share one Mesa-generated vertex/fragment shader pair. Each draw has
a different mat4 transform in VS and vec4 tint in FS. The second frame changes
a scalar time uniform. Both draws must survive with their own data; a mutable
single binding table would collapse geometry or apply the last tint to both.

After resetting and chainloading m1n1, run `sh run.sh OUTPUT_DIRECTORY` for
`glUniformMatrix4fv`, `glUniform4fv`, and `glUniform1f`. Reset and chainload again,
then run `sh run.sh OUTPUT_DIRECTORY blocks` to exercise separate std140 UBOs
bound at API bindings 4 and 7, with distinct buffer ranges for each draw.

The readback tool validates every pixel against independent barycentric
coverage and color calculations. Its PNGs contain detiled hardware bytes.
The current carrier supplies the fixed background clear; this test does not
use glReadPixels.

Both modes passed on T8132 M4 on 2026-09-04: two 512×512 frames, two draws per
frame, 18,432 pixels per triangle, exact coverage and exact 8-bit colors. The
ordinary-uniform and explicit-block attachments are byte-identical:

- Frame 0: `3fcc86f61e25ef83fc4416adefd77a2bb20ad35b685ac4a30fa1b6d208730726`
- Frame 1: `fde6907db327168856067d14be718e3bc261f71165f4c67dba3c3a0b32122ea7`

The later vertex-buffer work expanded the resource contract to four buffer
arguments per stage. In VS, vertex elements share this budget with UBOs. There
are 32 buffer/depth-state draws per batch in the compatibility arena. Shader
mains stay cached independently of values; each draw retains its real buffers
and ranges. The external preload used for that expansion is
`tmp/agx-apple9/render_buffers_launch.bin`, extracted without decoding from
`tmp/agx-re/experiments/EXP-M4-58-vertex-index-depth/`. This supersedes the
one-buffer `render_uniform_launch.bin` from EXP-M4-57. Both uniform modes were
rerun after the expansion and retain the exact hashes above.

The generalized varying path now uses the larger vertex launcher in
`render_buffers_varyings12_launch.bin`. The FS half and four-buffer argument
ABI are unchanged. See [varying linkage](../varyings/README.md) for provenance,
its hash, and the current interface limits.
