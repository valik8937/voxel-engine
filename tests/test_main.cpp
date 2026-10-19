#include "core/binary.h"
#include "core/hash.h"
#include "core/job_system.h"
#include "core/zstd.h"
#include "net/client.h"
#include "net/protocol.h"
#include "net/server.h"
#include "voxel/block.h"
#include "voxel/chunk.h"
#include "voxel/world.h"
#include "worldgen/generator.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        ++g_checks;                                                                       \
        const auto check_a = (a);                                                         \
        const auto check_b = (b);                                                         \
        if (!(check_a == check_b)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);   \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using namespace std::chrono_literals;

std::uint32_t next_random(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void test_binary() {
    vox::BinaryWriter writer;
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x789abcdeu);
    writer.u64(0x0123456789abcdefull);
    writer.i32(-123456);
    writer.f32(3.5f);
    writer.string("hello");
    writer.varuint(300);

    vox::BinaryReader reader(writer.data());
    CHECK_EQ(reader.u8(), 0x12);
    CHECK_EQ(reader.u16(), 0x3456);
    CHECK_EQ(reader.u32(), 0x789abcdeu);
    CHECK_EQ(reader.u64(), 0x0123456789abcdefull);
    CHECK_EQ(reader.i32(), -123456);
    CHECK_EQ(reader.f32(), 3.5f);
    CHECK_EQ(reader.string(), std::string("hello"));
    CHECK_EQ(reader.varuint(), 300u);
    CHECK(reader.ok());
    CHECK_EQ(reader.remaining(), 0u);

    vox::BinaryReader truncated(std::span<const std::uint8_t>(writer.data()).first(3));
    truncated.u32();
    CHECK(!truncated.ok());

    const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    CHECK_EQ(vox::crc32(bytes), 0xb63cfbcd);
}

void test_zstd() {
    std::vector<std::uint8_t> input(100000);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>((i / 7) % 17);
    }

    vox::ZstdCompressor compressor(3);
    auto compressed = compressor.compress(input);
    CHECK(compressed.has_value());
    CHECK(compressed->size() < input.size());

    vox::ZstdDecompressor decompressor;
    auto output = decompressor.decompress(*compressed, input.size());
    CHECK(output.has_value());
    CHECK(*output == input);

    auto bad = decompressor.decompress(*compressed, input.size() - 1);
    CHECK(!bad.has_value());
}

void test_chunk_rle() {
    using namespace vox;

    Chunk chunk = Chunk::make_flat(ChunkCoord{0, 0}, blocks::Air);
    CHECK(chunk.validate());
    CHECK_EQ(chunk.run_count(), static_cast<std::size_t>(kChunkColumns));

    std::vector<BlockId> reference(kChunkVoxelCount, blocks::Air);
    std::uint32_t state = 0x12345678;
    std::uint32_t expected_revision = 0;
    for (int i = 0; i < 20000; ++i) {
        const int x = static_cast<int>(next_random(state) % kChunkSizeX);
        const int y = static_cast<int>(next_random(state) % kChunkSizeY);
        const int z = static_cast<int>(next_random(state) % kChunkSizeZ);
        const BlockId block = static_cast<BlockId>(1 + next_random(state) % (blocks::Count - 1));
        const std::size_t index =
            (static_cast<std::size_t>(z) * kChunkSizeX + x) * kChunkSizeY + y;
        if (reference[index] != block) {
            ++expected_revision;
        }
        reference[index] = block;
        chunk.set(x, y, z, block);
    }
    CHECK(chunk.validate());
    CHECK_EQ(chunk.revision(), expected_revision);

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            for (int y = 0; y < kChunkSizeY; ++y) {
                const std::size_t index =
                    (static_cast<std::size_t>(z) * kChunkSizeX + x) * kChunkSizeY + y;
                CHECK_EQ(chunk.get(x, y, z), reference[index]);
                if (g_failures > 20) {
                    return;
                }
            }
        }
    }

    BinaryWriter writer;
    chunk.serialize(writer);
    CHECK(writer.size() < kChunkVoxelCount);

    Chunk restored;
    BinaryReader reader(writer.data());
    CHECK(Chunk::deserialize(reader, restored));
    CHECK(restored.validate());
    CHECK(restored.coord() == chunk.coord());
    CHECK_EQ(restored.revision(), chunk.revision());
    for (int z = 0; z < kChunkSizeZ; z += 7) {
        for (int x = 0; x < kChunkSizeX; x += 5) {
            for (int y = 0; y < kChunkSizeY; y += 11) {
                CHECK_EQ(restored.get(x, y, z), chunk.get(x, y, z));
            }
        }
    }

    // Out-of-range access and invalid blocks are rejected.
    CHECK_EQ(chunk.get(-1, 0, 0), blocks::Air);
    CHECK(!chunk.set(0, kChunkSizeY, 0, blocks::Stone));
    CHECK(!chunk.set(0, 0, 0, blocks::Count));

    vox::World world;
    world.insert(std::make_shared<Chunk>(std::move(chunk)));
    CHECK_EQ(world.chunk_count(), 1u);
    CHECK(world.get(ChunkCoord{0, 0}) != nullptr);
    CHECK(world.get(ChunkCoord{1, 0}) == nullptr);
}

void test_generator() {
    using namespace vox;

    WorldSpec spec = WorldSpec::make_default(424242);
    WorldGenerator generator(spec);
    CHECK(generator.spec_hash() != 0);

    const ChunkCoord coord{0, 0};
    Chunk a = generator.generate(coord);
    Chunk b = generator.generate(coord);
    CHECK(a.validate());
    CHECK_EQ(a.run_count(), b.run_count());
    CHECK_EQ(a.revision(), 0u);
    CHECK(a.memory_bytes() < kChunkVoxelCount);

    BinaryWriter wa;
    BinaryWriter wb;
    a.serialize(wa);
    b.serialize(wb);
    CHECK(wa.data() == wb.data());

    std::size_t water = 0;
    std::size_t stone = 0;
    for (int z = 0; z < kChunkSizeZ; z += 4) {
        for (int x = 0; x < kChunkSizeX; x += 4) {
            const int height = a.highest_solid(x, z);
            CHECK(height >= 0);
            CHECK(height < kChunkSizeY);
            water += a.column_block_count(x, z, blocks::Water);
            stone += a.column_block_count(x, z, blocks::Stone);
        }
    }
    CHECK(wa.size() > 0);
    CHECK(stone > 0 || water > 0);

    const float height = generator.height_at(123, -456);
    CHECK(height >= 2.0f && height <= 254.0f);
    const Biome biome = generator.biome_at(123, -456);
    CHECK(static_cast<int>(biome) >= 0);
    CHECK(static_cast<int>(biome) <= static_cast<int>(Biome::Snow));
}

void test_protocol() {
    using namespace vox;
    using namespace vox::net;

    {
        Hello hello{"player", 0xdeadbeef};
        const auto packet = encode_hello(hello);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        CHECK(view->type == PacketType::Hello);
        Hello decoded;
        CHECK(decode_hello(view->payload, decoded));
        CHECK_EQ(decoded.player_name, hello.player_name);
        CHECK_EQ(decoded.spec_hash, hello.spec_hash);
    }

    {
        ChunkData data;
        data.x = -3;
        data.z = 7;
        data.revision = 42;
        data.uncompressed_size = 100;
        data.compressed_size = 4;
        data.crc32 = 0x12345678;
        data.compressed = {1, 2, 3, 4};
        const auto packet = encode_chunk_data(data);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        ChunkData decoded;
        CHECK(decode_chunk_data(view->payload, decoded));
        CHECK_EQ(decoded.x, data.x);
        CHECK_EQ(decoded.z, data.z);
        CHECK_EQ(decoded.revision, data.revision);
        CHECK(decoded.compressed == data.compressed);
    }

    {
        Snapshot snapshot;
        snapshot.tick = 77;
        snapshot.players.push_back(PlayerState{1, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6});
        snapshot.players.push_back(PlayerState{2, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, 7});
        const auto packet = encode_snapshot(snapshot);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        Snapshot decoded;
        CHECK(decode_snapshot(view->payload, decoded));
        CHECK_EQ(decoded.tick, 77u);
        CHECK_EQ(decoded.players.size(), 2u);
        CHECK_EQ(decoded.players[1].player_id, 2u);
    }

    {
        const std::vector<std::uint8_t> garbage{1, 2, 3};
        CHECK(!parse_packet(garbage).has_value());
    }
    {
        auto packet = encode_ping(Ping{123});
        packet[0] = 0;
        CHECK(!parse_packet(packet).has_value());
    }
    {
        auto packet = encode_block_edit(BlockEdit{1, 2, 3, blocks::Stone, 4});
        packet.resize(packet.size() - 2);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        BlockEdit decoded;
        CHECK(!decode_block_edit(view->payload, decoded));
    }
}

void test_integration() {
    using namespace vox;
    using namespace vox::net;

    ServerConfig server_config;
    server_config.port = 0;
    server_config.seed = 9001;
    server_config.max_players = 4;
    server_config.persist = false;
    server_config.load_world = false;
    server_config.max_chunk_requests_per_tick = 16;

    GameServer server(server_config);
    CHECK(server.start());
    if (!server.running()) {
        return;
    }

    GameClient client;
    ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = server.port();
    client_config.player_name = "test-client";
    CHECK(client.connect(client_config));

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!client.welcome() && std::chrono::steady_clock::now() < deadline) {
        server.tick(0.02f);
        client.poll(1);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(client.welcome().has_value());
    CHECK_EQ(client.welcome()->spec_hash, server.spec_hash());

    if (client.welcome()) {
        const auto spawn_chunk = chunk_coord_at(
            static_cast<std::int64_t>(client.welcome()->spawn_x),
            static_cast<std::int64_t>(client.welcome()->spawn_z));
        CHECK(client.request_chunk(spawn_chunk));

        const auto chunk_deadline = std::chrono::steady_clock::now() + 5s;
        while (client.stats().chunks_received == 0 &&
               std::chrono::steady_clock::now() < chunk_deadline) {
            server.tick(0.02f);
            client.poll(1);
            std::this_thread::sleep_for(1ms);
        }
        CHECK(client.stats().chunks_received > 0);
        CHECK(client.world().get(spawn_chunk) != nullptr);

        const auto server_chunk = server.world().get(spawn_chunk);
        CHECK(server_chunk != nullptr);
        if (server_chunk) {
            const int lx = local_x(static_cast<std::int64_t>(client.welcome()->spawn_x));
            const int lz = local_z(static_cast<std::int64_t>(client.welcome()->spawn_z));
            const int y = server_chunk->highest_solid(lx, lz);
            CHECK(y > 1);
            if (y > 1) {
                const auto world_x = spawn_chunk.x * kChunkSizeX + lx;
                const auto world_z = spawn_chunk.z * kChunkSizeZ + lz;
                CHECK(client.send_block_edit(world_x, static_cast<std::uint16_t>(y),
                                             world_z, blocks::Air));
                const auto edit_deadline = std::chrono::steady_clock::now() + 5s;
                while (server.world().get_block(world_x, y, world_z) != blocks::Air &&
                       std::chrono::steady_clock::now() < edit_deadline) {
                    server.tick(0.02f);
                    client.poll(1);
                    std::this_thread::sleep_for(1ms);
                }
                CHECK_EQ(server.world().get_block(world_x, y, world_z), blocks::Air);
                CHECK(server.stats().edits_applied > 0);

                const auto update_deadline = std::chrono::steady_clock::now() + 2s;
                while (client.stats().block_updates_received == 0 &&
                       std::chrono::steady_clock::now() < update_deadline) {
                    server.tick(0.02f);
                    client.poll(1);
                    std::this_thread::sleep_for(1ms);
                }
                CHECK_EQ(client.world().get_block(world_x, y, world_z), blocks::Air);
            }
        }
    }

    client.disconnect();
    server.stop();
}

void test_job_system() {
    vox::JobSystem jobs(4);
    std::vector<int> values(1000, 0);
    jobs.parallel_for(values.size(), 32, [&](std::size_t i) {
        values[i] = static_cast<int>(i * 2);
    });
    for (std::size_t i = 0; i < values.size(); ++i) {
        CHECK_EQ(values[i], static_cast<int>(i * 2));
        if (g_failures > 20) {
            return;
        }
    }
}

} // namespace

int main() {
    test_binary();
    test_zstd();
    test_chunk_rle();
    test_generator();
    test_protocol();
    test_integration();
    test_job_system();

    if (g_failures == 0) {
        std::printf("All %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "%d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
