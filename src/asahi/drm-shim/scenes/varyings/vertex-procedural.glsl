#version 300 es
precision highp float;
out vec4 first_value;
out vec4 second_value;
out vec4 third_value;
void main()
{
    vec3 position = vec3(mix(-.75, .75, float((gl_VertexID & 1) == 0)),
                         mix(-.5, .5, float(gl_VertexID == 0 || gl_VertexID >= 4)), 0);
    vec4 vertex_color = vec4(position.xy * .6 + .5, .375, 1);
    gl_Position = vec4(position, 1);
    first_value = vec4(vertex_color.rgb, position.x * .125 + .25);
    second_value = vec4(position * .25 + vec3(.3, .4, .5), position.y * .125 + .25);
    third_value = vec4(vec3(position.x * position.x, position.y * position.y,
                       position.x * position.y) * .25 + vec3(.2, .3, .4),
                       (position.x + position.y) * .125 + .5);
}
