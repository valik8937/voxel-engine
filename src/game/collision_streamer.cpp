#include "game/collision_streamer.h"

#include "physics/physics_world.h"
#include "voxel/chunk_mesh.h"
#include "voxel/world.h"

#include <unordered_set>
#include <vector>

namespace vox {

CollisionStreamer::CollisionStreamer(const World& world, PhysicsWorld& physics)
    : world_(world), physics_(physics) {}

bool CollisionStreamer::update(std::span<const ChunkCoord> centers,
                               int horizontal_radius, int vertical_radius,
                               std::size_t max_adds) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
    desired.reserve(centers.size() *
                    static_cast<std::size_t>((horizontal_radius * 2 + 1) *
                                             (horizontal_radius * 2 + 1) *
                                             (vertical_radius * 2 + 1)));

    for (const ChunkCoord& center : centers) {
        for (int dy = -vertical_radius; dy <= vertical_radius; ++dy) {
            for (int dz = -horizontal_radius; dz <= horizontal_radius; ++dz) {
                for (int dx = -horizontal_radius; dx <= horizontal_radius; ++dx) {
                    desired.insert(
                        ChunkCoord{center.x + dx, center.y + dy, center.z + dz});
                }
            }
        }
    }

    bool changed = false;
    for (auto it = active_.begin(); it != active_.end();) {
        if (desired.find(*it) == desired.end()) {
            physics_.remove_static_chunk(*it);
            it = active_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    std::size_t added = 0;
    bool complete = true;
    for (const ChunkCoord& coord : desired) {
        if (active_.find(coord) != active_.end()) {
            continue;
        }
        if (added >= max_adds) {
            complete = false;
            break;
        }
        const auto chunk = world_.get(coord);
        if (!chunk) {
            complete = false;
            continue;
        }

        ChunkMesh mesh = mesh_chunk(world_, coord, false);
        physics_.add_static_chunk(coord, mesh);
        active_.insert(coord);
        ++added;
        changed = true;
    }

    if (changed) {
        physics_.optimize_broadphase();
    }
    return complete && active_.size() == desired.size();
}

bool CollisionStreamer::update(ChunkCoord center, int horizontal_radius,
                               int vertical_radius, std::size_t max_adds) {
    return update(std::span<const ChunkCoord>(&center, 1), horizontal_radius,
                  vertical_radius, max_adds);
}

void CollisionStreamer::invalidate(ChunkCoord coord) {
    if (active_.erase(coord) != 0) {
        physics_.remove_static_chunk(coord);
        physics_.optimize_broadphase();
    }
}

void CollisionStreamer::clear() {
    for (const ChunkCoord& coord : active_) {
        physics_.remove_static_chunk(coord);
    }
    active_.clear();
    physics_.optimize_broadphase();
}

bool CollisionStreamer::has_chunk(ChunkCoord coord) const {
    return active_.find(coord) != active_.end();
}

} // namespace vox
