#pragma once

#include "voxel/chunk.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vox {

class World {
public:
    [[nodiscard]] std::shared_ptr<Chunk> get(ChunkCoord coord) const;
    [[nodiscard]] std::shared_ptr<const Chunk> get_const(ChunkCoord coord) const;

    void insert(std::shared_ptr<Chunk> chunk);
    bool remove(ChunkCoord coord);
    void clear();

    [[nodiscard]] std::size_t chunk_count() const;
    [[nodiscard]] std::vector<std::shared_ptr<Chunk>> snapshot() const;
    [[nodiscard]] std::vector<std::shared_ptr<Chunk>> dirty_snapshot() const;

    [[nodiscard]] BlockId get_block(std::int64_t x, std::int64_t y,
                                    std::int64_t z) const;
    bool set_block(std::int64_t x, std::int64_t y, std::int64_t z, BlockId block);

private:
    mutable std::mutex mutex_;
    std::unordered_map<ChunkCoord, std::shared_ptr<Chunk>, ChunkCoordHash> chunks_;
};

} // namespace vox
