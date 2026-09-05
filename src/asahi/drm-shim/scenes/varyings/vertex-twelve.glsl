#version 300 es
precision highp float;
layout(location=0) in vec3 position;
layout(location=1) in vec4 vertex_color;
uniform mat4 u_transform;
out vec4 first_value;
out vec4 second_value;
out vec4 third_value;
void main()
{
    gl_Position = u_transform * vec4(position, 1);
    first_value = vec4(vertex_color.rgb, position.x * .125 + .25);
    second_value = vec4(position * .25 + vec3(.3, .4, .5), position.y * .125 + .25);
    third_value = vec4(vec3(position.x * position.x, position.y * position.y,
                       position.x * position.y) * .25 + vec3(.2, .3, .4),
                       (position.x + position.y) * .125 + .5);
}
