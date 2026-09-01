#!/bin/sh
# SPDX-License-Identifier: MIT

# Run exact copy2 -> supported VBO triangle -> same exact copy2 through one
# GLES 3.1 context.  The caller owns reset and chainload.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa_build=${MESA_BUILD:-/home/nsheth/Projects/asahi/mesa-m1n1-build}
m1n1_root=${M1N1_SHIM_ROOT:-/home/nsheth/Projects/asahi/m1n1-m4-agx}
binary=$mesa_build/t8132_gles_compute_vbo_lifecycle

ninja -C "$mesa_build" \
    src/asahi/drm-shim/libasahi_noop_drm_shim.so \
    src/gallium/targets/dri/libgallium-26.3.0-devel.so \
    src/egl/libEGL_mesa.so.0.0.0

cc -std=c11 -O2 -Wall -Wextra -Werror \
    "$script_dir/t8132_gles_compute_vbo_lifecycle.c" \
    -o "$binary" -lEGL -lGLESv2

if [ "${1:-}" = --self-test ]; then
    [ "$#" -eq 1 ] || {
        echo "usage: $0 [--self-test]" >&2
        exit 2
    }
    exec "$binary" --self-test
fi
[ "$#" -eq 0 ] || {
    echo "usage: $0 [--self-test]" >&2
    exit 2
}

export M1N1_SHIM_LIBRARY=$mesa_build/src/asahi/drm-shim/libasahi_noop_drm_shim.so
export M1N1_SHIM_ROOT=$m1n1_root
export M1N1_SHIM_BINARY=$binary
export G16G_RENDER_SOURCE=1
export G16G_EXECUTE_MESA_RENDER=1
export G16G_MESA_RENDER_STAGE=direct
export AGX_APPLE9_DIRECT_RENDER=1

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
    export ASAHI_MESA_DEBUG=${ASAHI_MESA_DEBUG:-nocompress}
    exec "$M1N1_SHIM_BINARY"
'
