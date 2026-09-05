#!/bin/sh
# SPDX-License-Identifier: MIT
# Reset and chainload m1n1 before invoking.
set -eu
if [ "$#" != 2 ]; then
    echo "usage: $0 PREPARED_ASSET_DIRECTORY NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
scene_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
asset_dir=$(CDPATH= cd -- "$1" && pwd)
mkdir -p -- "$2"
output_dir=$(CDPATH= cd -- "$2" && pwd)
if [ -e "$output_dir/render-0000-attachment-0.bin" ]; then
    echo "output directory already contains a render capture" >&2
    exit 2
fi
export T8132_GLES_VERTEX_SOURCE=$scene_dir/vertex.glsl
export T8132_GLES_FRAGMENT_SOURCE=$scene_dir/fragment.glsl
export T8132_GLES_ISLAND=$asset_dir/mesh.bin
export T8132_GLES_FRAME_DATA=$output_dir/frames.bin
export T8132_GLES_WIDTH=512 T8132_GLES_HEIGHT=512
export G16G_RENDER_ATTACHMENT_DUMP=$output_dir
cp "$asset_dir/asset.json" "$output_dir/asset.json"
"$scene_dir/../../run_t8132_gles_triangle.sh" >"$output_dir/run.log" 2>&1
