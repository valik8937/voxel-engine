#include "audio/audio_engine.h"
#include "core/log.h"
#include "game/player_controller.h"
#include "net/client.h"
#include "net/server.h"
#include "render/camera.h"
#include "render/renderer.h"
#include "voxel/block.h"
#include "voxel/chunk.h"
#include "voxel/raycast.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Options {
    bool singleplayer = true;
    bool headless = false;
    bool edit_test = true;
    bool stay = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    std::string name = "player";
    int radius = 6;
    float timeout_seconds = 15.0f;
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
        } else if (arg == "--headless") {
            options.headless = true;
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
                         "[--headless] [--name NAME] [--radius N] [--timeout SEC] "
                         "[--seed N] [--stay] [--no-edit]\n";
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

int run_headless(const Options& options, vox::net::GameServer* server) {
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
            (void)client.request_chunk(vox::ChunkCoord{spawn_chunk.x + x, spawn_chunk.z + z});
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
                const std::uint32_t expected_revision = chunk->revision() + 1;
                (void)client.send_block_edit(world_x, static_cast<std::uint16_t>(y), world_z,
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

int run_rendered(const Options& options, vox::net::GameServer* server) {
    vox::Renderer renderer;
    vox::RendererConfig renderer_config;
    renderer_config.title = "Voxel Engine";
    renderer_config.width = 1600;
    renderer_config.height = 900;
    renderer_config.vsync = true;
    if (!renderer.initialize(renderer_config)) {
        std::cerr << "Renderer initialization failed\n";
        return 1;
    }

    vox::AudioEngine audio;
    if (audio.initialize()) {
        audio.set_volume(0.35f);
        (void)audio.play_sound("assets/audio/click.wav");
    }

    vox::net::GameClient client;
    vox::net::ClientConfig config;
    config.host = options.host;
    config.port = options.port;
    config.player_name = options.name;
    if (!client.connect(config)) {
        std::cerr << "Connect failed: " << client.last_error() << "\n";
        audio.shutdown();
        renderer.shutdown();
        return 1;
    }

    SDL_RaiseWindow(renderer.window());

    vox::Camera camera;
    vox::PlayerMoveState local_player;
    vox::ChunkCoord local_spawn_chunk{};
    bool has_local_player = false;
    bool chunks_requested = false;
    bool running = true;
    bool mouse_captured = false;
    std::uint32_t input_sequence = 0;
    float input_accumulator = 0.0f;
    double last_time = now_seconds();
    double last_stats = last_time;
    float accumulator = 0.0f;
    std::uint64_t frame_count = 0;
    const double welcome_deadline = last_time + options.timeout_seconds;
    std::size_t expected_chunks = 0;
    std::vector<vox::PlayerRenderInfo> remote_players;
    remote_players.reserve(16);

    while (running) {
        const double now = now_seconds();
        float frame_dt = static_cast<float>(now - last_time);
        last_time = now;
        frame_dt = std::clamp(frame_dt, 0.0f, 0.1f);

        if (server != nullptr) {
            constexpr float kStep = 1.0f / 30.0f;
            accumulator += frame_dt;
            int steps = 0;
            while (accumulator >= kStep && steps < 4) {
                server->tick(kStep);
                accumulator -= kStep;
                ++steps;
            }
        }
        client.poll(0);

        remote_players.clear();
        if (client.welcome() && client.last_snapshot()) {
            for (const vox::net::PlayerState& state : client.last_snapshot()->players) {
                if (state.player_id == client.player_id()) {
                    continue;
                }
                remote_players.push_back(vox::PlayerRenderInfo{
                    state.player_id,
                    vox::Vec3{state.x, state.y, state.z},
                    state.yaw,
                });
            }
        }
        renderer.update_players(remote_players);

        if (client.welcome() && !chunks_requested) {
            const auto welcome = *client.welcome();
            local_player.position = vox::Vec3{welcome.spawn_x, welcome.spawn_y,
                                              welcome.spawn_z};
            local_player.velocity = {};
            local_player.on_ground = false;
            has_local_player = true;
            local_spawn_chunk = vox::chunk_coord_at(
                static_cast<std::int64_t>(welcome.spawn_x),
                static_cast<std::int64_t>(welcome.spawn_z));
            camera.position = local_player.position +
                              vox::Vec3{0.0f, vox::kPlayerEyeHeight, 0.0f};
            camera.pitch = -0.15f;
            for (int z = -options.radius; z <= options.radius; ++z) {
                for (int x = -options.radius; x <= options.radius; ++x) {
                    (void)client.request_chunk(vox::ChunkCoord{
                        local_spawn_chunk.x + x, local_spawn_chunk.z + z});
                    ++expected_chunks;
                }
            }
            chunks_requested = true;
            std::cout << "Connected to world, spawn " << welcome.spawn_x << ","
                      << welcome.spawn_y << "," << welcome.spawn_z << ", requested "
                      << expected_chunks << " chunks\n";
            std::cout << "Click to capture the mouse. Esc releases it, F1 toggles. "
                         "LMB breaks, RMB places stone.\n";
        }

        if (!client.welcome() && now > welcome_deadline) {
            std::cerr << "No welcome received: " << client.last_error() << "\n";
            running = false;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                if (mouse_captured) {
                    mouse_captured = false;
                    SDL_SetWindowRelativeMouseMode(renderer.window(), false);
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (mouse_captured) {
                        mouse_captured = false;
                        SDL_SetWindowRelativeMouseMode(renderer.window(), false);
                    } else {
                        running = false;
                    }
                } else if (event.key.scancode == SDL_SCANCODE_F1) {
                    mouse_captured = !mouse_captured;
                    SDL_SetWindowRelativeMouseMode(renderer.window(), mouse_captured);
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (!mouse_captured) {
                    mouse_captured = true;
                    SDL_SetWindowRelativeMouseMode(renderer.window(), true);
                } else if (has_local_player) {
                    const auto hit = vox::raycast_voxels(
                        client.world(), camera.position, camera.forward(), 6.0f);
                    if (hit.hit) {
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            (void)client.send_block_edit(hit.x, static_cast<std::uint16_t>(hit.y),
                                                         hit.z, vox::blocks::Air);
                            (void)client.world().set_block(hit.x, hit.y, hit.z, vox::blocks::Air);
                            (void)audio.play_sound("assets/audio/click.wav");
                        } else if (event.button.button == SDL_BUTTON_RIGHT) {
                            const int px = hit.x + hit.normal_x;
                            const int py = hit.y + hit.normal_y;
                            const int pz = hit.z + hit.normal_z;
                            const vox::Aabb target{
                                vox::Vec3{static_cast<float>(px), static_cast<float>(py),
                                          static_cast<float>(pz)},
                                vox::Vec3{static_cast<float>(px + 1),
                                          static_cast<float>(py + 1),
                                          static_cast<float>(pz + 1)},
                            };
                            if (py >= 0 && py < vox::kChunkSizeY &&
                                !target.overlaps(vox::player_aabb(local_player.position))) {
                                (void)client.send_block_edit(px, static_cast<std::uint16_t>(py),
                                                             pz, vox::blocks::Stone);
                                (void)client.world().set_block(px, py, pz, vox::blocks::Stone);
                                (void)audio.play_sound("assets/audio/click.wav");
                            }
                        }
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && mouse_captured) {
                camera.yaw += event.motion.xrel * 0.0025f;
                camera.pitch -= event.motion.yrel * 0.0025f;
                camera.pitch = std::clamp(camera.pitch, -1.55f, 1.55f);
            }
        }

        if (has_local_player) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            const bool spawn_ready =
                client.world().get(local_spawn_chunk) != nullptr;

            input_accumulator += frame_dt;
            constexpr float kInputStep = 1.0f / 30.0f;
            while (input_accumulator >= kInputStep) {
                input_accumulator -= kInputStep;
                if (!spawn_ready || keys == nullptr) {
                    continue;
                }

                vox::PlayerMoveInput input;
                input.move_forward = (keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) -
                                     (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f);
                input.move_right = (keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) -
                                   (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f);
                input.jump = keys[SDL_SCANCODE_SPACE];
                input.sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                input.yaw = camera.yaw;
                input.pitch = camera.pitch;

                vox::step_player(client.world(), local_player, input, kInputStep);

                vox::net::PlayerInput network_input;
                network_input.sequence = ++input_sequence;
                network_input.move_forward = input.move_forward;
                network_input.move_right = input.move_right;
                network_input.yaw = input.yaw;
                network_input.pitch = input.pitch;
                network_input.jump = input.jump;
                network_input.sprint = input.sprint;
                (void)client.send_player_input(network_input);
            }

            if (const vox::net::PlayerState* server_self = client.local_player()) {
                const vox::Vec3 server_position{server_self->x, server_self->y,
                                                server_self->z};
                if (vox::length(local_player.position - server_position) > 3.0f) {
                    local_player.position = server_position;
                    local_player.velocity = {};
                }
            }

            camera.position = local_player.position +
                              vox::Vec3{0.0f, vox::kPlayerEyeHeight, 0.0f};
        }

        renderer.update_world(client.world());

        if (renderer.begin_frame()) {
            renderer.render(camera);
            renderer.end_frame();
            ++frame_count;
        }

        if (now - last_stats >= 2.0) {
            const double elapsed = now - last_stats;
            last_stats = now;
            const double fps = static_cast<double>(frame_count) / std::max(elapsed, 0.001);
            frame_count = 0;
            std::cout << "fps=" << static_cast<int>(fps + 0.5)
                      << " tick=" << client.server_tick()
                      << " chunks=" << client.world().chunk_count()
                      << " meshes=" << renderer.mesh_count()
                      << " received=" << client.stats().chunks_received
                      << "\n";
        }
    }

    SDL_SetWindowRelativeMouseMode(renderer.window(), false);
    client.disconnect();
    audio.shutdown();
    renderer.shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    if (!options.singleplayer) {
        return options.headless ? run_headless(options, nullptr)
                                : run_rendered(options, nullptr);
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
    const int result = local.headless ? run_headless(local, &server)
                                      : run_rendered(local, &server);
    server.stop();
    return result;
}
