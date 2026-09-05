#!/bin/sh
# SPDX-License-Identifier: MIT
# Caller must reset and chainload m1n1 before starting a hardware process.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi
scene_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
output_dir=$(CDPATH= cd -- "$1" && pwd)
export T8132_GLES_VERTEX_SOURCE=$scene_dir/vertex.glsl
export T8132_GLES_FRAGMENT_SOURCE=$scene_dir/fragment.glsl
export T8132_GLES_WIDTH=512 T8132_GLES_HEIGHT=512 T8132_GLES_FRAMES=2
export T8132_GLES_VERTICES=300 T8132_GLES_DRAWS=1
export G16G_RENDER_ATTACHMENT_DUMP=$output_dir
"$scene_dir/../../run_t8132_gles_triangle.sh" >"$output_dir/run.log" 2>&1
for frame in 0000 0001; do
    python3 "$scene_dir/readback.py" \
        "$output_dir/render-$frame-attachment-0.bin" \
        --output "$output_dir/hundred-triangles-$frame"
done
