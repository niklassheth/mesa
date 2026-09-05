#version 300 es
precision highp float;
in vec3 prism;
layout(location = 0) out vec4 out_color;

void main()
{
    float u = prism.z - prism.y;
    float v = prism.x;
    float edge = min(prism.x, min(prism.y, prism.z));
    float bend = 3.0 * u * (1.0 - u * u) + v * v;
    float wave = sin(27.0 * v + 12.0 * u + 2.8 * bend);
    float tide = 0.5 + 0.5 * wave;
    float fire = smoothstep(-0.4, 0.75, u + 0.3 * bend);

    vec3 cold = mix(vec3(0.015, 0.028, 0.12),
                    vec3(0.02, 0.72, 0.62), tide * tide);
    vec3 warm = mix(vec3(0.18, 0.018, 0.14),
                    vec3(1.0, 0.34, 0.075), tide);
    vec3 pigment = mix(cold, warm, fire);

    float thread = 1.0 - smoothstep(0.015, 0.11, abs(wave));
    float phase = 22.0 * edge + 0.23 * bend;
    float contours = phase - floor(phase);
    float etching = 1.0 - smoothstep(0.008, 0.045, min(contours, 1.0 - contours));
    float inset = smoothstep(0.018, 0.045, edge);
    pigment *= 0.30 + 0.70 * inset;
    pigment += thread * inset * vec3(0.32, 0.58, 0.52);
    pigment += etching * inset * vec3(0.12, 0.15, 0.23);

    // Two fine luminous rims; the remaining frame is dark polished glass.
    float rim = 1.0 - smoothstep(0.0025, 0.008, edge);
    float inner_rim = 1.0 - smoothstep(0.0015, 0.004, abs(edge - 0.023));
    vec3 rim_color = mix(vec3(0.20, 0.95, 0.95),
                         vec3(1.0, 0.65, 0.23), fire);
    pigment += (rim + 0.55 * inner_rim) * rim_color;
    out_color = vec4(pigment, 1.0);
}
