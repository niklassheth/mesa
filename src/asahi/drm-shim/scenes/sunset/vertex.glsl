#version 300 es
// SPDX-License-Identifier: MIT
precision highp float;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 base_color;
uniform mat4 u_transform;
out vec3 world;
out vec3 face_normal;
out vec4 material;
void main() {
    gl_Position = u_transform * vec4(position, 1.0);
    world = position;
    face_normal = normal;
    material = base_color;
}
