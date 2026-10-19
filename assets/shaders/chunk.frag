#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_world;

layout(set = 3, binding = 0) uniform Fog {
    vec4 fog_color;
    vec4 camera_pos;
    vec4 params; // x = fog start, y = fog end, z = time
} fog;

layout(location = 0) out vec4 out_color;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

void main() {
    vec3 color = v_color.rgb;

    // Subtle per-block variation; the hash is constant over a block so greedy
    // meshing can still merge faces later.
    float variation = hash13(floor(v_world)) * 0.08 - 0.04;
    color *= 1.0 + variation;

    float distance_to_camera = distance(v_world, fog.camera_pos.xyz);
    float fog_t = clamp((distance_to_camera - fog.params.x) /
                        max(fog.params.y - fog.params.x, 1.0), 0.0, 1.0);
    fog_t = fog_t * fog_t * (3.0 - 2.0 * fog_t);
    color = mix(color, fog.fog_color.rgb, fog_t);

    out_color = vec4(color, 1.0);
}
