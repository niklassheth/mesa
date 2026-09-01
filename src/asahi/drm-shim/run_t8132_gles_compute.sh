#!/bin/sh
# SPDX-License-Identifier: MIT

# Run one Mesa-owned Apple9 compute workload through the ordinary DRM UAPI.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa_build=${MESA_BUILD:-/home/nsheth/Projects/asahi/mesa-m1n1-build}
m1n1_root=${M1N1_SHIM_ROOT:-/home/nsheth/Projects/asahi/m1n1-m4-agx}
workload=${1:-constant}
batching=${2:-single}
binary=$mesa_build/t8132_gles_compute

case "$workload" in
    constant|constant32|constant32-sparse|gid|mad|dag|reuse-dag|select-dag|compare-dag|compare-complete|\
    deep-int-dag|diamond-int-dag|fanout-int-dag|logic-lifetime-dag|pressure-int-dag|\
    minmax-int-dag|nested-select-dag|deep-float-dag|fanout-float-dag|mixed-domain-dag|\
    radix-alternating-dag|select-all-live-dag|minmax-nested-live-dag|fma-all-live-dag|\
    float-cache-ring-dag|cross-domain-cache-dag|logic-minmax-select-dag|cache-pressure-dag|\
    add|sub|rsub|mul|and|or|xor|not|ineg|u2f|i2f|f2i|f2u|\
    shl|ashr|ushr|imin|imax|umin|umax|fadd|fsub|rfsub|fmul|fmin|fmax|\
    fabs|fneg|fma|fma-nan-mul|archive-cross-0|archive-cross-1|archive-cross-2|archive-cross-3|\
    archive-cross-4|archive-cross-5|archive-cross-6|archive-cross-7|\
    sequence|single-boot-suite|bulk-suite|suite|dag-suite|cache-suite) ;;
    *) echo "usage: $0 WORKLOAD|sequence|single-boot-suite|bulk-suite|suite|dag-suite|cache-suite [single|batch-two]" >&2; exit 2 ;;
esac
case "$batching" in
    single|batch-two) ;;
    *) echo "usage: $0 WORKLOAD|suite|dag-suite|cache-suite [single|batch-two]" >&2; exit 2 ;;
esac

if [ "${MESA_SKIP_BUILD:-0}" != 1 ]; then
    ninja -C "$mesa_build" \
        src/asahi/drm-shim/libasahi_noop_drm_shim.so \
        src/gallium/targets/dri/libgallium-26.3.0-devel.so \
        src/egl/libEGL_mesa.so.0.0.0

    cc -std=c11 -O2 -Wall -Wextra -Werror \
        "$script_dir/t8132_gles_compute.c" -o "$binary" \
        -lEGL -lGLESv2 -lm
fi

# The persistent G16 queue now supports substantially more than the former
# two-job limit. Keep these shell-level suites as isolated cold jobs anyway so
# every compiler workload retains an independent init/firmware failure boundary.
if [ "$workload" = suite ] || [ "$workload" = dag-suite ] ||
   [ "$workload" = cache-suite ]; then
    if [ "$workload" = dag-suite ]; then
        workloads='deep-int-dag diamond-int-dag fanout-int-dag logic-lifetime-dag pressure-int-dag
minmax-int-dag nested-select-dag deep-float-dag fanout-float-dag mixed-domain-dag'
        suite_count=10
    elif [ "$workload" = cache-suite ]; then
        workloads='radix-alternating-dag select-all-live-dag minmax-nested-live-dag fma-all-live-dag
float-cache-ring-dag cross-domain-cache-dag logic-minmax-select-dag cache-pressure-dag'
        suite_count=8
    else
    workloads='constant constant32 constant32-sparse gid mad dag reuse-dag select-dag compare-dag compare-complete
deep-int-dag diamond-int-dag fanout-int-dag logic-lifetime-dag pressure-int-dag
minmax-int-dag nested-select-dag deep-float-dag fanout-float-dag mixed-domain-dag
radix-alternating-dag select-all-live-dag minmax-nested-live-dag fma-all-live-dag
	float-cache-ring-dag cross-domain-cache-dag logic-minmax-select-dag cache-pressure-dag
	add sub rsub mul and or xor not ineg u2f i2f f2i f2u
	shl ashr ushr imin imax umin umax fadd fsub rfsub fmul fmin fmax fabs fneg fma fma-nan-mul
	archive-cross-0 archive-cross-1 archive-cross-2 archive-cross-3
	archive-cross-4 archive-cross-5 archive-cross-6 archive-cross-7'
        suite_count=66
    fi
    suite_start=${MESA_SUITE_START:-}
    suite_started=0
    for item in $workloads; do
        if [ "$suite_started" -eq 0 ]; then
            if [ -n "$suite_start" ] && [ "$item" != "$suite_start" ]; then
                continue
            fi
            suite_started=1
        fi
        echo "T8132_GLES_COMPUTE_SUITE_BEGIN workload=$item"
        ssh 192.168.1.166 \
            'sudo -n /opt/homebrew/bin/macvdmtool reboot serial' >/dev/null
        index=0
        while [ ! -e /dev/m1n1 ]; do
            index=$((index + 1))
            [ "$index" -lt 60 ] || { echo 'timed out waiting for /dev/m1n1' >&2; exit 1; }
            sleep 1
        done
        chainload_output=$(uv run --with-requirements "$m1n1_root/requirements.txt" \
            python "$m1n1_root/proxyclient/tools/chainload.py" \
            -r "$m1n1_root/build/m1n1.bin")
        printf '%s\n' "$chainload_output" | tail -n 3
        MESA_SKIP_BUILD=1 "$0" "$item" "$batching"
    done
    if [ "$suite_started" -eq 0 ]; then
        echo "unknown MESA_SUITE_START workload: $suite_start" >&2
        exit 2
    fi
    echo "T8132_GLES_COMPUTE_SUITE_OK workloads=$suite_count"
    exit 0
fi

export M1N1_SHIM_LIBRARY=$mesa_build/src/asahi/drm-shim/libasahi_noop_drm_shim.so
export M1N1_SHIM_ROOT=$m1n1_root
export M1N1_SHIM_BINARY=$binary
if [ "$workload" = bulk-suite ]; then
    # Unlike the shell-level cold-boot suite above, the binary's
    # `suite` mode records a selected prefix into disjoint ranges of one large
    # SSBO and validates them after one final synchronization/map. By default
    # the binary selects its complete corpus, including the variants that
    # force a launch call across the archive's former 16-bit boundary.
    if [ -n "${T8132_COMPUTE_SUITE_WORKLOADS:-}" ]; then
        export T8132_COMPUTE_SUITE_WORKLOADS
    fi
    export M1N1_SHIM_WORKLOAD=suite
else
    export M1N1_SHIM_WORKLOAD=$workload
fi
export M1N1_SHIM_BATCHING=$batching

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
    if [ "$M1N1_SHIM_BATCHING" = batch-two ]; then
        exec "$M1N1_SHIM_BINARY" "$M1N1_SHIM_WORKLOAD" batch-two
    else
        exec "$M1N1_SHIM_BINARY" "$M1N1_SHIM_WORKLOAD"
    fi
'
