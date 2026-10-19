#pragma once

#include "voxel/chunk.h"

#include <cstddef>
#include <span>
#include <unordered_set>

namespace vox {

class World;
class PhysicsWorld;

// Maintains Jolt static triangle meshes for the chunks around one or more
// interest points. Only solid geometry is included (no water).
class CollisionStreamer {
public:
    CollisionStreamer(const World& world, PhysicsWorld& physics);

    // Adds up to max_adds new chunks and removes chunks outside the box.
    // Returns true when the desired set is fully loaded.
    bool update(std::span<const ChunkCoord> centers, int horizontal_radius,
                int vertical_radius, std::size_t max_adds = 8);
    bool update(ChunkCoord center, int horizontal_radius, int vertical_radius,
                std::size_t max_adds = 8);
    void invalidate(ChunkCoord coord);
    void clear();

    [[nodiscard]] bool has_chunk(ChunkCoord coord) const;
    [[nodiscard]] std::size_t active_chunk_count() const noexcept { return active_.size(); }

private:
    const World& world_;
    PhysicsWorld& physics_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> active_;
};

} // namespace vox
