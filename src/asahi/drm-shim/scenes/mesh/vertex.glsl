// SPDX-License-Identifier: MIT
#version 300 es
precision highp float;
layout(location=0) in vec3 position;
layout(location=1) in vec4 vertex_color;
uniform mat4 u_transform;
out vec3 color;
void main() {
    gl_Position = u_transform * vec4(position, 1.0);
    color = vertex_color.rgb;
}
