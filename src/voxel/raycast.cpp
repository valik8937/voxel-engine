#include "voxel/raycast.h"

#include "voxel/world.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vox {

RaycastHit raycast_voxels(const World& world, Vec3 origin, Vec3 direction,
                          float max_distance) {
    RaycastHit result;
    const float dir_length = length(direction);
    if (dir_length < 1.0e-6f || max_distance <= 0.0f) {
        return result;
    }
    direction = direction / dir_length;

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    const int step_x = direction.x > 0.0f ? 1 : (direction.x < 0.0f ? -1 : 0);
    const int step_y = direction.y > 0.0f ? 1 : (direction.y < 0.0f ? -1 : 0);
    const int step_z = direction.z > 0.0f ? 1 : (direction.z < 0.0f ? -1 : 0);

    const float inf = std::numeric_limits<float>::infinity();
    const float delta_x = step_x != 0 ? std::abs(1.0f / direction.x) : inf;
    const float delta_y = step_y != 0 ? std::abs(1.0f / direction.y) : inf;
    const float delta_z = step_z != 0 ? std::abs(1.0f / direction.z) : inf;

    const auto boundary = [](float position) {
        return std::floor(position) + 1.0f;
    };

    float t_max_x = inf;
    float t_max_y = inf;
    float t_max_z = inf;
    if (step_x > 0) {
        t_max_x = (boundary(origin.x) - origin.x) / direction.x;
    } else if (step_x < 0) {
        t_max_x = (std::floor(origin.x) - origin.x) / direction.x;
    }
    if (step_y > 0) {
        t_max_y = (boundary(origin.y) - origin.y) / direction.y;
    } else if (step_y < 0) {
        t_max_y = (std::floor(origin.y) - origin.y) / direction.y;
    }
    if (step_z > 0) {
        t_max_z = (boundary(origin.z) - origin.z) / direction.z;
    } else if (step_z < 0) {
        t_max_z = (std::floor(origin.z) - origin.z) / direction.z;
    }

    float distance = 0.0f;
    int normal_x = 0;
    int normal_y = 0;
    int normal_z = 0;

    for (int iteration = 0; iteration < 512; ++iteration) {
        const BlockId block = world.get_block(x, y, z);
        if (block_is_solid(block)) {
            result.hit = true;
            result.x = x;
            result.y = y;
            result.z = z;
            result.normal_x = normal_x;
            result.normal_y = normal_y;
            result.normal_z = normal_z;
            result.block = block;
            return result;
        }

        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            distance = t_max_x;
            t_max_x += delta_x;
            x += step_x;
            normal_x = -step_x;
            normal_y = 0;
            normal_z = 0;
        } else if (t_max_y < t_max_z) {
            distance = t_max_y;
            t_max_y += delta_y;
            y += step_y;
            normal_x = 0;
            normal_y = -step_y;
            normal_z = 0;
        } else {
            distance = t_max_z;
            t_max_z += delta_z;
            z += step_z;
            normal_x = 0;
            normal_y = 0;
            normal_z = -step_z;
        }

        if (distance > max_distance) {
            break;
        }
    }
    return result;
}

} // namespace vox
