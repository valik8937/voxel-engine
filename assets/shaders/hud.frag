#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec2 centered = abs(v_uv - vec2(0.5));
    float thickness = 0.0022;
    float arm = 0.017;
    bool horizontal = centered.y < thickness && centered.x < arm;
    bool vertical = centered.x < thickness && centered.y < arm;
    if (!horizontal && !vertical) {
        discard;
    }
    out_color = vec4(1.0, 1.0, 1.0, 0.85);
}
