#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 horizon = vec3(0.55, 0.72, 0.92);
    vec3 zenith = vec3(0.18, 0.38, 0.72);
    float t = clamp(v_uv.y, 0.0, 1.0);
    t = pow(t, 0.65);
    out_color = vec4(mix(horizon, zenith, t), 1.0);
}
