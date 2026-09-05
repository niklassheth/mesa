#version 300 es
// SPDX-License-Identifier: MIT
precision highp float;
in vec3 color;
layout(std140) uniform TintBlock { vec4 u_tint; float u_time; };
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(color * u_tint.rgb + vec3(u_time,0,0),1); }
