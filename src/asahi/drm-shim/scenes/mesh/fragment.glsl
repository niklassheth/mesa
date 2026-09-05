// SPDX-License-Identifier: MIT
#version 300 es
precision highp float;
in vec3 color;
uniform vec4 u_tint;
layout(location=0) out vec4 frag_color;
void main() {
    frag_color = vec4(color * u_tint.rgb, 1.0);
}
