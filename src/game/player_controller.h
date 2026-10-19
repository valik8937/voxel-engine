#pragma once

#include "core/math.h"

namespace vox {

class World;

inline constexpr float kPlayerHalfWidth = 0.3f;
inline constexpr float kPlayerHeight = 1.8f;
inline constexpr float kPlayerEyeHeight = 1.62f;

struct PlayerMoveState {
    Vec3 position{}; // feet centre
    Vec3 velocity{};
    bool on_ground = false;
};

struct PlayerMoveInput {
    float move_forward = 0.0f;
    float move_right = 0.0f;
    bool jump = false;
    bool sprint = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

[[nodiscard]] Aabb player_aabb(Vec3 feet);
[[nodiscard]] bool player_collides(const World& world, Vec3 feet);

void step_player(const World& world, PlayerMoveState& state, const PlayerMoveInput& input, float dt);

} // namespace vox
