#include "net/server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
    g_stop = 1;
}
} // namespace

int main(int argc, char** argv) {
    vox::net::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind" && i + 1 < argc) {
            config.bind_address = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            config.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-players" && i + 1 < argc) {
            config.max_players = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--world" && i + 1 < argc) {
            config.world_directory = argv[++i];
        } else if (arg == "--no-persist") {
            config.persist = false;
        } else if (arg == "--no-load") {
            config.load_world = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: voxel_server [--port N] [--bind ADDR] [--seed N] "
                         "[--max-players N] [--world DIR] [--no-persist] [--no-load]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    vox::net::GameServer server(config);
    if (!server.start()) {
        return 1;
    }

    using clock = std::chrono::steady_clock;
    constexpr auto step = std::chrono::milliseconds(33);
    auto next = clock::now();
    auto last_log = clock::now();

    while (!g_stop) {
        server.tick(1.0f / 30.0f);

        next += step;
        const auto now = clock::now();
        if (next < now) {
            next = now;
        }
        std::this_thread::sleep_until(next);

        if (now - last_log >= std::chrono::seconds(5)) {
            last_log = now;
            const auto& stats = server.stats();
            std::cout << "tick=" << server.current_tick()
                      << " players=" << server.connected_players()
                      << " chunks=" << server.world().chunk_count()
                      << " sent=" << stats.chunks_sent
                      << " generated=" << stats.chunks_generated
                      << " edits=" << stats.edits_applied
                      << " compressed=" << stats.compressed_bytes << " bytes\n";
        }
    }

    server.stop();
    return 0;
}
