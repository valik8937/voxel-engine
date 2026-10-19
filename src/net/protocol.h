#pragma once

#include "core/binary.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vox::net {

inline constexpr std::uint32_t kProtocolMagic = 0x31584f56u; // "VOX1"
inline constexpr std::uint16_t kProtocolVersion = 2;
inline constexpr std::size_t kHeaderSize = 7;
inline constexpr std::size_t kMaxPacketSize = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaxChunkCompressedSize = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxChunkUncompressedSize = 8u * 1024u * 1024u;

enum class PacketType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    ChunkRequest = 3,
    ChunkData = 4,
    BlockEdit = 5,
    BlockUpdate = 6,
    Snapshot = 7,
    Disconnect = 8,
    Ping = 9,
    Pong = 10,
    PlayerMove = 11,
    MoveReject = 12,
};

struct PacketView {
    PacketType type = PacketType::Hello;
    std::span<const std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> make_packet(PacketType type,
                                                    const BinaryWriter& payload);
[[nodiscard]] std::optional<PacketView> parse_packet(
    std::span<const std::uint8_t> packet);

struct Hello {
    std::string player_name;
    std::uint32_t spec_hash = 0;
};

struct Welcome {
    std::uint32_t player_id = 0;
    std::uint32_t server_tick = 0;
    std::uint32_t spec_hash = 0;
    float spawn_x = 0.0f;
    float spawn_y = 64.0f;
    float spawn_z = 0.0f;
    std::uint16_t chunk_size_x = 32;
    std::uint16_t chunk_size_y = 32;
    std::uint16_t chunk_size_z = 32;
};

struct ChunkRequest {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t request_id = 0;
};

struct ChunkData {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t revision = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    std::vector<std::uint8_t> compressed;
};

struct BlockEdit {
    std::int32_t x = 0;
    std::uint16_t y = 0;
    std::int32_t z = 0;
    std::uint16_t block = 0;
    std::uint32_t sequence = 0;
};

struct BlockUpdate {
    std::int32_t x = 0;
    std::uint16_t y = 0;
    std::int32_t z = 0;
    std::uint16_t block = 0;
    std::uint32_t revision = 0;
};

struct PlayerState {
    std::uint32_t player_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::uint32_t last_move_sequence = 0;
};

struct Snapshot {
    std::uint32_t tick = 0;
    std::vector<PlayerState> players;
};

struct Disconnect {
    std::string reason;
};

struct Ping {
    std::uint32_t nonce = 0;
};

struct Pong {
    std::uint32_t nonce = 0;
};

// Client-authoritative movement sample. The server accepts it after sanity
// checks and broadcasts it to other players.
struct PlayerMove {
    std::uint32_t sequence = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
    float velocity_z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool on_ground = false;
    bool teleport = false;
};

struct MoveReject {
    std::uint32_t sequence = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint8_t reason = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encode_hello(const Hello& message);
[[nodiscard]] bool decode_hello(std::span<const std::uint8_t> payload, Hello& out);

[[nodiscard]] std::vector<std::uint8_t> encode_welcome(const Welcome& message);
[[nodiscard]] bool decode_welcome(std::span<const std::uint8_t> payload, Welcome& out);

[[nodiscard]] std::vector<std::uint8_t> encode_chunk_request(const ChunkRequest& message);
[[nodiscard]] bool decode_chunk_request(std::span<const std::uint8_t> payload,
                                        ChunkRequest& out);

[[nodiscard]] std::vector<std::uint8_t> encode_chunk_data(const ChunkData& message);
[[nodiscard]] bool decode_chunk_data(std::span<const std::uint8_t> payload,
                                     ChunkData& out);

[[nodiscard]] std::vector<std::uint8_t> encode_block_edit(const BlockEdit& message);
[[nodiscard]] bool decode_block_edit(std::span<const std::uint8_t> payload,
                                     BlockEdit& out);

[[nodiscard]] std::vector<std::uint8_t> encode_block_update(const BlockUpdate& message);
[[nodiscard]] bool decode_block_update(std::span<const std::uint8_t> payload,
                                       BlockUpdate& out);

[[nodiscard]] std::vector<std::uint8_t> encode_snapshot(const Snapshot& message);
[[nodiscard]] bool decode_snapshot(std::span<const std::uint8_t> payload, Snapshot& out);

[[nodiscard]] std::vector<std::uint8_t> encode_disconnect(const Disconnect& message);
[[nodiscard]] bool decode_disconnect(std::span<const std::uint8_t> payload,
                                     Disconnect& out);

[[nodiscard]] std::vector<std::uint8_t> encode_ping(const Ping& message);
[[nodiscard]] bool decode_ping(std::span<const std::uint8_t> payload, Ping& out);

[[nodiscard]] std::vector<std::uint8_t> encode_pong(const Pong& message);
[[nodiscard]] bool decode_pong(std::span<const std::uint8_t> payload, Pong& out);

[[nodiscard]] std::vector<std::uint8_t> encode_player_move(const PlayerMove& message);
[[nodiscard]] bool decode_player_move(std::span<const std::uint8_t> payload,
                                      PlayerMove& out);

[[nodiscard]] std::vector<std::uint8_t> encode_move_reject(const MoveReject& message);
[[nodiscard]] bool decode_move_reject(std::span<const std::uint8_t> payload,
                                      MoveReject& out);

} // namespace vox::net
