#include "render/chunk_mesh.h"

#include "voxel/block.h"
#include "voxel/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace vox {
namespace {

constexpr int kPadSize = kChunkSizeX + 2;

struct FaceDef {
    int normal_axis;
    int u_axis;
    int v_axis;
    int normal[3];
    int base[3];
    int u[3];
    int v[3];
    float light;
};

// Cross(u, v) == normal for every face. Vertices are emitted in CCW order when
// viewed from outside the block.
constexpr std::array<FaceDef, 6> kFaces{{
    {0, 1, 2, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.72f},
    {0, 2, 1, {-1, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0.72f},
    {1, 2, 0, {0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, 1.00f},
    {1, 0, 2, {0, -1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 0.50f},
    {2, 0, 1, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 0.82f},
    {2, 1, 0, {0, 0, -1}, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 0.82f},
}};

class PaddedVolume {
public:
    PaddedVolume() : blocks_(static_cast<std::size_t>(kPadSize) * kPadSize * kPadSize,
                             blocks::Air) {}

    void reset() noexcept {
        std::fill(blocks_.begin(), blocks_.end(), blocks::Air);
    }

    [[nodiscard]] BlockId at(int x, int y, int z) const noexcept {
        if (static_cast<unsigned>(x) >= kPadSize ||
            static_cast<unsigned>(y) >= kPadSize ||
            static_cast<unsigned>(z) >= kPadSize) {
            return blocks::Air;
        }
        return blocks_[index(x, y, z)];
    }

    void set(int x, int y, int z, BlockId block) noexcept {
        if (static_cast<unsigned>(x) < kPadSize &&
            static_cast<unsigned>(y) < kPadSize &&
            static_cast<unsigned>(z) < kPadSize) {
            blocks_[index(x, y, z)] = block;
        }
    }

    [[nodiscard]] bool opaque(int x, int y, int z) const noexcept {
        return block_is_opaque(at(x, y, z));
    }

private:
    static std::size_t index(int x, int y, int z) noexcept {
        return (static_cast<std::size_t>(y) * kPadSize + static_cast<std::size_t>(z)) *
                   kPadSize +
               static_cast<std::size_t>(x);
    }

    std::vector<BlockId> blocks_;
};

struct FaceCell {
    BlockId block = blocks::Air;
    std::uint8_t ao[4]{0, 0, 0, 0};

    bool operator==(const FaceCell&) const = default;
};

int axis_value(const int p[3], int axis) {
    return p[axis];
}

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
    mesh.bounds.min = Vec3{static_cast<float>(coord.x * kChunkSizeX),
                           static_cast<float>(coord.y * kChunkSizeY),
                           static_cast<float>(coord.z * kChunkSizeZ)};
    mesh.bounds.max = mesh.bounds.min + Vec3{static_cast<float>(kChunkSizeX),
                                             static_cast<float>(kChunkSizeY),
                                             static_cast<float>(kChunkSizeZ)};

    // Reuse the ~80 KB padded volume and the greedy-mesh scratch buffers between
    // chunk rebuilds. This avoids one heap allocation per rebuilt chunk.
    thread_local PaddedVolume volume;
    thread_local std::vector<FaceCell> cells;
    thread_local std::vector<std::uint8_t> used;

    volume.reset();

    const auto neighbour_coords = neighbours_3x3x3(coord);
    std::array<std::shared_ptr<Chunk>, 27> neighbour_chunks{};
    for (std::size_t i = 0; i < neighbour_chunks.size(); ++i) {
        neighbour_chunks[i] = world.get(neighbour_coords[i]);
    }

    for (int pz = 0; pz < kPadSize; ++pz) {
        for (int py = 0; py < kPadSize; ++py) {
            for (int px = 0; px < kPadSize; ++px) {
                const int local_x = px - 1;
                const int local_y = py - 1;
                const int local_z = pz - 1;
                const int chunk_dx = local_x < 0 ? -1 : (local_x >= kChunkSizeX ? 1 : 0);
                const int chunk_dy = local_y < 0 ? -1 : (local_y >= kChunkSizeY ? 1 : 0);
                const int chunk_dz = local_z < 0 ? -1 : (local_z >= kChunkSizeZ ? 1 : 0);
                const int cx = local_x < 0 ? kChunkSizeX - 1
                                           : (local_x >= kChunkSizeX ? 0 : local_x);
                const int cy = local_y < 0 ? kChunkSizeY - 1
                                           : (local_y >= kChunkSizeY ? 0 : local_y);
                const int cz = local_z < 0 ? kChunkSizeZ - 1
                                           : (local_z >= kChunkSizeZ ? 0 : local_z);

                const std::size_t neighbour_index =
                    static_cast<std::size_t>(chunk_dy + 1) * 9 +
                    static_cast<std::size_t>(chunk_dz + 1) * 3 +
                    static_cast<std::size_t>(chunk_dx + 1);
                const auto& chunk = neighbour_chunks[neighbour_index];
                if (!chunk) {
                    continue;
                }
                volume.set(px, py, pz, chunk->get(cx, cy, cz));
            }
        }
    }

    const auto chunk = world.get(coord);
    if (!chunk) {
        return mesh;
    }
    mesh.revision = chunk->revision();

    const Vec3 chunk_origin{static_cast<float>(coord.x * kChunkSizeX),
                            static_cast<float>(coord.y * kChunkSizeY),
                            static_cast<float>(coord.z * kChunkSizeZ)};

    cells.assign(static_cast<std::size_t>(kChunkSizeX) * kChunkSizeY * kChunkSizeZ,
                 FaceCell{});
    used.assign(cells.size(), 0);

    mesh.vertices.reserve(16 * 1024);
    mesh.indices.reserve(24 * 1024);

    for (const FaceDef& face : kFaces) {
        std::fill(cells.begin(), cells.end(), FaceCell{});
        std::fill(used.begin(), used.end(), 0);

        for (int z = 0; z < kChunkSizeZ; ++z) {
            for (int y = 0; y < kChunkSizeY; ++y) {
                for (int x = 0; x < kChunkSizeX; ++x) {
                    const int p[3] = {x, y, z};
                    const int px = x + 1;
                    const int py = y + 1;
                    const int pz = z + 1;
                    const BlockId block = volume.at(px, py, pz);
                    if (block == blocks::Air) {
                        continue;
                    }

                    const BlockId neighbour = volume.at(px + face.normal[0],
                                                        py + face.normal[1],
                                                        pz + face.normal[2]);
                    if (block_is_liquid(block)) {
                        if (neighbour != blocks::Air) {
                            continue;
                        }
                    } else if (block_is_opaque(neighbour)) {
                        continue;
                    }

                    const int s = axis_value(p, face.normal_axis);
                    const int u = axis_value(p, face.u_axis);
                    const int v = axis_value(p, face.v_axis);
                    FaceCell& cell = cells[(static_cast<std::size_t>(s) * kChunkSizeZ + v) *
                                               kChunkSizeX +
                                           u];
                    cell.block = block;
                    cell.ao[0] = static_cast<std::uint8_t>(
                        corner_ao(volume, px, py, pz, face, 0, 0));
                    cell.ao[1] = static_cast<std::uint8_t>(
                        corner_ao(volume, px, py, pz, face, 1, 0));
                    cell.ao[2] = static_cast<std::uint8_t>(
                        corner_ao(volume, px, py, pz, face, 1, 1));
                    cell.ao[3] = static_cast<std::uint8_t>(
                        corner_ao(volume, px, py, pz, face, 0, 1));
                }
            }
        }

        for (int s = 0; s < 32; ++s) {
            for (int v = 0; v < 32; ++v) {
                for (int u = 0; u < 32; ++u) {
                    const std::size_t base =
                        (static_cast<std::size_t>(s) * kChunkSizeZ + v) * kChunkSizeX + u;
                    if (cells[base].block == blocks::Air || used[base] != 0) {
                        continue;
                    }

                    const FaceCell key = cells[base];
                    int width = 1;
                    while (u + width < 32) {
                        const std::size_t index =
                            (static_cast<std::size_t>(s) * kChunkSizeZ + v) * kChunkSizeX +
                            (u + width);
                        if (used[index] != 0 || cells[index] != key) {
                            break;
                        }
                        ++width;
                    }

                    int height = 1;
                    while (v + height < 32) {
                        bool matches = true;
                        for (int dx = 0; dx < width; ++dx) {
                            const std::size_t index =
                                (static_cast<std::size_t>(s) * kChunkSizeZ + (v + height)) *
                                    kChunkSizeX +
                                (u + dx);
                            if (used[index] != 0 || cells[index] != key) {
                                matches = false;
                                break;
                            }
                        }
                        if (!matches) {
                            break;
                        }
                        ++height;
                    }

                    for (int dv = 0; dv < height; ++dv) {
                        for (int du = 0; du < width; ++du) {
                            const std::size_t index =
                                (static_cast<std::size_t>(s) * kChunkSizeZ + (v + dv)) *
                                    kChunkSizeX +
                                (u + du);
                            used[index] = 1;
                        }
                    }

                    int p[3]{};
                    p[face.normal_axis] = s;
                    p[face.u_axis] = u;
                    p[face.v_axis] = v;

                    const Vec3 origin =
                        chunk_origin + Vec3{static_cast<float>(p[0] + face.base[0]),
                                            static_cast<float>(p[1] + face.base[1]),
                                            static_cast<float>(p[2] + face.base[2])};
                    const Vec3 du = Vec3{static_cast<float>(face.u[0]),
                                         static_cast<float>(face.u[1]),
                                         static_cast<float>(face.u[2])} *
                                    static_cast<float>(width);
                    const Vec3 dv = Vec3{static_cast<float>(face.v[0]),
                                         static_cast<float>(face.v[1]),
                                         static_cast<float>(face.v[2])} *
                                    static_cast<float>(height);

                    const Vec3 positions[4] = {
                        origin,
                        origin + du,
                        origin + du + dv,
                        origin + dv,
                    };
                    const std::uint32_t base_index =
                        static_cast<std::uint32_t>(mesh.vertices.size());
                    for (int corner = 0; corner < 4; ++corner) {
                        const Color color = shaded_color(key.block, key.ao[corner],
                                                         face.light);
                        mesh.vertices.push_back(MeshVertex{
                            positions[corner].x, positions[corner].y, positions[corner].z,
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

    return mesh;
}

} // namespace vox
