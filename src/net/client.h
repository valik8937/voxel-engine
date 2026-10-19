#pragma once

#include "core/zstd.h"
#include "net/protocol.h"
#include "voxel/world.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct _ENetHost;
struct _ENetPacket;
struct _ENetPeer;

namespace vox::net {

struct ClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    std::string player_name = "player";
    std::uint32_t expected_spec_hash = 0;
};

class GameClient {
public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Failed,
    };

    struct Stats {
        std::uint64_t chunks_received = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t snapshots_received = 0;
        std::uint64_t block_updates_received = 0;
        std::uint64_t chunks_requested = 0;
    };

    GameClient();
    ~GameClient();

    GameClient(const GameClient&) = delete;
    GameClient& operator=(const GameClient&) = delete;

    [[nodiscard]] bool connect(const ClientConfig& config);
    void disconnect();
    void poll(std::uint32_t timeout_ms = 0);

    [[nodiscard]] bool request_chunk(ChunkCoord coord);
    [[nodiscard]] bool send_block_edit(std::int32_t x, std::uint16_t y, std::int32_t z,
                                       BlockId block);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] const std::optional<Welcome>& welcome() const noexcept { return welcome_; }
    [[nodiscard]] const std::optional<Snapshot>& last_snapshot() const noexcept {
        return last_snapshot_;
    }
    [[nodiscard]] std::uint32_t player_id() const noexcept;
    [[nodiscard]] std::uint32_t server_tick() const noexcept;
    [[nodiscard]] const World& world() const noexcept { return world_; }
    [[nodiscard]] World& world() noexcept { return world_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void handle_packet(_ENetPacket* packet);
    void send_packet(std::span<const std::uint8_t> data, bool reliable,
                     std::uint8_t channel);

    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    State state_ = State::Disconnected;
    std::string last_error_;
    std::string player_name_;
    std::uint32_t expected_spec_hash_ = 0;
    std::uint32_t next_request_id_ = 1;
    std::uint32_t next_edit_sequence_ = 1;

    std::optional<Welcome> welcome_;
    std::optional<Snapshot> last_snapshot_;
    World world_;
    ZstdDecompressor decompressor_;
    Stats stats_;
};

} // namespace vox::net
