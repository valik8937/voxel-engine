#include "core/binary.h"
#include "core/hash.h"
#include "core/job_system.h"
#include "core/zstd.h"
#include "game/collision_streamer.h"
#include "game/player_input.h"
#include "net/client.h"
#include "net/protocol.h"
#include "net/server.h"
#include "physics/physics_world.h"
#include "voxel/block.h"
#include "voxel/chunk.h"
#include "voxel/raycast.h"
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

void test_math_transforms() {
    using namespace vox;

    const Vec3 translated = transform_point(Mat4::translation({1.0f, 2.0f, 3.0f}),
                                            {0.0f, 0.0f, 0.0f});
    CHECK(std::abs(translated.x - 1.0f) < 1.0e-5f);
    CHECK(std::abs(translated.y - 2.0f) < 1.0e-5f);
    CHECK(std::abs(translated.z - 3.0f) < 1.0e-5f);

    const Quat quarter_turn = Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, kPi * 0.5f);
    const Vec3 rotated = transform_point(quarter_turn.to_mat4(), {1.0f, 0.0f, 0.0f});
    CHECK(std::abs(rotated.x) < 1.0e-5f);
    CHECK(std::abs(rotated.z + 1.0f) < 1.0e-5f);

    const Mat4 transform = Mat4::trs({1.0f, 0.0f, 0.0f}, quarter_turn,
                                     {2.0f, 2.0f, 2.0f});
    const Vec3 point = transform_point(transform, {1.0f, 0.0f, 0.0f});
    CHECK(std::abs(point.x - 1.0f) < 1.0e-5f);
    CHECK(std::abs(point.z + 2.0f) < 1.0e-5f);
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

    Chunk chunk = Chunk::make_flat(ChunkCoord{0, 0, 0}, blocks::Air);
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
    CHECK(writer.size() < kChunkVoxelCount * 4);

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
    CHECK(world.get(ChunkCoord{0, 0, 0}) != nullptr);
    CHECK(world.get(ChunkCoord{1, 0, 0}) == nullptr);
}

void test_generator() {
    using namespace vox;

    WorldSpec spec = WorldSpec::make_default(424242);
    WorldGenerator generator(spec);
    CHECK(generator.spec_hash() != 0);

    const int surface_y = static_cast<int>(generator.height_at(0, 0));
    const ChunkCoord coord{0, surface_y / kChunkSizeY, 0};
    Chunk a = generator.generate(coord);
    Chunk b = generator.generate(coord);
    CHECK(a.validate());
    CHECK_EQ(a.run_count(), b.run_count());
    CHECK_EQ(a.revision(), 0u);
    CHECK(a.memory_bytes() < kChunkVoxelCount * 2);

    BinaryWriter wa;
    BinaryWriter wb;
    a.serialize(wa);
    b.serialize(wb);
    CHECK(wa.data() == wb.data());

    std::size_t water = 0;
    std::size_t stone = 0;
    std::size_t solid_columns = 0;
    for (int z = 0; z < kChunkSizeZ; z += 4) {
        for (int x = 0; x < kChunkSizeX; x += 4) {
            const int height = a.highest_solid(x, z);
            CHECK(height >= -1);
            CHECK(height < kChunkSizeY);
            if (height >= 0) {
                ++solid_columns;
            }
            water += a.column_block_count(x, z, blocks::Water);
            stone += a.column_block_count(x, z, blocks::Stone);
        }
    }
    CHECK(wa.size() > 0);
    CHECK(solid_columns > 0);
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
        data.y = 5;
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
        CHECK_EQ(decoded.y, data.y);
        CHECK_EQ(decoded.z, data.z);
        CHECK_EQ(decoded.revision, data.revision);
        CHECK(decoded.compressed == data.compressed);
    }

    {
        Snapshot snapshot;
        snapshot.tick = 77;
        snapshot.players.push_back(vox::net::PlayerState{1, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6});
        snapshot.players.push_back(vox::net::PlayerState{2, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, 7});
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
        vox::net::PlayerMove move;
        move.sequence = 42;
        move.x = 1.5f;
        move.y = 71.0f;
        move.z = -3.25f;
        move.velocity_x = 2.0f;
        move.velocity_y = -1.0f;
        move.velocity_z = -4.0f;
        move.yaw = 0.5f;
        move.pitch = -0.25f;
        move.on_ground = true;
        move.teleport = false;
        const auto packet = encode_player_move(move);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        CHECK(view->type == PacketType::PlayerMove);
        vox::net::PlayerMove decoded;
        CHECK(decode_player_move(view->payload, decoded));
        CHECK_EQ(decoded.sequence, move.sequence);
        CHECK_EQ(decoded.x, move.x);
        CHECK_EQ(decoded.y, move.y);
        CHECK_EQ(decoded.z, move.z);
        CHECK_EQ(decoded.on_ground, move.on_ground);
    }

    {
        vox::net::MoveReject reject;
        reject.sequence = 7;
        reject.x = 10.0f;
        reject.y = 20.0f;
        reject.z = 30.0f;
        reject.reason = 1;
        const auto packet = encode_move_reject(reject);
        const auto view = parse_packet(packet);
        CHECK(view.has_value());
        CHECK(view->type == PacketType::MoveReject);
        vox::net::MoveReject decoded;
        CHECK(decode_move_reject(view->payload, decoded));
        CHECK_EQ(decoded.sequence, reject.sequence);
        CHECK_EQ(decoded.reason, reject.reason);
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
            static_cast<std::int64_t>(client.welcome()->spawn_y),
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
            const int local_surface = server_chunk->highest_solid(lx, lz);
            const int y = spawn_chunk.y * kChunkSizeY + local_surface;
            CHECK(local_surface > 0);
            if (local_surface > 0) {
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

        const float start_z = client.welcome()->spawn_z;
        std::uint32_t move_sequence = 1;
        for (int tick = 0; tick < 30; ++tick) {
            vox::net::PlayerMove move;
            move.sequence = move_sequence++;
            move.x = client.welcome()->spawn_x;
            move.y = client.welcome()->spawn_y;
            move.z = start_z - 0.1f * static_cast<float>(tick + 1);
            move.yaw = 0.0f;
            move.on_ground = true;
            CHECK(client.send_player_move(move));
            client.poll(0);
            server.tick(1.0f / 30.0f);
            client.poll(1);
            std::this_thread::sleep_for(1ms);
        }
        const auto* local = client.local_player();
        CHECK(local != nullptr);
        if (local != nullptr) {
            CHECK(local->last_move_sequence > 0);
            CHECK(local->z < start_z - 2.0f);
        }
    }

    client.disconnect();
    server.stop();
}

void test_raycast() {
    using namespace vox;

    std::vector<Run> runs;
    for (int i = 0; i < kChunkColumns; ++i) {
        runs.push_back(Run{blocks::Stone, kChunkSizeY});
    }
    World world;
    world.insert(std::make_shared<Chunk>(
        Chunk::from_runs(ChunkCoord{0, 0, 0}, 0, std::move(runs))));

    const RaycastHit hit = raycast_voxels(world, {16.5f, 40.0f, 16.5f},
                                          {0.0f, -1.0f, 0.0f}, 10.0f);
    CHECK(hit.hit);
    CHECK_EQ(hit.y, 31);
    CHECK_EQ(hit.normal_y, 1);

    const RaycastHit miss = raycast_voxels(world, {16.5f, 40.0f, 16.5f},
                                           {0.0f, 1.0f, 0.0f}, 10.0f);
    CHECK(!miss.hit);
}

void test_networked_character_jump() {
    using namespace vox;
    using namespace vox::net;

    ServerConfig server_config;
    server_config.port = 0;
    server_config.seed = 1337;
    server_config.max_players = 2;
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
    CHECK(client.connect(client_config));

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!client.welcome() && std::chrono::steady_clock::now() < deadline) {
        server.tick(1.0f / 30.0f);
        client.poll(1);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(client.welcome().has_value());
    if (!client.welcome()) {
        client.disconnect();
        server.stop();
        return;
    }

    const auto welcome = *client.welcome();
    const ChunkCoord spawn_chunk = chunk_coord_at(
        static_cast<std::int64_t>(welcome.spawn_x),
        static_cast<std::int64_t>(welcome.spawn_y),
        static_cast<std::int64_t>(welcome.spawn_z));
    CHECK(client.request_chunk(spawn_chunk));

    PhysicsWorld physics;
    CHECK(physics.initialize());
    CollisionStreamer collision(client.world(), physics);
    const PhysicsCharacter character =
        physics.create_character({welcome.spawn_x, welcome.spawn_y, welcome.spawn_z});
    CHECK(character != kInvalidPhysicsCharacter);

    bool collision_ready = false;
    const auto collision_deadline = std::chrono::steady_clock::now() + 5s;
    while (!collision_ready && std::chrono::steady_clock::now() < collision_deadline) {
        server.tick(1.0f / 30.0f);
        client.poll(1);
        (void)collision.update(spawn_chunk, 1, 1, 1000);
        collision_ready = collision.has_chunk(spawn_chunk);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(collision_ready);

    constexpr Vec3 gravity{0.0f, -24.0f, 0.0f};
    std::uint32_t sequence = 1;
    for (int i = 0; i < 150; ++i) {
        PlayerMoveInput local_input;
        local_input.jump = (i == 0);
        const Vec3 wish = player_wish_velocity(local_input);

        physics.step(1.0f / 30.0f);
        physics.step_character(character, wish, local_input.jump, gravity,
                               1.0f / 30.0f);

        const Vec3 position = physics.character_position(character);
        const Vec3 velocity = physics.character_velocity(character);
        net::PlayerMove move;
        move.sequence = sequence++;
        move.x = position.x;
        move.y = position.y;
        move.z = position.z;
        move.velocity_x = velocity.x;
        move.velocity_y = velocity.y;
        move.velocity_z = velocity.z;
        move.yaw = 0.0f;
        move.on_ground = physics.character_on_ground(character);
        CHECK(client.send_player_move(move));

        client.poll(0);
        server.tick(1.0f / 30.0f);
        client.poll(1);
        std::this_thread::sleep_for(1ms);
    }

    const Vec3 local_position = physics.character_position(character);
    CHECK(local_position.y > welcome.spawn_y - 2.0f);
    if (const net::PlayerState* self = client.local_player()) {
        CHECK(self->y > welcome.spawn_y - 2.0f);
    }

    physics.remove_character(character);
    collision.clear();
    physics.shutdown();
    client.disconnect();
    server.stop();
}

void test_jolt_character_on_generated_chunks() {
    using namespace vox;

    WorldSpec spec = WorldSpec::make_default(1337);
    WorldGenerator generator(spec);
    World world;

    const float surface = generator.height_at(16, 16);
    const int surface_block = static_cast<int>(std::floor(surface));
    const ChunkCoord center = chunk_coord_at(16, surface_block + 1, 16);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const ChunkCoord coord{center.x + dx, center.y + dy, center.z + dz};
                world.insert(std::make_shared<Chunk>(generator.generate(coord)));
            }
        }
    }

    PhysicsWorld physics;
    CHECK(physics.initialize());
    CollisionStreamer collision(world, physics);
    CHECK(collision.update(center, 1, 1, 1000));
    CHECK(collision.has_chunk(center));

    const float start_y = static_cast<float>(surface_block + 3);
    const PhysicsCharacter character = physics.create_character({16.5f, start_y, 16.5f});
    CHECK(character != kInvalidPhysicsCharacter);

    for (int i = 0; i < 120; ++i) {
        physics.step(1.0f / 60.0f);
        physics.step_character(character, {}, false, {0.0f, -24.0f, 0.0f},
                               1.0f / 60.0f);
    }
    const float settled_y = physics.character_position(character).y;
    CHECK(settled_y > static_cast<float>(surface_block) - 0.5f);
    CHECK(settled_y < start_y);

    // Jump and verify the character does not tunnel through the generated chunk.
    physics.step_character(character, {}, true, {0.0f, -24.0f, 0.0f}, 1.0f / 60.0f);
    float peak_y = settled_y;
    for (int i = 0; i < 120; ++i) {
        physics.step(1.0f / 60.0f);
        physics.step_character(character, {}, false, {0.0f, -24.0f, 0.0f},
                               1.0f / 60.0f);
        peak_y = std::max(peak_y, physics.character_position(character).y);
    }
    CHECK(peak_y > settled_y + 0.2f);
    CHECK(physics.character_position(character).y >
          static_cast<float>(surface_block) - 0.5f);

    physics.remove_character(character);
    collision.clear();
    physics.shutdown();
}

void test_jolt_chunk_boundaries_and_jumping() {
    using namespace vox;

    World world;
    for (int z = -2; z <= 2; ++z) {
        for (int x = -2; x <= 2; ++x) {
            world.insert(std::make_shared<Chunk>(Chunk::make_flat(
                ChunkCoord{x, 0, z}, blocks::Stone)));
        }
    }

    PhysicsWorld physics;
    CHECK(physics.initialize());
    CollisionStreamer collision(world, physics);
    (void)collision.update(ChunkCoord{0, 0, 0}, 2, 1, 1000);
    CHECK(collision.has_chunk(ChunkCoord{0, 0, 0}));

    const PhysicsCharacter character =
        physics.create_character({16.5f, 34.0f, 16.5f});
    CHECK(character != kInvalidPhysicsCharacter);

    for (int i = 0; i < 300; ++i) {
        physics.step(1.0f / 60.0f);
        physics.step_character(character, {0.0f, 0.0f, -4.8f}, (i % 60) == 0,
                               {0.0f, -24.0f, 0.0f}, 1.0f / 60.0f);
    }

    const Vec3 position = physics.character_position(character);
    CHECK(position.y > 31.5f);
    CHECK(position.y < 34.5f);
    CHECK(position.z < 10.0f);

    physics.remove_character(character);
    collision.clear();
    physics.shutdown();
}

void test_physics() {
    vox::PhysicsWorld physics;
    CHECK(physics.initialize());
    CHECK(physics.running());

    const float ground[] = {
        -10.0f, 0.0f, -10.0f,
        -10.0f, 0.0f, 10.0f,
        10.0f, 0.0f, 10.0f,
        10.0f, 0.0f, -10.0f,
    };
    const std::uint32_t ground_indices[] = {0, 1, 2, 0, 2, 3};
    physics.add_static_triangles(vox::ChunkCoord{0, 0, 0}, ground, ground_indices);

    const vox::PhysicsBody box = physics.add_box({0.0f, 8.0f, 0.0f},
                                                 {0.5f, 0.5f, 0.5f}, true);
    CHECK(box != vox::kInvalidPhysicsBody);
    for (int i = 0; i < 180; ++i) {
        physics.step(1.0f / 60.0f);
    }
    const vox::Vec3 position = physics.body_position(box);
    CHECK(position.y < 8.0f);
    CHECK(position.y > -0.1f);

    physics.remove_body(box);

    const vox::PhysicsCharacter character =
        physics.create_character({0.0f, 4.0f, 0.0f});
    CHECK(character != vox::kInvalidPhysicsCharacter);
    for (int i = 0; i < 180; ++i) {
        physics.step(1.0f / 60.0f);
        physics.step_character(character, {}, false, {0.0f, -24.0f, 0.0f},
                               1.0f / 60.0f);
    }
    const vox::Vec3 character_position = physics.character_position(character);
    CHECK(character_position.y < 4.0f);
    CHECK(character_position.y > -0.5f);
    CHECK(physics.character_on_ground(character));

    for (int i = 0; i < 60; ++i) {
        physics.step(1.0f / 60.0f);
        physics.step_character(character, {2.0f, 0.0f, 0.0f}, false,
                               {0.0f, -24.0f, 0.0f}, 1.0f / 60.0f);
    }
    CHECK(physics.character_position(character).x > 0.5f);

    physics.remove_character(character);
    physics.shutdown();
    CHECK(!physics.running());
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
    test_math_transforms();
    test_binary();
    test_zstd();
    test_chunk_rle();
    test_generator();
    test_protocol();
    test_integration();
    test_networked_character_jump();
    test_raycast();
    test_jolt_character_on_generated_chunks();
    test_jolt_chunk_boundaries_and_jumping();
    test_physics();
    test_job_system();

    if (g_failures == 0) {
        std::printf("All %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "%d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
