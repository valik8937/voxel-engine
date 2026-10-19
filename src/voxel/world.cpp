#include "voxel/world.h"

namespace vox {

std::shared_ptr<Chunk> World::get(ChunkCoord coord) const {
    const std::lock_guard lock(mutex_);
    const auto it = chunks_.find(coord);
    return it != chunks_.end() ? it->second : nullptr;
}

std::shared_ptr<const Chunk> World::get_const(ChunkCoord coord) const {
    return get(coord);
}

void World::insert(std::shared_ptr<Chunk> chunk) {
    if (!chunk) {
        return;
    }
    const ChunkCoord coord = chunk->coord();
    const std::lock_guard lock(mutex_);
    chunks_[coord] = std::move(chunk);
}

bool World::remove(ChunkCoord coord) {
    const std::lock_guard lock(mutex_);
    return chunks_.erase(coord) != 0;
}

void World::clear() {
    const std::lock_guard lock(mutex_);
    chunks_.clear();
}

std::size_t World::chunk_count() const {
    const std::lock_guard lock(mutex_);
    return chunks_.size();
}

std::vector<std::shared_ptr<Chunk>> World::snapshot() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Chunk>> result;
    result.reserve(chunks_.size());
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        result.push_back(chunk);
    }
    return result;
}

std::vector<std::shared_ptr<Chunk>> World::dirty_snapshot() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Chunk>> result;
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        if (chunk->dirty()) {
            result.push_back(chunk);
        }
    }
    return result;
}

BlockId World::get_block(std::int64_t x, std::int64_t y, std::int64_t z) const {
    const auto chunk = get(chunk_coord_at(x, y, z));
    if (!chunk) {
        return blocks::Air;
    }
    return chunk->get_world(x, y, z);
}

bool World::set_block(std::int64_t x, std::int64_t y, std::int64_t z, BlockId block) {
    const std::lock_guard lock(mutex_);
    const auto it = chunks_.find(chunk_coord_at(x, y, z));
    if (it == chunks_.end()) {
        return false;
    }
    return it->second->set_world(x, y, z, block);
}

} // namespace vox
