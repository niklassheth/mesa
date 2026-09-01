#!/bin/sh
# SPDX-License-Identifier: MIT

# Run the capture-backed four-SSBO integer corpus through the m1n1 DRM shim.
# The caller owns target reset and chainload.

set -eu

mesa_build=${MESA_BUILD:-/home/nsheth/Projects/asahi/mesa-m1n1-build}
m1n1_root=${M1N1_SHIM_ROOT:-/home/nsheth/Projects/asahi/m1n1-m4-agx}
binary=$mesa_build/src/gallium/drivers/asahi/t8132_gallium_compute_mix4

if [ "${MESA_SKIP_BUILD:-0}" != 1 ]; then
    ninja -C "$mesa_build" \
        src/gallium/drivers/asahi/t8132_gallium_compute_mix4 \
        src/asahi/drm-shim/libasahi_noop_drm_shim.so
fi

export M1N1_SHIM_LIBRARY=$mesa_build/src/asahi/drm-shim/libasahi_noop_drm_shim.so
export M1N1_SHIM_ROOT=$m1n1_root
export M1N1_SHIM_BINARY=$binary

uv run --python 3.14 --with-requirements "$m1n1_root/requirements.txt" sh -c '
    python_site=$(find "$VIRTUAL_ENV/lib" -maxdepth 2 -type d \
        -name site-packages -print -quit)
    export PYTHONPATH="$M1N1_SHIM_ROOT/proxyclient:$python_site"
    export PYTHONUNBUFFERED=1
    export M1N1DEVICE=${M1N1DEVICE:-/dev/m1n1}
    export LD_PRELOAD="$M1N1_SHIM_LIBRARY"
    export MESA_SHADER_CACHE_DISABLE=true
    exec "$M1N1_SHIM_BINARY" "$@"
' sh "$@"
