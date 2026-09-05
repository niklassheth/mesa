#version 300 es
// SPDX-License-Identifier: MIT
precision highp float;
out vec3 color;

void main()
{
    float id = float(gl_VertexID);
    float triangle = floor((id + 0.25) / 3.0);
    float corner = id - 3.0 * triangle;
    float row = floor((triangle + 0.25) / 10.0);
    float column = triangle - 10.0 * row;
    float x = 16.25 + 48.0 * column +
              (corner < 0.5 ? 6.0 : (corner < 1.5 ? 42.0 : 24.0));
    float y = 16.0 + 48.0 * row + (corner < 1.5 ? 40.0 : 6.0);
    gl_Position = vec4(x / 256.0 - 1.0, 1.0 - y / 256.0, 0.0, 1.0);
    color = vec3((column + 1.0) / 12.0, (row + 1.0) / 12.0,
                 0.2 + 0.3 * corner);
}
