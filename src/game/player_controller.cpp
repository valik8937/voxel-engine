#include "game/player_controller.h"

#include "voxel/block.h"
#include "voxel/world.h"

#include <algorithm>
#include <cmath>

namespace vox {
namespace {

constexpr float kGravity = -24.0f;
constexpr float kWalkSpeed = 4.8f;
constexpr float kSprintSpeed = 7.6f;
constexpr float kJumpSpeed = 8.2f;
constexpr float kMaxFallSpeed = 55.0f;
constexpr float kSubstep = 0.2f;

bool solid_at(const World& world, int x, int y, int z) {
    return block_is_solid(world.get_block(x, y, z));
}

bool collides(const World& world, const Aabb& box) {
    const int min_x = static_cast<int>(std::floor(box.min.x + 1.0e-4f));
    const int max_x = static_cast<int>(std::floor(box.max.x - 1.0e-4f));
    const int min_y = static_cast<int>(std::floor(box.min.y + 1.0e-4f));
    const int max_y = static_cast<int>(std::floor(box.max.y - 1.0e-4f));
    const int min_z = static_cast<int>(std::floor(box.min.z + 1.0e-4f));
    const int max_z = static_cast<int>(std::floor(box.max.z - 1.0e-4f));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (solid_at(world, x, y, z)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void move_axis(const World& world, PlayerMoveState& state, int axis, float delta) {
    if (std::abs(delta) < 1.0e-7f) {
        return;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / kSubstep)));
    const float step = delta / static_cast<float>(steps);

    for (int i = 0; i < steps; ++i) {
        Vec3 candidate = state.position;
        if (axis == 0) {
            candidate.x += step;
        } else if (axis == 1) {
            candidate.y += step;
        } else {
            candidate.z += step;
        }

        if (collides(world, player_aabb(candidate))) {
            if (axis == 1 && delta < 0.0f) {
                state.on_ground = true;
            }
            if (axis == 0) {
                state.velocity.x = 0.0f;
            } else if (axis == 1) {
                state.velocity.y = 0.0f;
            } else {
                state.velocity.z = 0.0f;
            }
            return;
        }
        state.position = candidate;
    }
}

} // namespace

Aabb player_aabb(Vec3 feet) {
    return Aabb{
        Vec3{feet.x - kPlayerHalfWidth, feet.y, feet.z - kPlayerHalfWidth},
        Vec3{feet.x + kPlayerHalfWidth, feet.y + kPlayerHeight, feet.z + kPlayerHalfWidth},
    };
}

bool player_collides(const World& world, Vec3 feet) {
    return collides(world, player_aabb(feet));
}

void step_player(const World& world, PlayerMoveState& state, const PlayerMoveInput& input, float dt) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    if (dt <= 0.0f) {
        return;
    }

    const float forward = std::clamp(input.move_forward, -1.0f, 1.0f);
    const float right = std::clamp(input.move_right, -1.0f, 1.0f);

    const Vec3 forward_dir{std::sin(input.yaw), 0.0f, -std::cos(input.yaw)};
    const Vec3 right_dir{std::cos(input.yaw), 0.0f, std::sin(input.yaw)};
    Vec3 wish = forward_dir * forward + right_dir * right;
    if (length(wish) > 1.0f) {
        wish = normalize(wish);
    }

    const float speed = input.sprint ? kSprintSpeed : kWalkSpeed;
    state.velocity.x = wish.x * speed;
    state.velocity.z = wish.z * speed;

    if (input.jump && state.on_ground) {
        state.velocity.y = kJumpSpeed;
        state.on_ground = false;
    }

    state.velocity.y = std::max(kMaxFallSpeed * -1.0f, state.velocity.y + kGravity * dt);

    state.on_ground = false;
    move_axis(world, state, 0, state.velocity.x * dt);
    move_axis(world, state, 2, state.velocity.z * dt);
    move_axis(world, state, 1, state.velocity.y * dt);

    // Probe slightly below the feet to keep the grounded flag stable.
    if (!state.on_ground) {
        const Aabb probe = player_aabb(state.position + Vec3{0.0f, -0.06f, 0.0f});
        if (collides(world, probe)) {
            state.on_ground = true;
            if (state.velocity.y < 0.0f) {
                state.velocity.y = 0.0f;
            }
        }
    }
}

} // namespace vox
