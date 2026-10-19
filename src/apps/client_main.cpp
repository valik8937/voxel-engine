#include "net/client.h"
#include "net/server.h"
#include "voxel/block.h"
#include "voxel/chunk.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Options {
    bool singleplayer = true;
    bool edit_test = true;
    bool stay = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    std::string name = "player";
    int radius = 3;
    float timeout_seconds = 10.0f;
    std::uint32_t seed = 1337;
};

double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connect" && i + 1 < argc) {
            options.singleplayer = false;
            std::string endpoint = argv[++i];
            const auto colon = endpoint.find(':');
            if (colon == std::string::npos) {
                options.host = endpoint;
            } else {
                options.host = endpoint.substr(0, colon);
                options.port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
            }
        } else if (arg == "--singleplayer") {
            options.singleplayer = true;
        } else if (arg == "--name" && i + 1 < argc) {
            options.name = argv[++i];
        } else if (arg == "--radius" && i + 1 < argc) {
            options.radius = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            options.timeout_seconds = std::stof(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--stay") {
            options.stay = true;
        } else if (arg == "--no-edit") {
            options.edit_test = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: voxel_client [--singleplayer | --connect host:port] "
                         "[--name NAME] [--radius N] [--timeout SEC] [--seed N] "
                         "[--stay] [--no-edit]\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::exit(2);
        }
    }
    return options;
}

void pump(vox::net::GameServer* server, vox::net::GameClient& client,
          double& last_time, float& accumulator) {
    const double now = now_seconds();
    float dt = static_cast<float>(now - last_time);
    last_time = now;
    dt = std::min(dt, 0.25f);
    accumulator += dt;

    if (server != nullptr) {
        constexpr float kStep = 1.0f / 30.0f;
        int steps = 0;
        while (accumulator >= kStep && steps < 4) {
            server->tick(kStep);
            accumulator -= kStep;
            ++steps;
        }
    }
    client.poll(1);
}

int run_client(const Options& options, vox::net::GameServer* server) {
    vox::net::GameClient client;
    vox::net::ClientConfig config;
    config.host = options.host;
    config.port = options.port;
    config.player_name = options.name;
    if (!client.connect(config)) {
        std::cerr << "Connect failed: " << client.last_error() << "\n";
        return 1;
    }

    double last_time = now_seconds();
    float accumulator = 0.0f;
    const double deadline = last_time + options.timeout_seconds;

    while (!client.welcome() && now_seconds() < deadline) {
        pump(server, client, last_time, accumulator);
        if (!server) {
            std::this_thread::sleep_for(1ms);
        }
    }

    if (!client.welcome()) {
        std::cerr << "No welcome received: " << client.last_error() << "\n";
        return 1;
    }

    const auto welcome = *client.welcome();
    const auto spawn_chunk = vox::chunk_coord_at(
        static_cast<std::int64_t>(welcome.spawn_x),
        static_cast<std::int64_t>(welcome.spawn_z));

    std::size_t expected_chunks = 0;
    for (int z = -options.radius; z <= options.radius; ++z) {
        for (int x = -options.radius; x <= options.radius; ++x) {
            client.request_chunk(vox::ChunkCoord{spawn_chunk.x + x, spawn_chunk.z + z});
            ++expected_chunks;
        }
    }

    while (client.stats().chunks_received < expected_chunks && now_seconds() < deadline) {
        pump(server, client, last_time, accumulator);
        if (!server) {
            std::this_thread::sleep_for(1ms);
        }
    }

    std::cout << "Welcome: player_id=" << welcome.player_id
              << " spawn=" << welcome.spawn_x << "," << welcome.spawn_y << ","
              << welcome.spawn_z
              << " spec=" << std::hex << welcome.spec_hash << std::dec << "\n";
    std::cout << "Chunks: " << client.stats().chunks_received << "/" << expected_chunks
              << " received, " << client.stats().bytes_received << " compressed bytes, "
              << client.world().chunk_count() << " in world\n";
    std::cout << "Snapshots: " << client.stats().snapshots_received << "\n";

    if (options.edit_test) {
        const auto chunk = client.world().get(spawn_chunk);
        if (chunk) {
            const int lx = vox::local_x(static_cast<std::int64_t>(welcome.spawn_x));
            const int lz = vox::local_z(static_cast<std::int64_t>(welcome.spawn_z));
            const int y = chunk->highest_solid(lx, lz);
            if (y > 1) {
                const auto world_x = spawn_chunk.x * vox::kChunkSizeX + lx;
                const auto world_z = spawn_chunk.z * vox::kChunkSizeZ + lz;
                std::cout << "Edit test: set (" << world_x << "," << y << "," << world_z
                          << ") to air\n";
                std::uint32_t expected_revision = chunk->revision() + 1;
                client.send_block_edit(world_x, static_cast<std::uint16_t>(y), world_z,
                                       vox::blocks::Air);
                const double edit_deadline = now_seconds() + 2.0;
                while (now_seconds() < edit_deadline) {
                    pump(server, client, last_time, accumulator);
                    if (client.stats().block_updates_received > 0) {
                        break;
                    }
                    if (!server) {
                        std::this_thread::sleep_for(1ms);
                    }
                }
                if (server) {
                    const auto server_chunk = server->world().get(spawn_chunk);
                    if (server_chunk && server_chunk->revision() >= expected_revision) {
                        std::cout << "Server applied edit, revision="
                                  << server_chunk->revision() << "\n";
                    } else {
                        std::cout << "Server edit revision check: current="
                                  << (server_chunk ? server_chunk->revision() : 0)
                                  << " expected>=" << expected_revision << "\n";
                    }
                }
            }
        }
    }

    if (options.stay) {
        std::cout << "Staying connected. Press Ctrl+C to exit.\n";
        const double stay_deadline = now_seconds() + 3600.0;
        while (now_seconds() < stay_deadline) {
            pump(server, client, last_time, accumulator);
            if (!server) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    client.disconnect();
    return client.stats().chunks_received == expected_chunks ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    if (!options.singleplayer) {
        return run_client(options, nullptr);
    }

    vox::net::ServerConfig server_config;
    server_config.port = 0;
    server_config.seed = options.seed;
    server_config.world_directory = "worlds/singleplayer";
    server_config.max_players = 2;
    server_config.persist = true;
    vox::net::GameServer server(server_config);
    if (!server.start()) {
        std::cerr << "Cannot start embedded server\n";
        return 1;
    }

    Options local = options;
    local.host = "127.0.0.1";
    local.port = server.port();
    const int result = run_client(local, &server);
    server.stop();
    return result;
}
