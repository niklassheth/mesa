#!/bin/sh
# SPDX-License-Identifier: MIT
# Reset and chainload m1n1 before invoking.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY [quad|depth|cube]" >&2
    exit 2
fi
scene_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
output_dir=$(CDPATH= cd -- "$1" && pwd)
suffix=
if [ -n "${T8132_GLES_FOUR_BUFFERS:-}" ]; then suffix=-four; fi
export T8132_GLES_VERTEX_SOURCE=$scene_dir/vertex$suffix.glsl
export T8132_GLES_FRAGMENT_SOURCE=$scene_dir/fragment.glsl
if [ -e "$output_dir/render-0000-attachment-0.bin" ]; then
    echo "output directory already contains a render capture" >&2
    exit 2
fi
export T8132_GLES_WIDTH=512 T8132_GLES_HEIGHT=512
export T8132_GLES_MESH=${2:-cube}
export G16G_RENDER_ATTACHMENT_DUMP=$output_dir
"$scene_dir/../../run_t8132_gles_triangle.sh" >"$output_dir/run.log" 2>&1
python3 "$scene_dir/validate.py" "$output_dir" "$T8132_GLES_MESH"
