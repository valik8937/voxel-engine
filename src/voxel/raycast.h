#pragma once

#include "core/math.h"
#include "voxel/block.h"

namespace vox {

class World;

struct RaycastHit {
    bool hit = false;
    int x = 0;
    int y = 0;
    int z = 0;
    int normal_x = 0;
    int normal_y = 0;
    int normal_z = 0;
    BlockId block = blocks::Air;
};

[[nodiscard]] RaycastHit raycast_voxels(const World& world, Vec3 origin, Vec3 direction,
                                        float max_distance);

} // namespace vox
