#pragma once

#include "core/math.h"
#include "voxel/chunk.h"

#include <cstdint>
#include <memory>
#include <span>

namespace vox {

using PhysicsBody = std::uint32_t;
inline constexpr PhysicsBody kInvalidPhysicsBody = 0;

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();

    // positions_xyz is a flat array of x,y,z floats; indices index vertices.
    void add_static_triangles(ChunkCoord coord,
                              std::span<const float> positions_xyz,
                              std::span<const std::uint32_t> indices);
    void remove_static_chunk(ChunkCoord coord);

    [[nodiscard]] PhysicsBody add_box(Vec3 position, Vec3 half_extents, bool dynamic);
    void remove_body(PhysicsBody body);
    [[nodiscard]] Vec3 body_position(PhysicsBody body) const;

    void step(float dt);

    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vox
