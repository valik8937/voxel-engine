#include "audio/audio_engine.h"
#include "core/log.h"
#include "game/collision_streamer.h"
#include "game/player_input.h"
#include "net/client.h"
#include "physics/physics_world.h"
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
#include <memory>
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
    bool demo_ship = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    std::string name = "player";
    int radius = 6;
    int vertical_radius = 2;
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
        } else if (arg == "--vertical-radius" && i + 1 < argc) {
            options.vertical_radius = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            options.timeout_seconds = std::stof(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--stay") {
            options.stay = true;
        } else if (arg == "--demo-ship") {
            options.demo_ship = true;
        } else if (arg == "--no-edit") {
            options.edit_test = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: voxel_client [--singleplayer | --connect host:port] "
                         "[--headless] [--name NAME] [--radius N] [--vertical-radius N] "
                         "[--timeout SEC] [--seed N] [--stay] [--demo-ship] "
                         "[--no-edit]\n";
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
        static_cast<std::int64_t>(welcome.spawn_y),
        static_cast<std::int64_t>(welcome.spawn_z));
    const int min_chunk_y =
        std::max(0, spawn_chunk.y - options.vertical_radius);
    const int max_chunk_y =
        std::min(vox::kWorldHeightChunks - 1, spawn_chunk.y + options.vertical_radius);

    std::size_t expected_chunks = 0;
    for (int y = min_chunk_y; y <= max_chunk_y; ++y) {
        for (int z = -options.radius; z <= options.radius; ++z) {
            for (int x = -options.radius; x <= options.radius; ++x) {
                (void)client.request_chunk(
                    vox::ChunkCoord{spawn_chunk.x + x, y, spawn_chunk.z + z});
                ++expected_chunks;
            }
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
            const int local_y = chunk->highest_solid(lx, lz);
            const int y = spawn_chunk.y * vox::kChunkSizeY + local_y;
            if (local_y > 1) {
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

    vox::Camera camera;
    vox::PhysicsWorld physics;
    std::unique_ptr<vox::CollisionStreamer> collision_streamer;
    vox::PhysicsCharacter local_character = vox::kInvalidPhysicsCharacter;
    vox::ChunkCoord local_spawn_chunk{};
    bool local_collision_ready = false;
    bool chunks_requested = false;
    bool running = true;
    bool mouse_captured = false;
    std::uint32_t input_sequence = 0;
    std::uint32_t last_reject_sequence = 0;
    bool teleport_next_move = false;
    float input_accumulator = 0.0f;
    float collision_accumulator = 0.0f;
    double last_time = now_seconds();
    double last_stats = last_time;
    float accumulator = 0.0f;
    std::uint64_t frame_count = 0;
    const double welcome_deadline = last_time + options.timeout_seconds;
    std::size_t expected_chunks = 0;
    std::vector<vox::PlayerRenderInfo> remote_players;
    remote_players.reserve(16);
    vox::RenderObjectId demo_ship = vox::kInvalidRenderObject;
    vox::Vec3 demo_ship_anchor{};

    const auto refresh_local_collision = [&](int x, int y, int z) {
        if (!collision_streamer) {
            return;
        }
        const vox::ChunkCoord center =
            vox::chunk_coord_at(x, y, z);
        collision_streamer->invalidate(center);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const vox::ChunkCoord neighbour{center.x + dx, center.y + dy,
                                                    center.z + dz};
                    if (client.world().get(neighbour)) {
                        collision_streamer->invalidate(neighbour);
                    }
                }
            }
        }
        if (local_character != vox::kInvalidPhysicsCharacter) {
            const vox::Vec3 position = physics.character_position(local_character);
            (void)collision_streamer->update(
                vox::chunk_coord_at(static_cast<std::int64_t>(position.x),
                                    static_cast<std::int64_t>(position.y),
                                    static_cast<std::int64_t>(position.z)),
                2, 1, 1000);
        }
    };

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
            local_spawn_chunk = vox::chunk_coord_at(
                static_cast<std::int64_t>(welcome.spawn_x),
                static_cast<std::int64_t>(welcome.spawn_y),
                static_cast<std::int64_t>(welcome.spawn_z));

            if (physics.initialize()) {
                collision_streamer = std::make_unique<vox::CollisionStreamer>(
                    client.world(), physics);
                local_character = physics.create_character(
                    {welcome.spawn_x, welcome.spawn_y, welcome.spawn_z});
            }

            camera.position = vox::Vec3{welcome.spawn_x, welcome.spawn_y,
                                        welcome.spawn_z} +
                              vox::Vec3{0.0f, vox::kPlayerEyeHeight, 0.0f};
            camera.pitch = -0.15f;
            const int min_chunk_y =
                std::max(0, local_spawn_chunk.y - options.vertical_radius);
            const int max_chunk_y =
                std::min(vox::kWorldHeightChunks - 1,
                         local_spawn_chunk.y + options.vertical_radius);
            for (int y = min_chunk_y; y <= max_chunk_y; ++y) {
                for (int z = -options.radius; z <= options.radius; ++z) {
                    for (int x = -options.radius; x <= options.radius; ++x) {
                        (void)client.request_chunk(vox::ChunkCoord{
                            local_spawn_chunk.x + x, y, local_spawn_chunk.z + z});
                        ++expected_chunks;
                    }
                }
            }
            chunks_requested = true;
            teleport_next_move = false;
            if (options.demo_ship) {
                demo_ship = renderer.create_box_object(
                    {1.5f, 0.5f, 2.5f},
                    vox::Color{210, 120, 70, 255},
                    vox::Color{170, 85, 50, 255},
                    vox::Color{110, 60, 40, 255});
                demo_ship_anchor = vox::Vec3{welcome.spawn_x + 3.0f,
                                             welcome.spawn_y + 3.0f,
                                             welcome.spawn_z - 7.0f};
            }
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
                } else if (local_character != vox::kInvalidPhysicsCharacter) {
                    const auto hit = vox::raycast_voxels(
                        client.world(), camera.position, camera.forward(), 6.0f);
                    if (hit.hit) {
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            (void)client.send_block_edit(hit.x, static_cast<std::uint16_t>(hit.y),
                                                         hit.z, vox::blocks::Air);
                            (void)client.world().set_block(hit.x, hit.y, hit.z, vox::blocks::Air);
                            refresh_local_collision(hit.x, hit.y, hit.z);
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
                            const vox::Vec3 feet =
                                physics.character_position(local_character);
                            if (py >= 0 && py < vox::kWorldHeight &&
                                !target.overlaps(vox::player_aabb(feet))) {
                                (void)client.send_block_edit(px, static_cast<std::uint16_t>(py),
                                                             pz, vox::blocks::Stone);
                                (void)client.world().set_block(px, py, pz, vox::blocks::Stone);
                                refresh_local_collision(px, py, pz);
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

        physics.step(frame_dt);

        if (local_character != vox::kInvalidPhysicsCharacter) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            constexpr vox::Vec3 kGravity{0.0f, -24.0f, 0.0f};

            if (!local_collision_ready && collision_streamer) {
                (void)collision_streamer->update(local_spawn_chunk, 1, 1, 1000);
                local_collision_ready =
                    collision_streamer->has_chunk(local_spawn_chunk);
            }

            if (!local_collision_ready) {
                camera.position = vox::Vec3{client.welcome()->spawn_x,
                                            client.welcome()->spawn_y,
                                            client.welcome()->spawn_z} +
                                  vox::Vec3{0.0f, vox::kPlayerEyeHeight, 0.0f};
            }

            collision_accumulator += frame_dt;
            if (local_collision_ready && collision_accumulator >= 0.5f &&
                collision_streamer) {
                collision_accumulator = 0.0f;
                const vox::Vec3 position =
                    physics.character_position(local_character);
                (void)collision_streamer->update(
                    vox::chunk_coord_at(static_cast<std::int64_t>(position.x),
                                        static_cast<std::int64_t>(position.y),
                                        static_cast<std::int64_t>(position.z)),
                    2, 1, 8);
            }

            if (local_collision_ready) {
                input_accumulator += frame_dt;
                constexpr float kInputStep = 1.0f / 30.0f;
                while (input_accumulator >= kInputStep) {
                    input_accumulator -= kInputStep;
                    if (keys == nullptr) {
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

                    const vox::Vec3 feet =
                        physics.character_position(local_character);
                    const vox::ChunkCoord feet_chunk = vox::chunk_coord_at(
                        static_cast<std::int64_t>(feet.x),
                        static_cast<std::int64_t>(feet.y),
                        static_cast<std::int64_t>(feet.z));
                    if (collision_streamer &&
                        !collision_streamer->has_chunk(feet_chunk)) {
                        (void)collision_streamer->update(feet_chunk, 2, 1, 4);
                        continue;
                    }

                    physics.step_character(local_character,
                                           vox::player_wish_velocity(input),
                                           input.jump, kGravity, kInputStep);

                    const vox::Vec3 position =
                        physics.character_position(local_character);
                    const vox::Vec3 velocity =
                        physics.character_velocity(local_character);

                    vox::net::PlayerMove move;
                    move.sequence = ++input_sequence;
                    move.x = position.x;
                    move.y = position.y;
                    move.z = position.z;
                    move.velocity_x = velocity.x;
                    move.velocity_y = velocity.y;
                    move.velocity_z = velocity.z;
                    move.yaw = camera.yaw;
                    move.pitch = camera.pitch;
                    move.on_ground = physics.character_on_ground(local_character);
                    move.teleport = teleport_next_move;
                    if (client.send_player_move(move)) {
                        teleport_next_move = false;
                    }
                }

                if (const auto& reject = client.last_move_reject()) {
                    if (reject->sequence >= last_reject_sequence) {
                        last_reject_sequence = reject->sequence;
                        physics.set_character_position(
                            local_character, {reject->x, reject->y, reject->z});
                        VOX_LOG_WARN("Move rejected (reason {}), snapped to {:.1f},{:.1f},{:.1f}",
                                     reject->reason, reject->x, reject->y, reject->z);
                    }
                    client.clear_move_reject();
                }

                vox::Vec3 character_position =
                    physics.character_position(local_character);
                if (character_position.y < -16.0f && client.welcome()) {
                    const vox::Vec3 respawn{client.welcome()->spawn_x,
                                            client.welcome()->spawn_y,
                                            client.welcome()->spawn_z};
                    physics.set_character_position(local_character, respawn);
                    character_position = respawn;
                    teleport_next_move = true;
                }
                camera.position = character_position +
                                  vox::Vec3{0.0f, vox::kPlayerEyeHeight, 0.0f};
            }
        }

        if (demo_ship != vox::kInvalidRenderObject) {
            const float angle = static_cast<float>(now) * 0.8f;
            const vox::Vec3 bob = vox::Vec3{0.0f, std::sin(angle * 0.7f) * 0.35f, 0.0f};
            renderer.update_object_transform(
                demo_ship,
                vox::Mat4::trs(demo_ship_anchor + bob,
                               vox::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, angle),
                               {1.0f, 1.0f, 1.0f}));
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
            const vox::Vec3 debug_position =
                local_character != vox::kInvalidPhysicsCharacter
                    ? physics.character_position(local_character)
                    : camera.position;
            std::cout << "fps=" << static_cast<int>(fps + 0.5)
                      << " tick=" << client.server_tick()
                      << " chunks=" << client.world().chunk_count()
                      << " meshes=" << renderer.mesh_count()
                      << " received=" << client.stats().chunks_received
                      << " pos=" << debug_position.x << "," << debug_position.y
                      << "," << debug_position.z << "\n";
        }
    }

    SDL_SetWindowRelativeMouseMode(renderer.window(), false);
    if (collision_streamer) {
        collision_streamer->clear();
    }
    physics.shutdown();
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
