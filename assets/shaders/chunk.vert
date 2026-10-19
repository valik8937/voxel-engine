#version 450

layout(set = 1, binding = 0) uniform Scene {
    mat4 view_proj;
    mat4 model;
} scene;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_world;

void main() {
    vec4 world = scene.model * vec4(in_position, 1.0);
    v_color = in_color;
    v_world = world.xyz;
    gl_Position = scene.view_proj * world;
}
