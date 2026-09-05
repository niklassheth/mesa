#version 300 es
precision highp float;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 base_color;
uniform mat4 u_transform;
uniform vec4 u_light;
out vec3 color;
void main()
{
    gl_Position = u_transform * vec4(position, 1.0);
    float diffuse = max(dot(normal, u_light.xyz), 0.0);
    // A small ambient term keeps the unlit side readable. Approximate gamma 2
    // encoding compensates for the current plain RGBA8 display attachment.
    color = sqrt(base_color * (u_light.w + (1.0 - u_light.w) * diffuse));
}
