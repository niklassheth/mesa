#!/bin/sh
# SPDX-License-Identifier: MIT
# Caller must reset and chainload m1n1 before each hardware process.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY [blocks]" >&2
    exit 2
fi
scene_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
output_dir=$(CDPATH= cd -- "$1" && pwd)
suffix=
unset T8132_GLES_UNIFORM_BLOCKS
if [ "${2:-}" = blocks ]; then
    suffix=-block
    export T8132_GLES_UNIFORM_BLOCKS=1
elif [ -n "${2:-}" ]; then
    echo "unknown mode: $2" >&2
    exit 2
fi
export T8132_GLES_VERTEX_SOURCE=$scene_dir/vertex$suffix.glsl
export T8132_GLES_FRAGMENT_SOURCE=$scene_dir/fragment$suffix.glsl
export T8132_GLES_WIDTH=512 T8132_GLES_HEIGHT=512 T8132_GLES_FRAMES=2
export T8132_GLES_VERTICES=3 T8132_GLES_DRAWS=2 T8132_GLES_UNIFORMS=1
export G16G_RENDER_ATTACHMENT_DUMP=$output_dir
"$scene_dir/../../run_t8132_gles_triangle.sh" >"$output_dir/run.log" 2>&1
python3 "$scene_dir/readback.py" "$output_dir/render-0000-attachment-0.bin" \
    --output "$output_dir/uniforms-frame0" --frame 0
python3 "$scene_dir/readback.py" "$output_dir/render-0001-attachment-0.bin" \
    --output "$output_dir/uniforms-frame1" --frame 1
