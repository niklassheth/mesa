#version 300 es
precision highp float;
out vec3 prism;

void main()
{
    int id = gl_VertexID;
    // An asymmetric, tilted triangle. Unequal W exercises perspective-correct
    // interpolation while preserving these deliberately chosen screen points.
    float x = id == 0 ? -0.22 : (id == 1 ? -0.82 : 0.90);
    float y = id == 0 ?  0.88 : (id == 1 ? -0.72 : -0.45);
    float w = id == 0 ?  1.00 : (id == 1 ? 1.35 : 0.85);
    gl_Position = vec4(x * w, y * w, 0.0, w);
    prism = vec3(id == 0 ? 1.0 : 0.0,
                 id == 1 ? 1.0 : 0.0,
                 id == 2 ? 1.0 : 0.0);
}
