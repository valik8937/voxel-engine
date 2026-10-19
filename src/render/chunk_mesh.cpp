#include "render/chunk_mesh.h"

#include "voxel/block.h"
#include "voxel/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace vox {
namespace {

constexpr int kPadX = kChunkSizeX + 2;
constexpr int kPadY = kChunkSizeY + 2;
constexpr int kPadZ = kChunkSizeZ + 2;

struct FaceDef {
    int normal[3];
    int base[3];
    int u[3];
    int v[3];
    float light;
};

// Cross(u, v) == normal for every face. Vertices are emitted in CCW order when
// viewed from outside the block.
constexpr std::array<FaceDef, 6> kFaces{{
    {{1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.72f},
    {{-1, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0.72f},
    {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, 1.00f},
    {{0, -1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 0.50f},
    {{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 0.82f},
    {{0, 0, -1}, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 0.82f},
}};

class PaddedVolume {
public:
    PaddedVolume() : blocks_(static_cast<std::size_t>(kPadX) * kPadY * kPadZ, blocks::Air) {}

    [[nodiscard]] BlockId at(int x, int y, int z) const noexcept {
        if (static_cast<unsigned>(x) >= kPadX ||
            static_cast<unsigned>(y) >= kPadY ||
            static_cast<unsigned>(z) >= kPadZ) {
            return blocks::Air;
        }
        return blocks_[index(x, y, z)];
    }

    void set(int x, int y, int z, BlockId block) noexcept {
        if (static_cast<unsigned>(x) < kPadX &&
            static_cast<unsigned>(y) < kPadY &&
            static_cast<unsigned>(z) < kPadZ) {
            blocks_[index(x, y, z)] = block;
        }
    }

    [[nodiscard]] bool opaque(int x, int y, int z) const noexcept {
        return block_is_opaque(at(x, y, z));
    }

private:
    static std::size_t index(int x, int y, int z) noexcept {
        return (static_cast<std::size_t>(y) * kPadZ + static_cast<std::size_t>(z)) * kPadX +
               static_cast<std::size_t>(x);
    }

    std::vector<BlockId> blocks_;
};

int corner_ao(const PaddedVolume& volume, int px, int py, int pz,
              const FaceDef& face, int du, int dv) {
    const int nx = px + face.normal[0];
    const int ny = py + face.normal[1];
    const int nz = pz + face.normal[2];

    const int su = du != 0 ? 1 : -1;
    const int sv = dv != 0 ? 1 : -1;

    const int s1x = nx + face.u[0] * su;
    const int s1y = ny + face.u[1] * su;
    const int s1z = nz + face.u[2] * su;
    const int s2x = nx + face.v[0] * sv;
    const int s2y = ny + face.v[1] * sv;
    const int s2z = nz + face.v[2] * sv;
    const int cx = s1x + face.v[0] * sv;
    const int cy = s1y + face.v[1] * sv;
    const int cz = s1z + face.v[2] * sv;

    const bool side1 = volume.opaque(s1x, s1y, s1z);
    const bool side2 = volume.opaque(s2x, s2y, s2z);
    if (side1 && side2) {
        return 0;
    }
    const bool corner = volume.opaque(cx, cy, cz);
    return 3 - (static_cast<int>(side1) + static_cast<int>(side2) +
                static_cast<int>(corner));
}

Color shaded_color(BlockId block, int ao, float light) {
    const auto& ramp = block_definition(block).ramp;
    const Color base = ramp[static_cast<std::size_t>(std::clamp(ao, 0, 3))];
    return Color{
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(base.r) * light, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(base.g) * light, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(base.b) * light, 0.0f, 255.0f)),
        255,
    };
}

} // namespace

ChunkMesh mesh_chunk(const World& world, ChunkCoord coord) {
    ChunkMesh mesh;
    mesh.bounds.min = Vec3{static_cast<float>(coord.x * kChunkSizeX), 0.0f,
                           static_cast<float>(coord.z * kChunkSizeZ)};
    mesh.bounds.max = mesh.bounds.min + Vec3{static_cast<float>(kChunkSizeX),
                                             static_cast<float>(kChunkSizeY),
                                             static_cast<float>(kChunkSizeZ)};

    PaddedVolume volume;

    // Fill the padded volume from the chunk and its 8 neighbours. Padded index
    // 0 corresponds to local -1; 33 corresponds to local 32.
    for (int pz = 0; pz < kPadZ; ++pz) {
        for (int px = 0; px < kPadX; ++px) {
            const int local_x = px - 1;
            const int local_z = pz - 1;
            const int chunk_dx = local_x < 0 ? -1 : (local_x >= kChunkSizeX ? 1 : 0);
            const int chunk_dz = local_z < 0 ? -1 : (local_z >= kChunkSizeZ ? 1 : 0);
            const int cx = local_x < 0 ? kChunkSizeX - 1
                                       : (local_x >= kChunkSizeX ? 0 : local_x);
            const int cz = local_z < 0 ? kChunkSizeZ - 1
                                       : (local_z >= kChunkSizeZ ? 0 : local_z);

            const auto chunk = world.get(ChunkCoord{coord.x + chunk_dx, coord.z + chunk_dz});
            if (!chunk) {
                continue;
            }

            for (int py = 0; py < kPadY; ++py) {
                const int local_y = py - 1;
                if (local_y < 0 || local_y >= kChunkSizeY) {
                    continue;
                }
                volume.set(px, py, pz, chunk->get(cx, local_y, cz));
            }
        }
    }

    mesh.vertices.reserve(32 * 1024);
    mesh.indices.reserve(48 * 1024);

    const auto origin_x = static_cast<float>(coord.x * kChunkSizeX);
    const auto origin_z = static_cast<float>(coord.z * kChunkSizeZ);

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int y = 0; y < kChunkSizeY; ++y) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                const int px = x + 1;
                const int py = y + 1;
                const int pz = z + 1;
                const BlockId block = volume.at(px, py, pz);
                if (block == blocks::Air) {
                    continue;
                }

                const bool is_water = block_is_liquid(block);
                for (const FaceDef& face : kFaces) {
                    const BlockId neighbour = volume.at(px + face.normal[0],
                                                        py + face.normal[1],
                                                        pz + face.normal[2]);
                    if (is_water) {
                        if (neighbour != blocks::Air) {
                            continue;
                        }
                    } else if (block_is_opaque(neighbour)) {
                        continue;
                    }

                    const std::array<std::array<int, 2>, 4> corners{{
                        {{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}},
                    }};

                    const std::uint32_t base_index =
                        static_cast<std::uint32_t>(mesh.vertices.size());
                    for (const auto& [du, dv] : corners) {
                        const int ao = corner_ao(volume, px, py, pz, face, du, dv);
                        const Color color = shaded_color(block, ao, face.light);

                        const float vx = static_cast<float>(x + face.base[0] +
                                                            face.u[0] * du + face.v[0] * dv);
                        const float vy = static_cast<float>(y + face.base[1] +
                                                            face.u[1] * du + face.v[1] * dv);
                        const float vz = static_cast<float>(z + face.base[2] +
                                                            face.u[2] * du + face.v[2] * dv);
                        mesh.vertices.push_back(MeshVertex{
                            origin_x + vx, vy, origin_z + vz,
                            color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f,
                        });
                    }
                    mesh.indices.push_back(base_index + 0);
                    mesh.indices.push_back(base_index + 1);
                    mesh.indices.push_back(base_index + 2);
                    mesh.indices.push_back(base_index + 0);
                    mesh.indices.push_back(base_index + 2);
                    mesh.indices.push_back(base_index + 3);
                }
            }
        }
    }

    mesh.revision = world.get(coord) ? world.get(coord)->revision() : 0;
    return mesh;
}

} // namespace vox
