#include "voxel/chunk.h"

#include <algorithm>
#include <limits>

namespace vox {

namespace {
constexpr std::uint32_t kMaxRuns = static_cast<std::uint32_t>(kChunkColumns * kChunkSizeY);
}

Chunk Chunk::make_flat(ChunkCoord coord, BlockId fill) {
    Chunk chunk(coord);
    chunk.runs_.resize(kChunkColumns);
    for (auto& run : chunk.runs_) {
        run.block = fill;
        run.length = kChunkSizeY;
    }
    chunk.rebuild_offsets();
    return chunk;
}

Chunk Chunk::from_runs(ChunkCoord coord, std::uint32_t revision, std::vector<Run> runs) {
    Chunk chunk;
    chunk.coord_ = coord;
    chunk.revision_ = revision;
    chunk.runs_ = std::move(runs);
    chunk.rebuild_offsets();
    return chunk;
}

void Chunk::rebuild_offsets() {
    column_offsets_.fill(0);
    std::uint32_t run_index = 0;

    for (int column = 0; column < kChunkColumns; ++column) {
        column_offsets_[static_cast<std::size_t>(column)] = run_index;
        std::uint32_t column_height = 0;
        while (run_index < runs_.size() && column_height < kChunkSizeY) {
            const std::uint32_t length = runs_[run_index].length;
            if (length == 0 || column_height + length > kChunkSizeY) {
                break;
            }
            column_height += length;
            ++run_index;
        }
        if (column_height != kChunkSizeY) {
            // Malformed data; make the rest of the chunk empty and keep offsets valid.
            runs_.resize(run_index);
            for (int rest = column; rest < kChunkColumns; ++rest) {
                column_offsets_[static_cast<std::size_t>(rest)] = run_index;
            }
            column_offsets_[kChunkColumns] = run_index;
            return;
        }
    }
    column_offsets_[kChunkColumns] = static_cast<std::uint32_t>(runs_.size());
}

BlockId Chunk::get(int x, int y, int z) const noexcept {
    if (static_cast<unsigned>(x) >= kChunkSizeX ||
        static_cast<unsigned>(y) >= kChunkSizeY ||
        static_cast<unsigned>(z) >= kChunkSizeZ) {
        return blocks::Air;
    }

    const int column = z * kChunkSizeX + x;
    const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
    const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];

    int current_y = 0;
    for (std::uint32_t i = begin; i < end; ++i) {
        const Run& run = runs_[i];
        if (y < current_y + run.length) {
            return run.block;
        }
        current_y += run.length;
    }
    return blocks::Air;
}

BlockId Chunk::get_world(std::int64_t x, int y, std::int64_t z) const noexcept {
    return get(local_x(x), y, local_z(z));
}

bool Chunk::set(int x, int y, int z, BlockId block) {
    if (static_cast<unsigned>(x) >= kChunkSizeX ||
        static_cast<unsigned>(y) >= kChunkSizeY ||
        static_cast<unsigned>(z) >= kChunkSizeZ) {
        return false;
    }
    if (block >= blocks::Count) {
        return false;
    }

    const int column = z * kChunkSizeX + x;
    const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
    const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];

    std::array<BlockId, kChunkSizeY> blocks{};
    int current_y = 0;
    for (std::uint32_t i = begin; i < end; ++i) {
        const Run& run = runs_[i];
        for (int n = 0; n < run.length; ++n) {
            if (current_y + n < kChunkSizeY) {
                blocks[static_cast<std::size_t>(current_y + n)] = run.block;
            }
        }
        current_y += run.length;
    }

    if (blocks[static_cast<std::size_t>(y)] == block) {
        return false;
    }
    blocks[static_cast<std::size_t>(y)] = block;

    std::vector<Run> encoded;
    encode_column(blocks, encoded);

    std::vector<Run> new_runs;
    new_runs.reserve(runs_.size() - (end - begin) + encoded.size());
    new_runs.insert(new_runs.end(), runs_.begin(), runs_.begin() + begin);
    new_runs.insert(new_runs.end(), encoded.begin(), encoded.end());
    new_runs.insert(new_runs.end(), runs_.begin() + end, runs_.end());

    runs_ = std::move(new_runs);
    rebuild_offsets();
    ++revision_;
    dirty_ = true;
    return true;
}

bool Chunk::set_world(std::int64_t x, int y, std::int64_t z, BlockId block) {
    return set(local_x(x), y, local_z(z), block);
}

std::size_t Chunk::memory_bytes() const noexcept {
    return runs_.capacity() * sizeof(Run) + sizeof(column_offsets_) + sizeof(Chunk);
}

int Chunk::highest_solid(int x, int z) const noexcept {
    if (static_cast<unsigned>(x) >= kChunkSizeX ||
        static_cast<unsigned>(z) >= kChunkSizeZ) {
        return -1;
    }
    const int column = z * kChunkSizeX + x;
    const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
    const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];

    int current_y = 0;
    int highest = -1;
    for (std::uint32_t i = begin; i < end; ++i) {
        const Run& run = runs_[i];
        if (block_is_solid(run.block)) {
            highest = current_y + run.length - 1;
        }
        current_y += run.length;
    }
    return highest;
}

std::size_t Chunk::column_block_count(int x, int z, BlockId block) const noexcept {
    if (static_cast<unsigned>(x) >= kChunkSizeX ||
        static_cast<unsigned>(z) >= kChunkSizeZ) {
        return 0;
    }
    const int column = z * kChunkSizeX + x;
    const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
    const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];
    std::size_t count = 0;
    for (std::uint32_t i = begin; i < end; ++i) {
        if (runs_[i].block == block) {
            count += runs_[i].length;
        }
    }
    return count;
}

void Chunk::decode_column(int x, int z, std::array<BlockId, kChunkSizeY>& out) const noexcept {
    out.fill(blocks::Air);
    if (static_cast<unsigned>(x) >= kChunkSizeX ||
        static_cast<unsigned>(z) >= kChunkSizeZ) {
        return;
    }
    const int column = z * kChunkSizeX + x;
    const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
    const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];

    int y = 0;
    for (std::uint32_t i = begin; i < end && y < kChunkSizeY; ++i) {
        const Run& run = runs_[i];
        const int count = std::min<int>(run.length, kChunkSizeY - y);
        for (int n = 0; n < count; ++n) {
            out[static_cast<std::size_t>(y + n)] = run.block;
        }
        y += count;
    }
}

void Chunk::encode_column(const std::array<BlockId, kChunkSizeY>& column,
                          std::vector<Run>& out) const {
    out.clear();
    out.reserve(kChunkSizeY / 2 + 1);
    for (int y = 0; y < kChunkSizeY; ++y) {
        const BlockId block = column[static_cast<std::size_t>(y)];
        if (!out.empty() && out.back().block == block && out.back().length < std::numeric_limits<std::uint16_t>::max()) {
            ++out.back().length;
        } else {
            out.push_back(Run{block, 1});
        }
    }
}

void Chunk::serialize(BinaryWriter& writer) const {
    writer.i32(coord_.x);
    writer.i32(coord_.z);
    writer.u32(revision_);
    writer.u32(static_cast<std::uint32_t>(runs_.size()));
    for (const Run& run : runs_) {
        writer.u16(run.block);
        writer.u16(run.length);
    }
}

bool Chunk::deserialize(BinaryReader& reader, Chunk& out) {
    Chunk chunk;
    chunk.coord_.x = reader.i32();
    chunk.coord_.z = reader.i32();
    chunk.revision_ = reader.u32();
    const std::uint32_t run_count = reader.u32();
    if (!reader.ok() || run_count > kMaxRuns) {
        return false;
    }

    chunk.runs_.reserve(run_count);
    for (std::uint32_t i = 0; i < run_count; ++i) {
        Run run;
        run.block = reader.u16();
        run.length = reader.u16();
        if (!reader.ok() || run.length == 0 || run.length > kChunkSizeY ||
            run.block >= blocks::Count) {
            return false;
        }
        chunk.runs_.push_back(run);
    }
    if (!reader.ok()) {
        return false;
    }

    chunk.rebuild_offsets();
    if (!chunk.validate()) {
        return false;
    }
    out = std::move(chunk);
    return true;
}

bool Chunk::validate() const noexcept {
    if (column_offsets_[kChunkColumns] != runs_.size()) {
        return false;
    }
    for (int column = 0; column < kChunkColumns; ++column) {
        const std::uint32_t begin = column_offsets_[static_cast<std::size_t>(column)];
        const std::uint32_t end = column_offsets_[static_cast<std::size_t>(column) + 1];
        if (begin > end || end > runs_.size()) {
            return false;
        }
        std::uint32_t height = 0;
        for (std::uint32_t i = begin; i < end; ++i) {
            const Run& run = runs_[i];
            if (run.length == 0 || run.block >= blocks::Count) {
                return false;
            }
            height += run.length;
        }
        if (height != kChunkSizeY) {
            return false;
        }
    }
    return true;
}

} // namespace vox
