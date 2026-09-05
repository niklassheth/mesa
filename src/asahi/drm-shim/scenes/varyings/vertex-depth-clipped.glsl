#version 300 es
precision highp float;
layout(location=0) in vec3 position;
layout(location=1) in vec4 vertex_color;
uniform mat4 u_transform;
out vec3 first_value;
out vec3 second_value;
out vec3 third_value;
void main()
{
    gl_Position = (u_transform * vec4(position, 1)) * (1. + position.x * .5);
    gl_Position.z = position.x * 4.;
    first_value = vertex_color.rgb;
    second_value = position * .25 + vec3(.3, .4, .5);
    third_value = vec3(position.x * position.x, position.y * position.y,
                       position.x * position.y) * .25 + vec3(.2, .3, .4);
}
