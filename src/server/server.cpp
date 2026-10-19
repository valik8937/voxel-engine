#include "net/server.h"

#include "core/hash.h"
#include "core/log.h"
#include "net/enet_runtime.h"
#include "voxel/block.h"

#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace vox::net {
namespace {

constexpr std::uint8_t kChannelControl = 0;
constexpr std::uint8_t kChannelChunks = 1;
constexpr std::uint8_t kChannelEdits = 2;
constexpr std::uint8_t kChannelSnapshots = 3;

std::string safe_name(std::string name) {
    if (name.empty()) {
        return "player";
    }
    if (name.size() > 32) {
        name.resize(32);
    }
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32) {
            c = '?';
        }
    }
    return name;
}

} // namespace

GameServer::GameServer(ServerConfig config)
    : config_(std::move(config)),
      generator_(WorldSpec::make_default(config_.seed)),
      store_(config_.world_directory) {}

GameServer::~GameServer() {
    stop();
}

bool GameServer::start() {
    if (running_) {
        return true;
    }
    if (!enet_runtime_initialize()) {
        VOX_LOG_ERROR("ENet initialization failed");
        return false;
    }

    ENetAddress address{};
    address.port = config_.port;
    if (config_.bind_address.empty() || config_.bind_address == "0.0.0.0" ||
        config_.bind_address == "*") {
        address.host = ENET_HOST_ANY;
    } else if (enet_address_set_host(&address, config_.bind_address.c_str()) != 0) {
        VOX_LOG_ERROR("Invalid bind address '{}'", config_.bind_address);
        return false;
    }

    host_ = enet_host_create(&address, config_.max_players, 4, 0, 0);
    if (host_ == nullptr) {
        VOX_LOG_ERROR("Cannot create server host on port {}", config_.port);
        return false;
    }
    port_ = host_->address.port;

    if (config_.load_world) {
        (void)store_.load(world_);
    }
    find_spawn();

    // Keep a small collision-ready area available immediately. The client will
    // request the rest as its render distance grows.
    const ChunkCoord spawn_chunk = chunk_coord_at(
        static_cast<std::int64_t>(spawn_x_), static_cast<std::int64_t>(spawn_y_),
        static_cast<std::int64_t>(spawn_z_));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                (void)get_or_generate(
                    ChunkCoord{spawn_chunk.x + dx, spawn_chunk.y + dy,
                               spawn_chunk.z + dz});
            }
        }
    }
    refine_spawn();

    running_ = true;
    VOX_LOG_INFO("Server listening on port {} (seed {}, spec {:08x}, spawn {:.1f},{:.1f},{:.1f})",
                 port_, config_.seed, generator_.spec_hash(), spawn_x_, spawn_y_, spawn_z_);
    return true;
}

void GameServer::stop() {
    if (!running_ && host_ == nullptr) {
        return;
    }
    if (config_.persist) {
        save_dirty();
    }
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    players_.clear();
    pending_chunks_.clear();
    running_ = false;
    VOX_LOG_INFO("Server stopped");
}

void GameServer::tick(float dt) {
    if (!running_ || host_ == nullptr) {
        return;
    }

    dt = std::clamp(dt, 0.0f, 0.25f);

    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                handle_event_connect(event.peer);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                handle_event_disconnect(event.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                handle_packet(event.peer, event.packet);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    process_pending_chunks();
    step_players(dt);

    snapshot_accumulator_ += dt;
    if (snapshot_accumulator_ >= config_.snapshot_interval) {
        snapshot_accumulator_ = 0.0f;
        send_snapshot();
    }

    save_accumulator_ += dt;
    if (config_.persist && save_accumulator_ >= config_.save_interval) {
        save_accumulator_ = 0.0f;
        save_dirty();
    }

    enet_host_flush(host_);
    ++server_tick_;
}

void GameServer::handle_event_connect(_ENetPeer* peer) {
    Player player;
    player.peer = peer;
    player.id = next_player_id_++;
    player.x = spawn_x_;
    player.y = spawn_y_;
    player.z = spawn_z_;
    players_[peer] = player;
    VOX_LOG_INFO("Peer connected: {}", static_cast<unsigned>(player.id));
}

void GameServer::handle_event_disconnect(_ENetPeer* peer) {
    const auto it = players_.find(peer);
    if (it != players_.end()) {
        VOX_LOG_INFO("Player {} disconnected", it->second.name.empty() ? "unknown" : it->second.name);
        players_.erase(it);
    }
}

void GameServer::handle_packet(_ENetPeer* peer, _ENetPacket* packet) {
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return;
    }
    Player& player = it->second;

    const std::span<const std::uint8_t> data(packet->data, packet->dataLength);
    const auto view = parse_packet(data);
    if (!view) {
        VOX_LOG_WARN("Dropped malformed packet ({} bytes)", packet->dataLength);
        return;
    }

    if (!player.authenticated) {
        if (view->type == PacketType::Hello) {
            handle_hello(player, view->payload);
        }
        return;
    }

    switch (view->type) {
        case PacketType::ChunkRequest:
            handle_chunk_request(player, view->payload);
            break;
        case PacketType::BlockEdit:
            handle_block_edit(player, view->payload);
            break;
        case PacketType::PlayerInput:
            handle_player_input(player, view->payload);
            break;
        case PacketType::Ping: {
            Ping ping;
            if (decode_ping(view->payload, ping)) {
                const auto pong = encode_pong(Pong{ping.nonce});
                send_packet(peer, pong, false, kChannelControl);
            }
            break;
        }
        case PacketType::Disconnect:
            enet_peer_disconnect_later(peer, 0);
            break;
        default:
            break;
    }
}

void GameServer::handle_hello(Player& player, std::span<const std::uint8_t> payload) {
    Hello hello;
    if (!decode_hello(payload, hello)) {
        const auto response = encode_disconnect(Disconnect{"invalid hello"});
        send_packet(player.peer, response, true, kChannelControl);
        enet_peer_disconnect_later(player.peer, 0);
        return;
    }
    if (hello.spec_hash != 0 && hello.spec_hash != generator_.spec_hash()) {
        const auto response = encode_disconnect(Disconnect{"world spec mismatch"});
        send_packet(player.peer, response, true, kChannelControl);
        enet_peer_disconnect_later(player.peer, 0);
        return;
    }

    player.name = safe_name(std::move(hello.player_name));
    player.authenticated = true;
    player.x = spawn_x_;
    player.y = spawn_y_;
    player.z = spawn_z_;
    VOX_LOG_INFO("Player {} authenticated as '{}'", player.id, player.name);
    send_welcome(player);
}

void GameServer::handle_chunk_request(Player& player, std::span<const std::uint8_t> payload) {
    ChunkRequest request;
    if (!decode_chunk_request(payload, request)) {
        return;
    }
    if (pending_chunks_.size() >= 4096) {
        return;
    }
    pending_chunks_.push_back(PendingChunk{
        player.peer, ChunkCoord{request.x, request.y, request.z}, request.request_id});
}

void GameServer::handle_block_edit(Player& player, std::span<const std::uint8_t> payload) {
    BlockEdit edit;
    if (!decode_block_edit(payload, edit) || edit.block >= blocks::Count) {
        return;
    }

    const float dx = static_cast<float>(edit.x) + 0.5f - player.x;
    const float dy = static_cast<float>(edit.y) + 0.5f - player.y;
    const float dz = static_cast<float>(edit.z) + 0.5f - player.z;
    if (dx * dx + dy * dy + dz * dz > 8.0f * 8.0f) {
        return;
    }

    const auto chunk = get_or_generate(chunk_coord_at(edit.x, edit.y, edit.z));
    if (!chunk) {
        return;
    }
    chunk->set_world(edit.x, edit.y, edit.z, edit.block);
    ++stats_.edits_applied;

    BlockUpdate update;
    update.x = edit.x;
    update.y = edit.y;
    update.z = edit.z;
    update.block = edit.block;
    update.revision = chunk->revision();
    send_block_update(update);
}

void GameServer::handle_player_input(Player& player, std::span<const std::uint8_t> payload) {
    PlayerInput input;
    if (!decode_player_input(payload, input)) {
        return;
    }
    if (input.sequence <= player.last_input_sequence) {
        return;
    }
    player.last_input_sequence = input.sequence;
    player.yaw = input.yaw;
    player.pitch = std::clamp(input.pitch, -1.55f, 1.55f);
    player.last_input.move_forward = input.move_forward;
    player.last_input.move_right = input.move_right;
    player.last_input.jump = input.jump;
    player.last_input.sprint = input.sprint;
    player.last_input.yaw = input.yaw;
    player.last_input.pitch = input.pitch;
    player.has_input = true;
}

void GameServer::step_players(float dt) {
    for (auto& [peer, player] : players_) {
        (void)peer;
        if (!player.authenticated || !player.has_input) {
            continue;
        }

        PlayerMoveState state;
        state.position = Vec3{player.x, player.y, player.z};
        state.velocity = player.velocity;
        state.on_ground = player.on_ground;

        step_player(world_, state, player.last_input, dt);

        player.x = state.position.x;
        player.y = state.position.y;
        player.z = state.position.z;
        player.velocity = state.velocity;
        player.on_ground = state.on_ground;

        // Movement axes are one-shot; gravity still applies next tick.
        player.last_input.move_forward = 0.0f;
        player.last_input.move_right = 0.0f;
        player.last_input.jump = false;
    }
}

void GameServer::send_welcome(Player& player) {
    Welcome welcome;
    welcome.player_id = player.id;
    welcome.server_tick = server_tick_;
    welcome.spec_hash = generator_.spec_hash();
    welcome.spawn_x = spawn_x_;
    welcome.spawn_y = spawn_y_;
    welcome.spawn_z = spawn_z_;
    welcome.chunk_size_x = kChunkSizeX;
    welcome.chunk_size_y = kChunkSizeY;
    welcome.chunk_size_z = kChunkSizeZ;
    const auto packet = encode_welcome(welcome);
    send_packet(player.peer, packet, true, kChannelControl);
}

void GameServer::send_chunk(Player& player, ChunkCoord coord, std::uint32_t request_id) {
    (void)request_id;
    const auto chunk = get_or_generate(coord);
    if (!chunk) {
        return;
    }

    BinaryWriter writer;
    chunk->serialize(writer);
    auto compressed = compressor_.compress(writer.data());
    if (!compressed) {
        VOX_LOG_ERROR("Cannot compress chunk {},{}", coord.x, coord.z);
        return;
    }
    if (compressed->size() > kMaxChunkCompressedSize ||
        writer.size() > kMaxChunkUncompressedSize) {
        VOX_LOG_ERROR("Chunk {},{} exceeds transfer limit", coord.x, coord.z);
        return;
    }

    ChunkData data;
    data.x = coord.x;
    data.y = coord.y;
    data.z = coord.z;
    data.revision = chunk->revision();
    data.uncompressed_size = static_cast<std::uint32_t>(writer.size());
    data.compressed_size = static_cast<std::uint32_t>(compressed->size());
    data.crc32 = crc32(writer.data());
    data.compressed = std::move(*compressed);

    const auto packet = encode_chunk_data(data);
    send_packet(player.peer, packet, true, kChannelChunks);
    ++stats_.chunks_sent;
    stats_.compressed_bytes += data.compressed_size;
}

void GameServer::send_block_update(const BlockUpdate& update) {
    const auto packet = encode_block_update(update);
    for (auto& [peer, player] : players_) {
        if (player.authenticated) {
            send_packet(peer, packet, true, kChannelEdits);
        }
    }
}

void GameServer::send_snapshot() {
    Snapshot snapshot;
    snapshot.tick = server_tick_;
    snapshot.players.reserve(players_.size());
    for (const auto& [peer, player] : players_) {
        (void)peer;
        if (!player.authenticated) {
            continue;
        }
        PlayerState state;
        state.player_id = player.id;
        state.x = player.x;
        state.y = player.y;
        state.z = player.z;
        state.yaw = player.yaw;
        state.pitch = player.pitch;
        state.last_input = player.last_input_sequence;
        snapshot.players.push_back(state);
    }
    const auto packet = encode_snapshot(snapshot);
    for (auto& [peer, player] : players_) {
        if (player.authenticated) {
            send_packet(peer, packet, false, kChannelSnapshots);
        }
    }
    ++stats_.snapshots_sent;
}

void GameServer::send_packet(_ENetPeer* peer, std::span<const std::uint8_t> data,
                             bool reliable, std::uint8_t channel) {
    if (host_ == nullptr || peer == nullptr || data.empty()) {
        return;
    }
    ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet == nullptr) {
        return;
    }
    if (enet_peer_send(peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
    }
}

std::shared_ptr<Chunk> GameServer::get_or_generate(ChunkCoord coord) {
    if (auto chunk = world_.get(coord)) {
        return chunk;
    }
    auto chunk = std::make_shared<Chunk>(generator_.generate(coord));
    world_.insert(chunk);
    ++stats_.chunks_generated;
    return chunk;
}

void GameServer::process_pending_chunks() {
    std::size_t processed = 0;
    while (!pending_chunks_.empty() && processed < config_.max_chunk_requests_per_tick) {
        const PendingChunk request = pending_chunks_.front();
        pending_chunks_.pop_front();
        ++processed;

        const auto it = players_.find(request.peer);
        if (it == players_.end() || !it->second.authenticated) {
            continue;
        }
        send_chunk(it->second, request.coord, request.request_id);
    }
}

void GameServer::find_spawn() {
    float best_x = 0.5f;
    float best_z = 0.5f;
    float best_height = generator_.height_at(0, 0);
    for (int radius = 0; radius <= 128; radius += 8) {
        for (int z = -radius; z <= radius; z += 8) {
            for (int x = -radius; x <= radius; x += 8) {
                const float height = generator_.height_at(x, z);
                if (height > spec().sea_level + 2.0f) {
                    best_x = static_cast<float>(x) + 0.5f;
                    best_z = static_cast<float>(z) + 0.5f;
                    best_height = height;
                    radius = 1000;
                    z = 1000;
                    break;
                }
            }
        }
    }
    spawn_x_ = best_x;
    spawn_y_ = best_height + 2.0f;
    spawn_z_ = best_z;
}

void GameServer::refine_spawn() {
    const ChunkCoord spawn_chunk = chunk_coord_at(
        static_cast<std::int64_t>(spawn_x_), static_cast<std::int64_t>(spawn_y_),
        static_cast<std::int64_t>(spawn_z_));
    const auto chunk = world_.get(spawn_chunk);
    if (!chunk) {
        return;
    }

    const int sea_level = static_cast<int>(std::lround(spec().sea_level));
    const int base_y = spawn_chunk.y * kChunkSizeY;
    const int center_x = local_x(static_cast<std::int64_t>(spawn_x_));
    const int center_z = local_z(static_cast<std::int64_t>(spawn_z_));

    int best_x = center_x;
    int best_z = center_z;
    int best_height = chunk->highest_solid(center_x, center_z);
    int best_distance = std::numeric_limits<int>::max();
    int best_slope = std::numeric_limits<int>::max();

    constexpr int kSpawnSlopeRadius = 8;
    for (int lz = kSpawnSlopeRadius; lz < kChunkSizeZ - kSpawnSlopeRadius; ++lz) {
        for (int lx = kSpawnSlopeRadius; lx < kChunkSizeX - kSpawnSlopeRadius; ++lx) {
            const int height = chunk->highest_solid(lx, lz);
            const int world_height = base_y + height;
            if (height < 0 || world_height < sea_level + 1 ||
                height + 4 >= kChunkSizeY) {
                continue;
            }

            bool clear = true;
            for (int dy = 1; dy <= 4; ++dy) {
                if (chunk->get(lx, height + dy, lz) != blocks::Air) {
                    clear = false;
                    break;
                }
            }
            if (!clear) {
                continue;
            }

            int slope = 0;
            for (int dz = -kSpawnSlopeRadius; dz <= kSpawnSlopeRadius; ++dz) {
                for (int dx = -kSpawnSlopeRadius; dx <= kSpawnSlopeRadius; ++dx) {
                    const int neighbour = chunk->highest_solid(lx + dx, lz + dz);
                    if (neighbour < 0) {
                        slope = 999;
                        break;
                    }
                    slope = std::max(slope, std::abs(neighbour - height));
                }
            }

            const int distance = (lx - center_x) * (lx - center_x) +
                                 (lz - center_z) * (lz - center_z);
            if (slope < best_slope || (slope == best_slope && distance < best_distance)) {
                best_slope = slope;
                best_distance = distance;
                best_x = lx;
                best_z = lz;
                best_height = height;
            }
        }
    }

    if (best_height < 0) {
        return;
    }
    spawn_x_ = static_cast<float>(spawn_chunk.x * kChunkSizeX + best_x) + 0.5f;
    spawn_z_ = static_cast<float>(spawn_chunk.z * kChunkSizeZ + best_z) + 0.5f;
    spawn_y_ = static_cast<float>(base_y + best_height + 1);

    VOX_LOG_INFO("Refined spawn to {:.1f},{:.1f},{:.1f} (surface y={})",
                 spawn_x_, spawn_y_, spawn_z_, base_y + best_height);
}

void GameServer::save_dirty() {
    if (config_.persist) {
        (void)store_.save_dirty(world_);
    }
}

} // namespace vox::net
