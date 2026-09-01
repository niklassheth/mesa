#!/bin/sh
# SPDX-License-Identifier: MIT

# Run the exact Apple9 SSBO GLSL corpus through the m1n1-backed DRM shim.
# The caller owns target reset/chainload; this script only builds and launches.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa_build=${MESA_BUILD:-/home/nsheth/Projects/asahi/mesa-m1n1-build}
m1n1_root=${M1N1_SHIM_ROOT:-/home/nsheth/Projects/asahi/m1n1-m4-agx}
submissions=${1:-2}
dispatches_per_submission=${2:-2}
workload=${3:-add3}
binary=$mesa_build/src/asahi/drm-shim/t8132_gles_compute_add3

case "$submissions" in
    ''|*[!0-9]*) echo "usage: $0 [SUBMISSIONS [DISPATCHES_PER_SUBMISSION [WORKLOAD]]]" >&2; exit 2 ;;
esac
case "$dispatches_per_submission" in
    ''|*[!0-9]*) echo "usage: $0 [SUBMISSIONS [DISPATCHES_PER_SUBMISSION [WORKLOAD]]]" >&2; exit 2 ;;
esac

if [ "${MESA_SKIP_BUILD:-0}" != 1 ]; then
    ninja -C "$mesa_build" \
        src/asahi/drm-shim/t8132_gles_compute_add3 \
        src/asahi/drm-shim/libasahi_noop_drm_shim.so \
        src/gallium/targets/dri/libgallium-26.3.0-devel.so \
        src/egl/libEGL_mesa.so.0.0.0
fi

export M1N1_SHIM_LIBRARY=$mesa_build/src/asahi/drm-shim/libasahi_noop_drm_shim.so
export M1N1_SHIM_ROOT=$m1n1_root
export M1N1_SHIM_BINARY=$binary
export T8132_ADD3_SUBMISSIONS=$submissions
export T8132_ADD3_DISPATCHES=$dispatches_per_submission
export T8132_ADD3_WORKLOAD=$workload

uv run --python 3.14 --with-requirements "$m1n1_root/requirements.txt" sh -c '
    python_site=$(find "$VIRTUAL_ENV/lib" -maxdepth 2 -type d \
        -name site-packages -print -quit)
    export PYTHONPATH="$M1N1_SHIM_ROOT/proxyclient:$python_site"
    export PYTHONUNBUFFERED=1
    export M1N1DEVICE=${M1N1DEVICE:-/dev/m1n1}
    export LD_PRELOAD="$M1N1_SHIM_LIBRARY"
    export LD_LIBRARY_PATH="'$mesa_build'/src/egl:'$mesa_build'/src/gallium/targets/dri${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export __EGL_VENDOR_LIBRARY_FILENAMES="'$mesa_build'/src/egl/50_mesa.json"
    export MESA_LOADER_DRIVER_OVERRIDE=asahi
    export MESA_SHADER_CACHE_DISABLE=true
    exec "$M1N1_SHIM_BINARY" "$T8132_ADD3_SUBMISSIONS" \
        "$T8132_ADD3_DISPATCHES" "$T8132_ADD3_WORKLOAD"
'
