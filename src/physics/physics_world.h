#pragma once

#include "core/math.h"
#include "voxel/chunk.h"
#include "voxel/chunk_mesh.h"

#include <cstdint>
#include <memory>
#include <span>

namespace vox {

using PhysicsBody = std::uint32_t;
inline constexpr PhysicsBody kInvalidPhysicsBody = 0;

using PhysicsCharacter = std::uint32_t;
inline constexpr PhysicsCharacter kInvalidPhysicsCharacter = 0;

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
    void add_static_chunk(ChunkCoord coord, const ChunkMesh& mesh);
    void remove_static_chunk(ChunkCoord coord);
    void optimize_broadphase();

    [[nodiscard]] PhysicsBody add_box(Vec3 position, Vec3 half_extents, bool dynamic);
    void remove_body(PhysicsBody body);
    [[nodiscard]] Vec3 body_position(PhysicsBody body) const;

    [[nodiscard]] PhysicsCharacter create_character(Vec3 feet);
    void remove_character(PhysicsCharacter character);
    void step_character(PhysicsCharacter character, Vec3 horizontal_velocity, bool jump,
                        Vec3 gravity, float dt);
    void set_character_position(PhysicsCharacter character, Vec3 feet);
    void set_character_velocity(PhysicsCharacter character, Vec3 velocity);
    [[nodiscard]] Vec3 character_position(PhysicsCharacter character) const;
    [[nodiscard]] Vec3 character_velocity(PhysicsCharacter character) const;
    [[nodiscard]] bool character_on_ground(PhysicsCharacter character) const;

    void step(float dt);

    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vox
