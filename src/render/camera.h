#pragma once

#include "core/math.h"

namespace vox {

struct Camera {
    Vec3 position{0.0f, 80.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fov_degrees = 70.0f;
    float near_plane = 0.05f;
    float far_plane = 1024.0f;

    [[nodiscard]] Vec3 forward() const noexcept {
        const float cp = std::cos(pitch);
        return Vec3{std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
    }

    [[nodiscard]] Vec3 right() const noexcept {
        return normalize(cross(forward(), Vec3{0.0f, 1.0f, 0.0f}));
    }

    [[nodiscard]] Mat4 view_projection(float aspect) const noexcept {
        const Mat4 view = look_at(position, position + forward(), Vec3{0.0f, 1.0f, 0.0f});
        const Mat4 projection = perspective(fov_degrees * kPi / 180.0f, aspect,
                                            near_plane, far_plane);
        return projection * view;
    }
};

} // namespace vox
