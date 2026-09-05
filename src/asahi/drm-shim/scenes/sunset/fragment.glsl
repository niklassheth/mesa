#version 300 es
// SPDX-License-Identifier: MIT
precision highp float;
in vec3 world;
in vec3 face_normal;
in vec4 material;
uniform vec4 u_light;
uniform vec3 u_eye;
layout(location=0) out vec4 frag_color;
void main() {
    vec3 n = normalize(face_normal);
    vec3 l = u_light.xyz;
    vec3 view = normalize(u_eye - world);
    vec3 halfway = normalize(l + view);
    float spec = max(dot(n, halfway), 0.0);
    spec *= spec; spec *= spec; spec *= spec; spec *= spec;
    vec3 ambient = mix(vec3(0.13,0.055,0.055), vec3(0.10,0.17,0.29), n.y*0.5+0.5);
    vec3 lit = material.rgb * (ambient + vec3(1.0,0.46,0.20)*max(dot(n,l),0.0)*0.85);
    lit += vec3(1.0,0.65,0.32)*spec*clamp(material.w,0.0,1.0)*1.4;
    float fog = clamp(length(u_eye-world)*0.08 + max(-world.y-0.1,0.0)*0.28-0.17,0.0,0.65);
    lit = mix(lit,vec3(0.43,0.20,0.25),fog);
    float height = clamp(face_normal.y*0.5+0.5,0.0,1.0);
    vec3 sky = mix(vec3(0.8,0.30,0.13),vec3(0.065,0.055,0.18),height);
    vec2 sun_delta = face_normal.xy-vec2(-0.52,0.65);
    float r2 = dot(sun_delta,sun_delta);
    sky += vec3(1.0,0.55,0.18)*(0.025/(r2+0.035));
    sky += vec3(1.0,0.8,0.45)*clamp((0.009-r2)*500.0,0.0,1.0);
    frag_color = vec4(sqrt(clamp(mix(lit,sky,clamp(-material.w,0.0,1.0)),0.0,1.0)),1.0);
}
