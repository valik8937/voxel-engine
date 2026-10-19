#version 450

layout(set = 1, binding = 0) uniform Camera {
    mat4 view_proj;
} camera;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_world;

void main() {
    v_color = in_color;
    v_world = in_position;
    gl_Position = camera.view_proj * vec4(in_position, 1.0);
}
