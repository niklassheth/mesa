#!/bin/sh
# SPDX-License-Identifier: MIT

# Run Apple9 desktop GL or GLES compute Piglit through the m1n1 DRM shim.
# The caller owns target reset and m1n1 chainload.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa_root=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
workspace=$(dirname "$mesa_root")

mesa_build=${MESA_BUILD:-$workspace/mesa-m1n1-build}
m1n1_root=${M1N1_SHIM_ROOT:-$workspace/m1n1-m4-agx}
piglit_root=${PIGLIT_ROOT:-$workspace/piglit}
piglit_build=${PIGLIT_BUILD:-$workspace/piglit-build}
piglit_local=${PIGLIT_LOCAL:-$workspace/piglit-local}
mode=${1:-direct}

case "$mode" in
    direct|direct-gles)
        action=direct
        api=gles
        ;;
    piglit|piglit-gles)
        action=piglit
        api=gles
        ;;
    direct-gl)
        action=direct
        api=gl
        ;;
    piglit-gl)
        action=piglit
        api=gl
        ;;
    *)
        echo "usage: $0 [direct|piglit|direct-gl|piglit-gl] [RESULTS_DIRECTORY]" >&2
        exit 2
        ;;
esac

shim=$mesa_build/src/asahi/drm-shim/libasahi_noop_drm_shim.so
if [ "$api" = gles ]; then
    runner=$piglit_build/bin/shader_runner_gles3
    piglit_target=shader_runner_gles3
    profile=apple9_compute
    results_name=apple9-compute
    default_timeout=60
else
    runner=$piglit_build/bin/shader_runner
    piglit_target=shader_runner
    profile=apple9_desktop_compute
    results_name=apple9-desktop-compute
    default_timeout=300
fi
timeout=${T8132_PIGLIT_TIMEOUT:-$default_timeout}
profile_link=$piglit_root/tests/$profile.py

if [ ! -L "$profile_link" ] ||
   [ "$(readlink -f "$profile_link")" != "$script_dir/$profile.py" ]; then
    echo "Piglit profile link is missing; run $script_dir/setup.sh" >&2
    exit 1
fi

if [ "${MESA_SKIP_BUILD:-0}" != 1 ]; then
    ninja -C "$mesa_build" \
        src/asahi/drm-shim/libasahi_noop_drm_shim.so \
        src/gallium/targets/dri/libgallium-26.3.0-devel.so \
        src/egl/libEGL_mesa.so.0.0.0
    cmake --build "$piglit_build" --target "$piglit_target" --parallel
fi

for required in \
    "$shim" \
    "$runner" \
    "$mesa_build/src/egl/50_mesa.json" \
    "$piglit_local/lib/libwaffle-1.so"; do
    if [ ! -e "$required" ]; then
        echo "missing required file: $required" >&2
        exit 1
    fi
done

if [ "$action" = piglit ]; then
    results=${2:-$workspace/piglit-results/$results_name-$(date +%Y%m%d-%H%M%S)}
    if [ -e "$results" ]; then
        echo "results path already exists: $results" >&2
        exit 1
    fi
else
    results=
fi

export T8132_PIGLIT_ACTION=$action
export T8132_PIGLIT_API=$api
export T8132_PIGLIT_RESULTS=$results
export T8132_PIGLIT_ROOT=$piglit_root
export T8132_PIGLIT_BUILD=$piglit_build
export T8132_PIGLIT_TEST_DIR=$script_dir/tests
export T8132_PIGLIT_GL_TEST_DIR=$script_dir/tests-gl
export T8132_PIGLIT_RUNNER=$runner
export T8132_PIGLIT_PROFILE=$profile
export T8132_PIGLIT_TIMEOUT=$timeout
export T8132_PIGLIT_GL_SUBSET=${T8132_PIGLIT_GL_SUBSET:-supported}
export T8132_PIGLIT_CHILD_LD_PRELOAD=$shim
export T8132_PIGLIT_CHILD_LD_LIBRARY_PATH="$piglit_local/lib:$piglit_build/lib:$mesa_build/src/egl:$mesa_build/src/gallium/targets/dri${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export T8132_PIGLIT_CHILD_EGL_VENDOR=$mesa_build/src/egl/50_mesa.json
export T8132_PIGLIT_M1N1_ROOT=$m1n1_root

uv run --python 3.14 \
    --with-requirements "$m1n1_root/requirements.txt" \
    --with 'numpy>=1.13' \
    --with 'mako>=1.0.2' \
    sh -c '
        python_site=$(find "$VIRTUAL_ENV/lib" -maxdepth 2 -type d \
            -name site-packages -print -quit)
        export T8132_PIGLIT_CHILD_PYTHONPATH="$T8132_PIGLIT_M1N1_ROOT/proxyclient:$python_site"

        if [ "$T8132_PIGLIT_ACTION" = direct ]; then
            if [ "$T8132_PIGLIT_API" = gles ]; then
                set -- "$T8132_PIGLIT_TEST_DIR"/*.shader_test
            else
                case "$T8132_PIGLIT_GL_SUBSET" in
                    smoke)
                        set -- "$T8132_PIGLIT_GL_TEST_DIR/01-global-invocation-id.shader_test"
                        ;;
                    supported)
                        set -- \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/no_local_work_size.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/mismatched_local_work_sizes.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/mix_compute_and_non_compute.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_variable_group_size/execution/global-invocation-id.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_variable_group_size/execution/separate-global-id.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_variable_group_size/execution/separate-global-id-2.shader_test" \
                            "$T8132_PIGLIT_GL_TEST_DIR"/*.shader_test
                        ;;
                    linker-basic)
                        set -- \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/one_local_work_size.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/no_local_work_size.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/matched_local_work_sizes.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/mismatched_local_work_sizes.shader_test" \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker/mix_compute_and_non_compute.shader_test"
                        ;;
                    linker)
                        set -- "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker"/*.shader_test
                        ;;
                    execution)
                        set -- "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/execution"/*.shader_test
                        ;;
                    all)
                        set -- \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/linker"/*.shader_test \
                            "$T8132_PIGLIT_ROOT/tests/spec/arb_compute_shader/execution"/*.shader_test
                        ;;
                    *)
                        echo "unknown T8132_PIGLIT_GL_SUBSET: $T8132_PIGLIT_GL_SUBSET" >&2
                        exit 2
                        ;;
                esac
            fi
            exec env \
                LD_PRELOAD="$T8132_PIGLIT_CHILD_LD_PRELOAD" \
                LD_LIBRARY_PATH="$T8132_PIGLIT_CHILD_LD_LIBRARY_PATH" \
                PYTHONPATH="$T8132_PIGLIT_CHILD_PYTHONPATH" \
                PYTHONUNBUFFERED=1 \
                M1N1_SHIM_ROOT="$T8132_PIGLIT_M1N1_ROOT" \
                M1N1DEVICE="${M1N1DEVICE:-/dev/m1n1}" \
                __EGL_VENDOR_LIBRARY_FILENAMES="$T8132_PIGLIT_CHILD_EGL_VENDOR" \
                MESA_LOADER_DRIVER_OVERRIDE=asahi \
                MESA_SHADER_CACHE_DISABLE=true \
                PIGLIT_PLATFORM=surfaceless_egl \
                PIGLIT_NO_WINDOW=1 \
                "$T8132_PIGLIT_RUNNER" \
                "$@" \
                -auto -report-subtests
        fi

        # Only the test object receives the shim environment from the profile.
        # In particular, the Piglit Python controller is not preloaded.
        export PIGLIT_BUILD_DIR="$T8132_PIGLIT_BUILD"
        export PIGLIT_SOURCE_DIR="$T8132_PIGLIT_ROOT"
        export PIGLIT_NO_FAST_SKIP=1
        exec "$T8132_PIGLIT_ROOT/piglit" run \
            --process-isolation false \
            --platform surfaceless_egl \
            --timeout "$T8132_PIGLIT_TIMEOUT" \
            -j 1 \
            "$T8132_PIGLIT_PROFILE" \
            "$T8132_PIGLIT_RESULTS"
    '
