#version 300 es
precision highp float;
out vec3 color;

void main()
{
    int id = gl_VertexID;
    bool right = id >= 3;
    int corner = right ? id - 3 : id;
    float x_left = corner == 0 ? -0.875 : (corner == 1 ? -0.125 : -0.5);
    float y_left = corner == 2 ? 0.75 : -0.625;
    float x_right = corner == 0 ? 0.125 : (corner == 1 ? 0.5 : 0.875);
    float y_right = corner == 1 ? -0.75 : 0.625;
    // Quarter-pixel offset avoids sample centers exactly on sloping edges.
    gl_Position = vec4((right ? x_right : x_left) + 0.0009765625,
                       right ? y_right : y_left, 0.0, 1.0);

    vec3 cold = corner == 0 ? vec3(0.0, 0.9, 0.9) :
               (corner == 1 ? vec3(0.05, 0.15, 0.8) : vec3(0.4, 1.0, 0.5));
    vec3 warm = corner == 0 ? vec3(1.0, 0.25, 0.02) :
               (corner == 1 ? vec3(0.7, 0.02, 0.5) : vec3(1.0, 0.8, 0.08));
    color = right ? warm : cold;
}
