#include "net/protocol.h"

#include <algorithm>
#include <cmath>

namespace vox::net {

std::vector<std::uint8_t> make_packet(PacketType type, const BinaryWriter& payload) {
    BinaryWriter writer(kHeaderSize + payload.size());
    writer.u32(kProtocolMagic);
    writer.u16(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(type));
    writer.bytes(payload.data());
    return std::move(writer.data());
}

std::optional<PacketView> parse_packet(std::span<const std::uint8_t> packet) {
    if (packet.size() < kHeaderSize || packet.size() > kMaxPacketSize) {
        return std::nullopt;
    }
    BinaryReader reader(packet);
    const std::uint32_t magic = reader.u32();
    const std::uint16_t version = reader.u16();
    const auto type = static_cast<PacketType>(reader.u8());
    if (!reader.ok() || magic != kProtocolMagic || version != kProtocolVersion) {
        return std::nullopt;
    }
    if (type < PacketType::Hello || type > PacketType::PlayerInput) {
        return std::nullopt;
    }
    return PacketView{type, packet.subspan(kHeaderSize)};
}

std::vector<std::uint8_t> encode_hello(const Hello& message) {
    BinaryWriter payload;
    payload.string(message.player_name);
    payload.u32(message.spec_hash);
    return make_packet(PacketType::Hello, payload);
}

bool decode_hello(std::span<const std::uint8_t> payload, Hello& out) {
    BinaryReader reader(payload);
    out.player_name = reader.string(64);
    out.spec_hash = reader.u32();
    return reader.ok() && !out.player_name.empty();
}

std::vector<std::uint8_t> encode_welcome(const Welcome& message) {
    BinaryWriter payload;
    payload.u32(message.player_id);
    payload.u32(message.server_tick);
    payload.u32(message.spec_hash);
    payload.f32(message.spawn_x);
    payload.f32(message.spawn_y);
    payload.f32(message.spawn_z);
    payload.u16(message.chunk_size_x);
    payload.u16(message.chunk_size_y);
    payload.u16(message.chunk_size_z);
    return make_packet(PacketType::Welcome, payload);
}

bool decode_welcome(std::span<const std::uint8_t> payload, Welcome& out) {
    BinaryReader reader(payload);
    out.player_id = reader.u32();
    out.server_tick = reader.u32();
    out.spec_hash = reader.u32();
    out.spawn_x = reader.f32();
    out.spawn_y = reader.f32();
    out.spawn_z = reader.f32();
    out.chunk_size_x = reader.u16();
    out.chunk_size_y = reader.u16();
    out.chunk_size_z = reader.u16();
    return reader.ok();
}

std::vector<std::uint8_t> encode_chunk_request(const ChunkRequest& message) {
    BinaryWriter payload;
    payload.i32(message.x);
    payload.i32(message.z);
    payload.u32(message.request_id);
    return make_packet(PacketType::ChunkRequest, payload);
}

bool decode_chunk_request(std::span<const std::uint8_t> payload, ChunkRequest& out) {
    BinaryReader reader(payload);
    out.x = reader.i32();
    out.z = reader.i32();
    out.request_id = reader.u32();
    return reader.ok();
}

std::vector<std::uint8_t> encode_chunk_data(const ChunkData& message) {
    BinaryWriter payload(32 + message.compressed.size());
    payload.i32(message.x);
    payload.i32(message.z);
    payload.u32(message.revision);
    payload.u32(message.uncompressed_size);
    payload.u32(message.compressed_size);
    payload.u32(message.crc32);
    payload.bytes(message.compressed);
    return make_packet(PacketType::ChunkData, payload);
}

bool decode_chunk_data(std::span<const std::uint8_t> payload, ChunkData& out) {
    BinaryReader reader(payload);
    out.x = reader.i32();
    out.z = reader.i32();
    out.revision = reader.u32();
    out.uncompressed_size = reader.u32();
    out.compressed_size = reader.u32();
    out.crc32 = reader.u32();
    if (!reader.ok() || out.compressed_size > kMaxChunkCompressedSize ||
        out.uncompressed_size > kMaxChunkUncompressedSize ||
        out.uncompressed_size == 0) {
        return false;
    }
    const auto compressed = reader.bytes(out.compressed_size);
    out.compressed.assign(compressed.begin(), compressed.end());
    return reader.ok() && out.compressed.size() == out.compressed_size;
}

std::vector<std::uint8_t> encode_block_edit(const BlockEdit& message) {
    BinaryWriter payload;
    payload.i32(message.x);
    payload.u16(message.y);
    payload.i32(message.z);
    payload.u16(message.block);
    payload.u32(message.sequence);
    return make_packet(PacketType::BlockEdit, payload);
}

bool decode_block_edit(std::span<const std::uint8_t> payload, BlockEdit& out) {
    BinaryReader reader(payload);
    out.x = reader.i32();
    out.y = reader.u16();
    out.z = reader.i32();
    out.block = reader.u16();
    out.sequence = reader.u32();
    return reader.ok() && out.y < 256;
}

std::vector<std::uint8_t> encode_block_update(const BlockUpdate& message) {
    BinaryWriter payload;
    payload.i32(message.x);
    payload.u16(message.y);
    payload.i32(message.z);
    payload.u16(message.block);
    payload.u32(message.revision);
    return make_packet(PacketType::BlockUpdate, payload);
}

bool decode_block_update(std::span<const std::uint8_t> payload, BlockUpdate& out) {
    BinaryReader reader(payload);
    out.x = reader.i32();
    out.y = reader.u16();
    out.z = reader.i32();
    out.block = reader.u16();
    out.revision = reader.u32();
    return reader.ok() && out.y < 256;
}

std::vector<std::uint8_t> encode_snapshot(const Snapshot& message) {
    BinaryWriter payload(8 + message.players.size() * 32);
    payload.u32(message.tick);
    payload.u16(static_cast<std::uint16_t>(message.players.size()));
    for (const PlayerState& player : message.players) {
        payload.u32(player.player_id);
        payload.f32(player.x);
        payload.f32(player.y);
        payload.f32(player.z);
        payload.f32(player.yaw);
        payload.f32(player.pitch);
        payload.u32(player.last_input);
    }
    return make_packet(PacketType::Snapshot, payload);
}

bool decode_snapshot(std::span<const std::uint8_t> payload, Snapshot& out) {
    BinaryReader reader(payload);
    out.tick = reader.u32();
    const std::uint16_t count = reader.u16();
    if (!reader.ok() || count > 256) {
        return false;
    }
    out.players.clear();
    out.players.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        PlayerState player;
        player.player_id = reader.u32();
        player.x = reader.f32();
        player.y = reader.f32();
        player.z = reader.f32();
        player.yaw = reader.f32();
        player.pitch = reader.f32();
        player.last_input = reader.u32();
        if (!reader.ok()) {
            return false;
        }
        out.players.push_back(player);
    }
    return reader.ok();
}

std::vector<std::uint8_t> encode_disconnect(const Disconnect& message) {
    BinaryWriter payload;
    payload.string(message.reason);
    return make_packet(PacketType::Disconnect, payload);
}

bool decode_disconnect(std::span<const std::uint8_t> payload, Disconnect& out) {
    BinaryReader reader(payload);
    out.reason = reader.string(512);
    return reader.ok();
}

std::vector<std::uint8_t> encode_ping(const Ping& message) {
    BinaryWriter payload;
    payload.u32(message.nonce);
    return make_packet(PacketType::Ping, payload);
}

bool decode_ping(std::span<const std::uint8_t> payload, Ping& out) {
    BinaryReader reader(payload);
    out.nonce = reader.u32();
    return reader.ok();
}

std::vector<std::uint8_t> encode_pong(const Pong& message) {
    BinaryWriter payload;
    payload.u32(message.nonce);
    return make_packet(PacketType::Pong, payload);
}

bool decode_pong(std::span<const std::uint8_t> payload, Pong& out) {
    BinaryReader reader(payload);
    out.nonce = reader.u32();
    return reader.ok();
}

std::vector<std::uint8_t> encode_player_input(const PlayerInput& message) {
    BinaryWriter payload;
    payload.u32(message.sequence);
    payload.f32(message.move_forward);
    payload.f32(message.move_right);
    payload.f32(message.yaw);
    payload.f32(message.pitch);
    payload.boolean(message.jump);
    payload.boolean(message.sprint);
    return make_packet(PacketType::PlayerInput, payload);
}

bool decode_player_input(std::span<const std::uint8_t> payload, PlayerInput& out) {
    BinaryReader reader(payload);
    out.sequence = reader.u32();
    out.move_forward = reader.f32();
    out.move_right = reader.f32();
    out.yaw = reader.f32();
    out.pitch = reader.f32();
    out.jump = reader.boolean();
    out.sprint = reader.boolean();
    return reader.ok() && std::isfinite(out.move_forward) &&
           std::isfinite(out.move_right) && std::isfinite(out.yaw) &&
           std::isfinite(out.pitch) && std::abs(out.move_forward) <= 1.0f &&
           std::abs(out.move_right) <= 1.0f;
}

} // namespace vox::net
