#pragma once

#include "core/zstd.h"
#include "voxel/world.h"

#include <cstddef>
#include <filesystem>

namespace vox {

class WorldStore {
public:
    explicit WorldStore(std::filesystem::path directory);

    [[nodiscard]] bool load(World& world);
    [[nodiscard]] bool save_chunk(const Chunk& chunk);
    [[nodiscard]] std::size_t save_dirty(World& world);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    [[nodiscard]] std::filesystem::path chunk_path(ChunkCoord coord) const;

    std::filesystem::path directory_;
    ZstdCompressor compressor_{3};
    ZstdDecompressor decompressor_;
};

} // namespace vox
