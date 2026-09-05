# One hundred triangles

One non-indexed `glDrawArrays(GL_TRIANGLES, 0, 300)` draws a 10×10 grid
through the ordinary Mesa GLSL vertex/fragment compiler and the m1n1 DRM shim.
The vertex shader derives each triangle's position and color from gl_VertexID;
the fragment shader interpolates its color. No production compiler or driver
changes are needed for this scene.

After resetting and chainloading m1n1, invoke `sh run.sh OUTPUT_DIRECTORY`.
The runner draws two 512×512 frames and validates both raw BGRA8 attachments.
Its PNGs contain detiled hardware bytes; the independent CPU reference only
checks coverage and color. A quarter-pixel horizontal offset avoids ambiguous
edge samples. The current carrier supplies the fixed background clear.

Validated on the M4 on 2026-09-04: all 100 triangles present, 612 covered pixels
each, zero coverage errors, color error at most one 8-bit channel step. Both
frames have identical attachment SHA-256:
`b611f28ff66e5b1eac15b1b92380c4413df5cc98a7cf54f1f23d370cd1c2d86b`.

This establishes support for 100 triangles in one draw; it does not establish
the maximum primitive count, indexed drawing, or 100 separate draw calls.
