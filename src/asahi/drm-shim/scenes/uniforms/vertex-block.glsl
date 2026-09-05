#version 300 es
// SPDX-License-Identifier: MIT
precision highp float;
layout(std140) uniform TransformBlock { mat4 u_transform; };
out vec3 color;
void main() {
    int id = gl_VertexID;
    float x = id == 0 ? -.75 : (id == 1 ? .75 : 0.0);
    float y = id == 2 ? .75 : -.75;
    gl_Position = u_transform * vec4(x, y, 0.0, 1.0);
    color = id == 0 ? vec3(1,0,0) : (id == 1 ? vec3(0,1,0) : vec3(0,0,1));
}
