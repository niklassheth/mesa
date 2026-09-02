# T8132 Apple9 Piglit harness

This directory contains compute profiles for the m1n1-backed DRM shim. The
small GLES 3.1 profile runs five tests in one `shader_runner_gles3` process.
The desktop OpenGL profile runs all 35 tests in Piglit's pinned
`ARB_compute_shader` shader-test corpus through `shader_runner`. Both use
Waffle's surfaceless EGL backend; GLX and X11 are not required.

Install the pinned development dependencies beside Mesa:

```sh
src/asahi/drm-shim/piglit/setup.sh
```

The setup keeps the Waffle and Piglit source checkouts unchanged. It installs
Waffle under `../piglit-local`, builds Piglit under `../piglit-build`, and
places two ignored symlinks in Piglit's `tests` package so its controller can
import the Mesa-owned GLES and desktop profiles.

After resetting and chainloading the target, run the batch directly:

```sh
src/asahi/drm-shim/piglit/run.sh direct
```

Run the same batch through Piglit's result controller:

```sh
src/asahi/drm-shim/piglit/run.sh piglit
```

The `direct` and `piglit` names are GLES aliases. Run the desktop OpenGL
compute corpus with:

```sh
src/asahi/drm-shim/piglit/run.sh direct-gl
src/asahi/drm-shim/piglit/run.sh piglit-gl
```

The controller receives no `LD_PRELOAD`. The profile applies the shim, Mesa
EGL/Gallium paths, m1n1 Python path, and surfaceless Waffle platform only to
the batched shader-runner child. Controller results default to a timestamped
directory under `../piglit-results`; an explicit unused result path may be
passed as the second argument.

Set `MESA_SKIP_BUILD=1` to skip the incremental Mesa and Piglit builds. The
usual `MESA_BUILD`, `M1N1_SHIM_ROOT`, `PIGLIT_ROOT`, `PIGLIT_BUILD`, and
`PIGLIT_LOCAL` overrides are supported. The default controller timeout is 60
seconds for the five-test GLES batch and 300 seconds for the 35-test desktop
batch; `T8132_PIGLIT_TIMEOUT` overrides either value.
