#version 300 es
precision highp float;
uniform vec4 u_tint;
layout(location=0) out vec4 frag_color;
void main() { frag_color = vec4(vec3(.1, .2, .3) * u_tint.rgb, 1); }
