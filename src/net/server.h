#pragma once

#include "core/zstd.h"
#include "game/collision_streamer.h"
#include "game/player_input.h"
#include "net/protocol.h"
#include "physics/physics_world.h"
#include "server/world_store.h"
#include "voxel/world.h"
#include "worldgen/generator.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct _ENetHost;
struct _ENetPacket;
struct _ENetPeer;

namespace vox::net {

struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 7777;
    std::uint32_t seed = 1337;
    std::size_t max_players = 16;
    std::filesystem::path world_directory = "worlds/default";
    bool load_world = true;
    bool persist = true;
    std::size_t max_chunk_requests_per_tick = 4;
    float snapshot_interval = 0.05f;
    float save_interval = 10.0f;
};

class GameServer {
public:
    struct Stats {
        std::uint64_t chunks_sent = 0;
        std::uint64_t chunks_generated = 0;
        std::uint64_t edits_applied = 0;
        std::uint64_t snapshots_sent = 0;
        std::uint64_t compressed_bytes = 0;
    };

    explicit GameServer(ServerConfig config);
    ~GameServer();

    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    [[nodiscard]] bool start();
    void stop();
    void tick(float dt);

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint32_t current_tick() const noexcept { return server_tick_; }
    [[nodiscard]] std::size_t connected_players() const noexcept { return players_.size(); }
    [[nodiscard]] const World& world() const noexcept { return world_; }
    [[nodiscard]] World& world() noexcept { return world_; }
    [[nodiscard]] const WorldGenerator& generator() const noexcept { return generator_; }
    [[nodiscard]] const WorldSpec& spec() const noexcept { return generator_.spec(); }
    [[nodiscard]] std::uint32_t spec_hash() const noexcept { return generator_.spec_hash(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Player {
        _ENetPeer* peer = nullptr;
        std::uint32_t id = 0;
        std::string name;
        bool authenticated = false;
        float x = 0.0f;
        float y = 64.0f;
        float z = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        Vec3 velocity{};
        bool on_ground = false;
        bool has_input = false;
        PhysicsCharacter character = kInvalidPhysicsCharacter;
        PlayerMoveInput last_input{};
        std::uint32_t last_input_sequence = 0;
    };

    struct PendingChunk {
        _ENetPeer* peer = nullptr;
        ChunkCoord coord;
        std::uint32_t request_id = 0;
    };

    void handle_event_connect(_ENetPeer* peer);
    void handle_event_disconnect(_ENetPeer* peer);
    void handle_packet(_ENetPeer* peer, _ENetPacket* packet);

    void handle_hello(Player& player, std::span<const std::uint8_t> payload);
    void handle_chunk_request(Player& player, std::span<const std::uint8_t> payload);
    void handle_block_edit(Player& player, std::span<const std::uint8_t> payload);
    void handle_player_input(Player& player, std::span<const std::uint8_t> payload);
    void step_players(float dt);
    void update_collision_streaming();
    void refresh_collision(ChunkCoord coord);

    void send_welcome(Player& player);
    void send_chunk(Player& player, ChunkCoord coord, std::uint32_t request_id);
    void send_block_update(const BlockUpdate& update);
    void send_snapshot();
    void send_packet(_ENetPeer* peer, std::span<const std::uint8_t> data,
                     bool reliable, std::uint8_t channel);

    [[nodiscard]] std::shared_ptr<Chunk> get_or_generate(ChunkCoord coord);
    void process_pending_chunks();
    void find_spawn();
    void refine_spawn();
    void save_dirty();

    ServerConfig config_;
    WorldGenerator generator_;
    WorldStore store_;
    World world_;
    ZstdCompressor compressor_{3};
    PhysicsWorld physics_;
    std::unique_ptr<CollisionStreamer> collision_streamer_;
    float collision_accumulator_ = 0.0f;

    _ENetHost* host_ = nullptr;
    bool running_ = false;
    std::uint16_t port_ = 0;
    std::uint32_t server_tick_ = 0;
    std::uint32_t next_player_id_ = 1;
    float snapshot_accumulator_ = 0.0f;
    float save_accumulator_ = 0.0f;
    float spawn_x_ = 0.5f;
    float spawn_y_ = 66.0f;
    float spawn_z_ = 0.5f;

    std::unordered_map<_ENetPeer*, Player> players_;
    std::deque<PendingChunk> pending_chunks_;
    Stats stats_;
};

} // namespace vox::net
