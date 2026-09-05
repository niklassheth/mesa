#version 300 es
precision highp float;
in vec4 third_value;
in vec4 first_value;
in vec4 second_value;
uniform vec4 u_tint;
layout(location=0) out vec4 frag_color;
void main()
{
    frag_color = vec4((first_value.xyz * .2 + second_value.xyz * .3 + third_value.xyz * .4 + vec3(first_value.w, second_value.w, third_value.w) * .1) * u_tint.rgb, 1);
}
