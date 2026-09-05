#version 300 es
precision highp float;
in vec3 third_value;
in vec3 first_value;
in vec3 second_value;
uniform vec4 u_tint;
layout(location=0) out vec4 frag_color;
void main()
{
    frag_color = vec4((first_value * .2 + second_value * .3 + third_value.xxx * .4) * u_tint.rgb, 1);
}
