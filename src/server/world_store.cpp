#include "server/world_store.h"

#include "core/hash.h"
#include "core/log.h"
#include "core/zstd.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vox {
namespace {

constexpr std::uint32_t kStoreMagic = 0x314b4843u; // "CHK1"
constexpr std::uint16_t kStoreVersion = 2;
constexpr std::size_t kMaxStorePayload = 8u * 1024u * 1024u;

} // namespace

WorldStore::WorldStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::filesystem::path WorldStore::chunk_path(ChunkCoord coord) const {
    return directory_ / ("chunk_" + std::to_string(coord.x) + "_" +
                         std::to_string(coord.y) + "_" + std::to_string(coord.z) +
                         ".vch");
}

bool WorldStore::load(World& world) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        VOX_LOG_ERROR("Cannot create world directory '{}': {}", directory_.string(), ec.message());
        return false;
    }

    std::size_t loaded = 0;
    std::size_t failed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".vch") {
            continue;
        }

        std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
        if (!file) {
            ++failed;
            continue;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) > kMaxStorePayload) {
            ++failed;
            continue;
        }
        file.seekg(0);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            ++failed;
            continue;
        }

        BinaryReader reader(bytes);
        const std::uint32_t magic = reader.u32();
        const std::uint16_t version = reader.u16();
        const std::int32_t x = reader.i32();
        const std::int32_t y = reader.i32();
        const std::int32_t z = reader.i32();
        const std::uint32_t revision = reader.u32();
        const std::uint32_t uncompressed_size = reader.u32();
        const std::uint32_t compressed_size = reader.u32();
        const std::uint32_t checksum = reader.u32();
        if (!reader.ok() || magic != kStoreMagic || version != kStoreVersion ||
            uncompressed_size == 0 || uncompressed_size > kMaxStorePayload ||
            compressed_size > kMaxStorePayload) {
            ++failed;
            continue;
        }
        const auto compressed = reader.bytes(compressed_size);
        if (!reader.ok() || compressed.size() != compressed_size) {
            ++failed;
            continue;
        }

        auto raw = decompressor_.decompress(compressed, uncompressed_size);
        if (!raw || crc32(*raw) != checksum) {
            ++failed;
            continue;
        }

        BinaryReader chunk_reader(*raw);
        Chunk chunk;
        if (!Chunk::deserialize(chunk_reader, chunk)) {
            ++failed;
            continue;
        }
        if (chunk.coord() != ChunkCoord{x, y, z} || chunk.revision() != revision) {
            ++failed;
            continue;
        }
        chunk.set_dirty(false);
        world.insert(std::make_shared<Chunk>(std::move(chunk)));
        ++loaded;
    }

    if (failed != 0) {
        VOX_LOG_WARN("Loaded {} chunks, {} files failed", loaded, failed);
    } else {
        VOX_LOG_INFO("Loaded {} saved chunks from '{}'", loaded, directory_.string());
    }
    return failed == 0;
}

bool WorldStore::save_chunk(const Chunk& chunk) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        VOX_LOG_ERROR("Cannot create world directory '{}': {}", directory_.string(), ec.message());
        return false;
    }

    BinaryWriter chunk_writer;
    chunk.serialize(chunk_writer);

    auto compressed = compressor_.compress(chunk_writer.data());
    if (!compressed) {
        VOX_LOG_ERROR("Zstd compression failed for chunk {},{}", chunk.coord().x, chunk.coord().z);
        return false;
    }

    BinaryWriter file_writer(32 + compressed->size());
    file_writer.u32(kStoreMagic);
    file_writer.u16(kStoreVersion);
    file_writer.i32(chunk.coord().x);
    file_writer.i32(chunk.coord().y);
    file_writer.i32(chunk.coord().z);
    file_writer.u32(chunk.revision());
    file_writer.u32(static_cast<std::uint32_t>(chunk_writer.size()));
    file_writer.u32(static_cast<std::uint32_t>(compressed->size()));
    file_writer.u32(crc32(chunk_writer.data()));
    file_writer.bytes(*compressed);

    const auto path = chunk_path(chunk.coord());
    const auto temp_path = path.string() + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            VOX_LOG_ERROR("Cannot open '{}' for writing", temp_path);
            return false;
        }
        file.write(reinterpret_cast<const char*>(file_writer.data().data()),
                   static_cast<std::streamsize>(file_writer.size()));
        if (!file) {
            VOX_LOG_ERROR("Cannot write '{}'", temp_path);
            return false;
        }
    }

    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        VOX_LOG_ERROR("Cannot rename '{}' to '{}': {}", temp_path, path.string(), ec.message());
        return false;
    }
    return true;
}

std::size_t WorldStore::save_dirty(World& world) {
    std::size_t saved = 0;
    for (const auto& chunk : world.dirty_snapshot()) {
        if (chunk && save_chunk(*chunk)) {
            chunk->set_dirty(false);
            ++saved;
        }
    }
    if (saved != 0) {
        VOX_LOG_INFO("Saved {} dirty chunks", saved);
    }
    return saved;
}

} // namespace vox
