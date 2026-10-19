#pragma once

#include "core/binary.h"
#include "voxel/block.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vox {

inline constexpr int kChunkSizeX = 32;
inline constexpr int kChunkSizeY = 256;
inline constexpr int kChunkSizeZ = 32;
inline constexpr int kChunkColumns = kChunkSizeX * kChunkSizeZ;
inline constexpr std::size_t kChunkVoxelCount =
    static_cast<std::size_t>(kChunkSizeX) * kChunkSizeY * kChunkSizeZ;

struct ChunkCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;

    constexpr bool operator==(const ChunkCoord&) const = default;

    [[nodiscard]] constexpr std::uint64_t key() const noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
               static_cast<std::uint32_t>(z);
    }
};

struct ChunkCoordHash {
    [[nodiscard]] std::size_t operator()(const ChunkCoord& coord) const noexcept {
        return static_cast<std::size_t>(coord.key() * 0x9e3779b97f4a7c15ull);
    }
};

constexpr ChunkCoord chunk_coord_at(std::int64_t world_x, std::int64_t world_z) noexcept {
    const auto floor_div = [](std::int64_t value, std::int64_t divisor) {
        const std::int64_t q = value / divisor;
        const std::int64_t r = value % divisor;
        return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
    };
    return ChunkCoord{
        static_cast<std::int32_t>(floor_div(world_x, kChunkSizeX)),
        static_cast<std::int32_t>(floor_div(world_z, kChunkSizeZ)),
    };
}

constexpr int local_x(std::int64_t world_x) noexcept {
    const int v = static_cast<int>(world_x % kChunkSizeX);
    return v < 0 ? v + kChunkSizeX : v;
}

constexpr int local_z(std::int64_t world_z) noexcept {
    const int v = static_cast<int>(world_z % kChunkSizeZ);
    return v < 0 ? v + kChunkSizeZ : v;
}

constexpr std::array<ChunkCoord, 9> neighbours_3x3(ChunkCoord center) noexcept {
    return {{
        {center.x - 1, center.z - 1}, {center.x, center.z - 1}, {center.x + 1, center.z - 1},
        {center.x - 1, center.z},     center,                   {center.x + 1, center.z},
        {center.x - 1, center.z + 1}, {center.x, center.z + 1}, {center.x + 1, center.z + 1},
    }};
}

struct Run {
    BlockId block = blocks::Air;
    std::uint16_t length = 0;
};

// Runtime chunk representation: a run-length encoded stream of columns.
// A column is 256 voxels from y=0 upwards. column_offsets has 1025 entries so
// that column c occupies runs[column_offsets[c] .. column_offsets[c + 1]).
class Chunk {
public:
    Chunk() = default;
    explicit Chunk(ChunkCoord coord) : coord_(coord) { rebuild_offsets(); }

    static Chunk make_flat(ChunkCoord coord, BlockId fill);

    static Chunk from_runs(ChunkCoord coord, std::uint32_t revision,
                           std::vector<Run> runs);

    [[nodiscard]] BlockId get(int x, int y, int z) const noexcept;
    [[nodiscard]] BlockId get_world(std::int64_t x, int y, std::int64_t z) const noexcept;
    bool set(int x, int y, int z, BlockId block);
    bool set_world(std::int64_t x, int y, std::int64_t z, BlockId block);

    [[nodiscard]] ChunkCoord coord() const noexcept { return coord_; }
    void set_coord(ChunkCoord coord) noexcept { coord_ = coord; }

    [[nodiscard]] std::uint32_t revision() const noexcept { return revision_; }
    void set_revision(std::uint32_t revision) noexcept { revision_ = revision; }
    void bump_revision() noexcept { ++revision_; }

    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void set_dirty(bool dirty) noexcept { dirty_ = dirty; }

    [[nodiscard]] const std::vector<Run>& runs() const noexcept { return runs_; }
    [[nodiscard]] const std::array<std::uint32_t, kChunkColumns + 1>& column_offsets() const noexcept {
        return column_offsets_;
    }

    [[nodiscard]] std::size_t run_count() const noexcept { return runs_.size(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

    // Returns the y of the highest non-air block, or -1 if the column is empty.
    [[nodiscard]] int highest_solid(int x, int z) const noexcept;
    [[nodiscard]] std::size_t column_block_count(int x, int z, BlockId block) const noexcept;

    void serialize(BinaryWriter& writer) const;
    [[nodiscard]] static bool deserialize(BinaryReader& reader, Chunk& out);

    [[nodiscard]] bool validate() const noexcept;

    void rebuild_offsets();

private:
    void decode_column(int x, int z, std::array<BlockId, kChunkSizeY>& out) const noexcept;
    void encode_column(const std::array<BlockId, kChunkSizeY>& column,
                       std::vector<Run>& out) const;

    ChunkCoord coord_{};
    std::uint32_t revision_ = 0;
    bool dirty_ = false;
    std::vector<Run> runs_;
    std::array<std::uint32_t, kChunkColumns + 1> column_offsets_{};
};

} // namespace vox
