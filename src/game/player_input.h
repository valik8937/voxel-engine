#pragma once

#include "core/math.h"

#include <algorithm>
#include <cmath>

namespace vox {

inline constexpr float kPlayerHalfWidth = 0.3f;
inline constexpr float kPlayerHeight = 1.8f;
inline constexpr float kPlayerEyeHeight = 1.62f;
inline constexpr float kPlayerWalkSpeed = 4.8f;
inline constexpr float kPlayerSprintSpeed = 7.6f;

struct PlayerMoveInput {
    float move_forward = 0.0f;
    float move_right = 0.0f;
    bool jump = false;
    bool sprint = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

[[nodiscard]] inline Vec3 player_wish_velocity(const PlayerMoveInput& input) {
    const float forward = std::clamp(input.move_forward, -1.0f, 1.0f);
    const float right = std::clamp(input.move_right, -1.0f, 1.0f);
    const Vec3 forward_dir{std::sin(input.yaw), 0.0f, -std::cos(input.yaw)};
    const Vec3 right_dir{std::cos(input.yaw), 0.0f, std::sin(input.yaw)};
    Vec3 wish = forward_dir * forward + right_dir * right;
    if (length(wish) > 1.0f) {
        wish = normalize(wish);
    }
    const float speed = input.sprint ? kPlayerSprintSpeed : kPlayerWalkSpeed;
    return wish * speed;
}

[[nodiscard]] inline Aabb player_aabb(Vec3 feet) {
    return Aabb{
        Vec3{feet.x - kPlayerHalfWidth, feet.y, feet.z - kPlayerHalfWidth},
        Vec3{feet.x + kPlayerHalfWidth, feet.y + kPlayerHeight, feet.z + kPlayerHalfWidth},
    };
}

} // namespace vox
