#include "net/client.h"

#include "core/hash.h"
#include "core/log.h"
#include "net/enet_runtime.h"

#include <enet/enet.h>

#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace vox::net {
namespace {

constexpr std::uint8_t kChannelControl = 0;
constexpr std::uint8_t kChannelEdits = 2;
constexpr std::uint8_t kChannelInput = 3;

} // namespace

GameClient::GameClient() = default;

GameClient::~GameClient() {
    disconnect();
}

bool GameClient::connect(const ClientConfig& config) {
    if (host_ != nullptr) {
        disconnect();
    }
    if (!enet_runtime_initialize()) {
        last_error_ = "ENet initialization failed";
        state_ = State::Failed;
        return false;
    }

    host_ = enet_host_create(nullptr, 1, 4, 0, 0);
    if (host_ == nullptr) {
        last_error_ = "Cannot create client host";
        state_ = State::Failed;
        return false;
    }

    ENetAddress address{};
    address.port = config.port;
    if (enet_address_set_host(&address, config.host.c_str()) != 0) {
        last_error_ = "Cannot resolve host '" + config.host + "'";
        enet_host_destroy(host_);
        host_ = nullptr;
        state_ = State::Failed;
        return false;
    }

    peer_ = enet_host_connect(host_, &address, 4, 0);
    if (peer_ == nullptr) {
        last_error_ = "Cannot start ENet connection";
        enet_host_destroy(host_);
        host_ = nullptr;
        state_ = State::Failed;
        return false;
    }

    player_name_ = config.player_name.empty() ? "player" : config.player_name;
    if (player_name_.size() > 32) {
        player_name_.resize(32);
    }
    expected_spec_hash_ = config.expected_spec_hash;
    state_ = State::Connecting;
    last_error_.clear();
    VOX_LOG_INFO("Connecting to {}:{}", config.host, config.port);
    return true;
}

void GameClient::disconnect() {
    if (peer_ != nullptr) {
        enet_peer_disconnect(peer_, 0);
        peer_ = nullptr;
    }
    if (host_ != nullptr) {
        ENetEvent event;
        while (enet_host_service(host_, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }
        enet_host_flush(host_);
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    state_ = State::Disconnected;
}

void GameClient::poll(std::uint32_t timeout_ms) {
    if (host_ == nullptr) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(host_, &event, timeout_ms) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                state_ = State::Connected;
                Hello hello;
                hello.player_name = player_name_;
                hello.spec_hash = expected_spec_hash_;
                const auto packet = encode_hello(hello);
                send_packet(packet, true, kChannelControl);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                state_ = State::Disconnected;
                peer_ = nullptr;
                if (last_error_.empty()) {
                    last_error_ = "Disconnected by server";
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                handle_packet(event.packet);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
        timeout_ms = 0;
    }
}

bool GameClient::request_chunk(ChunkCoord coord) {
    if (state_ != State::Connected || peer_ == nullptr) {
        return false;
    }
    ChunkRequest request;
    request.x = coord.x;
    request.y = coord.y;
    request.z = coord.z;
    request.request_id = next_request_id_++;
    const auto packet = encode_chunk_request(request);
    send_packet(packet, true, kChannelControl);
    ++stats_.chunks_requested;
    return true;
}

bool GameClient::send_block_edit(std::int32_t x, std::uint16_t y, std::int32_t z,
                                 BlockId block) {
    if (state_ != State::Connected || peer_ == nullptr) {
        return false;
    }
    BlockEdit edit;
    edit.x = x;
    edit.y = y;
    edit.z = z;
    edit.block = block;
    edit.sequence = next_edit_sequence_++;
    const auto packet = encode_block_edit(edit);
    send_packet(packet, true, kChannelEdits);
    return true;
}

bool GameClient::send_player_input(const PlayerInput& input) {
    if (state_ != State::Connected || peer_ == nullptr) {
        return false;
    }
    const auto packet = encode_player_input(input);
    send_packet(packet, false, kChannelInput);
    return true;
}

std::uint32_t GameClient::player_id() const noexcept {
    return welcome_ ? welcome_->player_id : 0;
}

std::uint32_t GameClient::server_tick() const noexcept {
    return last_snapshot_ ? last_snapshot_->tick : 0;
}

const PlayerState* GameClient::local_player() const noexcept {
    if (!welcome_ || !last_snapshot_) {
        return nullptr;
    }
    for (const PlayerState& player : last_snapshot_->players) {
        if (player.player_id == welcome_->player_id) {
            return &player;
        }
    }
    return nullptr;
}

void GameClient::handle_packet(_ENetPacket* packet) {
    const std::span<const std::uint8_t> data(packet->data, packet->dataLength);
    const auto view = parse_packet(data);
    if (!view) {
        VOX_LOG_WARN("Dropped malformed server packet ({} bytes)", packet->dataLength);
        return;
    }

    switch (view->type) {
        case PacketType::Welcome: {
            Welcome welcome;
            if (!decode_welcome(view->payload, welcome)) {
                last_error_ = "Malformed welcome packet";
                state_ = State::Failed;
                return;
            }
            if (expected_spec_hash_ != 0 && welcome.spec_hash != expected_spec_hash_) {
                last_error_ = "World spec mismatch";
                state_ = State::Failed;
                enet_peer_disconnect_later(peer_, 0);
                return;
            }
            welcome_ = welcome;
            VOX_LOG_INFO("Welcome: player id {}, spawn {:.1f},{:.1f},{:.1f}",
                         welcome.player_id, welcome.spawn_x, welcome.spawn_y, welcome.spawn_z);
            break;
        }
        case PacketType::ChunkData: {
            ChunkData chunk_data;
            if (!decode_chunk_data(view->payload, chunk_data)) {
                VOX_LOG_WARN("Malformed chunk packet");
                return;
            }
            auto raw = decompressor_.decompress(chunk_data.compressed,
                                                chunk_data.uncompressed_size);
            if (!raw || crc32(*raw) != chunk_data.crc32) {
                VOX_LOG_WARN("Chunk {},{} failed checksum/decompression",
                             chunk_data.x, chunk_data.z);
                return;
            }
            BinaryReader reader(*raw);
            Chunk chunk;
            if (!Chunk::deserialize(reader, chunk)) {
                VOX_LOG_WARN("Chunk {},{} failed to deserialize", chunk_data.x, chunk_data.z);
                return;
            }
            if (chunk.coord() != ChunkCoord{chunk_data.x, chunk_data.y, chunk_data.z}) {
                VOX_LOG_WARN("Chunk coordinate mismatch");
                return;
            }
            chunk.set_revision(chunk_data.revision);
            chunk.set_dirty(false);
            world_.insert(std::make_shared<Chunk>(std::move(chunk)));
            ++stats_.chunks_received;
            stats_.bytes_received += chunk_data.compressed_size;
            break;
        }
        case PacketType::BlockUpdate: {
            BlockUpdate update;
            if (!decode_block_update(view->payload, update)) {
                return;
            }
            world_.set_block(update.x, update.y, update.z, update.block);
            ++stats_.block_updates_received;
            break;
        }
        case PacketType::Snapshot: {
            Snapshot snapshot;
            if (!decode_snapshot(view->payload, snapshot)) {
                return;
            }
            last_snapshot_ = std::move(snapshot);
            ++stats_.snapshots_received;
            break;
        }
        case PacketType::Disconnect: {
            Disconnect disconnect;
            if (decode_disconnect(view->payload, disconnect)) {
                last_error_ = disconnect.reason;
            }
            state_ = State::Disconnected;
            break;
        }
        case PacketType::Ping: {
            Ping ping;
            if (decode_ping(view->payload, ping)) {
                const auto pong = encode_pong(Pong{ping.nonce});
                send_packet(pong, false, kChannelControl);
            }
            break;
        }
        case PacketType::Pong:
            break;
        default:
            break;
    }
}

void GameClient::send_packet(std::span<const std::uint8_t> data, bool reliable,
                             std::uint8_t channel) {
    if (host_ == nullptr || peer_ == nullptr || data.empty()) {
        return;
    }
    ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet == nullptr) {
        return;
    }
    if (enet_peer_send(peer_, channel, packet) < 0) {
        enet_packet_destroy(packet);
    }
}

} // namespace vox::net
