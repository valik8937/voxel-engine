#pragma once

#include "core/math.h"
#include "voxel/chunk.h"

#include <cstdint>
#include <vector>

namespace vox {

struct MeshVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct ChunkMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    Aabb bounds;
    std::uint32_t revision = 0;

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return vertices.capacity() * sizeof(MeshVertex) +
               indices.capacity() * sizeof(std::uint32_t);
    }
};

class World;

[[nodiscard]] ChunkMesh mesh_chunk(const World& world, ChunkCoord coord);

} // namespace vox
