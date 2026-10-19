#include "render/renderer.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vox {
namespace {

struct CameraUniforms {
    float view_proj[16];
};

struct FogUniforms {
    float fog_color[4];
    float camera_pos[4];
    float params[4]; // x = start, y = end, z = time, w = unused
};

SDL_GPUTextureFormat choose_depth_format(SDL_GPUDevice* device) {
    constexpr SDL_GPUTextureFormat candidates[] = {
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        SDL_GPU_TEXTUREFORMAT_D16_UNORM,
    };
    for (const SDL_GPUTextureFormat format : candidates) {
        if (SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return format;
        }
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

Color shade(Color color, float light) {
    return Color{
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(color.r) * light, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(color.g) * light, 0.0f, 255.0f)),
        static_cast<std::uint8_t>(std::clamp(static_cast<float>(color.b) * light, 0.0f, 255.0f)),
        255,
    };
}

void append_box(ChunkMesh& mesh, Vec3 min, Vec3 max, Color top, Color side, Color bottom) {
    struct Face {
        int normal[3];
        int base[3];
        int u[3];
        int v[3];
        float light;
        Color color;
    };

    const Face faces[6] = {
        {{1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.72f, side},
        {{-1, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0.72f, side},
        {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, 1.00f, top},
        {{0, -1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 0.50f, bottom},
        {{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 0.82f, side},
        {{0, 0, -1}, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 0.82f, side},
    };

    const Vec3 size = max - min;
    for (const Face& face : faces) {
        const Color color = shade(face.color, face.light);
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        const int corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const auto& corner : corners) {
            const float x = min.x + size.x * static_cast<float>(face.base[0] +
                                                               face.u[0] * corner[0] +
                                                               face.v[0] * corner[1]);
            const float y = min.y + size.y * static_cast<float>(face.base[1] +
                                                               face.u[1] * corner[0] +
                                                               face.v[1] * corner[1]);
            const float z = min.z + size.z * static_cast<float>(face.base[2] +
                                                               face.u[2] * corner[0] +
                                                               face.v[2] * corner[1]);
            mesh.vertices.push_back(MeshVertex{
                x, y, z, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f,
            });
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
        (void)face.normal;
    }
}

} // namespace

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize(const RendererConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        VOX_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        VOX_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
    if (device_ == nullptr) {
        VOX_LOG_ERROR("SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        VOX_LOG_ERROR("SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return false;
    }

    vsync_ = config.vsync;
    if (!SDL_SetGPUSwapchainParameters(device_, window_,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       vsync_ ? SDL_GPU_PRESENTMODE_VSYNC
                                              : SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        VOX_LOG_WARN("SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
    }

    swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
        VOX_LOG_ERROR("Cannot query swapchain format: {}", SDL_GetError());
        return false;
    }

    depth_format_ = choose_depth_format(device_);
    if (depth_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
        VOX_LOG_ERROR("No supported depth format");
        return false;
    }

    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
    width_ = std::max(1, pixel_width);
    height_ = std::max(1, pixel_height);

    if (!create_depth_texture() || !create_pipelines()) {
        return false;
    }

    VOX_LOG_INFO("Renderer initialized: {}x{}, swapchain {}, depth {}, {}",
                 width_, height_, static_cast<int>(swapchain_format_),
                 static_cast<int>(depth_format_),
                 SDL_GetGPUDeviceDriver(device_));
    return true;
}

void Renderer::shutdown() {
    if (device_ != nullptr) {
        SDL_WaitForGPUIdle(device_);
        release_all_meshes();
        if (chunk_pipeline_ != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, chunk_pipeline_);
            chunk_pipeline_ = nullptr;
        }
        if (sky_pipeline_ != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, sky_pipeline_);
            sky_pipeline_ = nullptr;
        }
        if (hud_pipeline_ != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(device_, hud_pipeline_);
            hud_pipeline_ = nullptr;
        }
        if (depth_texture_ != nullptr) {
            SDL_ReleaseGPUTexture(device_, depth_texture_);
            depth_texture_ = nullptr;
        }
        if (window_ != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(device_, window_);
        }
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

SDL_GPUShader* Renderer::load_shader(const std::string& name,
                                     SDL_GPUShaderStage stage,
                                     std::uint32_t uniform_buffers) {
    const std::string path = std::string(VOX_SHADER_DIR) + "/" + name;
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr || size == 0) {
        VOX_LOG_ERROR("Cannot load shader '{}': {}", path, SDL_GetError());
        return nullptr;
    }

    SDL_GPUShaderCreateInfo info{};
    info.code = static_cast<const Uint8*>(data);
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_uniform_buffers = uniform_buffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    SDL_free(data);
    if (shader == nullptr) {
        VOX_LOG_ERROR("SDL_CreateGPUShader('{}') failed: {}", path, SDL_GetError());
    }
    return shader;
}

bool Renderer::create_depth_texture() {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = depth_format_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = static_cast<Uint32>(width_);
    info.height = static_cast<Uint32>(height_);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    depth_texture_ = SDL_CreateGPUTexture(device_, &info);
    if (depth_texture_ == nullptr) {
        VOX_LOG_ERROR("Cannot create depth texture: {}", SDL_GetError());
        return false;
    }
    return true;
}

bool Renderer::create_pipelines() {
    SDL_GPUShader* chunk_vs = load_shader("chunk.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader* chunk_fs = load_shader("chunk.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    SDL_GPUShader* sky_vs = load_shader("sky.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* sky_fs = load_shader("sky.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);
    SDL_GPUShader* hud_vs = load_shader("hud.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* hud_fs = load_shader("hud.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);

    if (chunk_vs == nullptr || chunk_fs == nullptr || sky_vs == nullptr ||
        sky_fs == nullptr || hud_vs == nullptr || hud_fs == nullptr) {
        if (chunk_vs) SDL_ReleaseGPUShader(device_, chunk_vs);
        if (chunk_fs) SDL_ReleaseGPUShader(device_, chunk_fs);
        if (sky_vs) SDL_ReleaseGPUShader(device_, sky_vs);
        if (sky_fs) SDL_ReleaseGPUShader(device_, sky_fs);
        if (hud_vs) SDL_ReleaseGPUShader(device_, hud_vs);
        if (hud_fs) SDL_ReleaseGPUShader(device_, hud_fs);
        return false;
    }

    SDL_GPUVertexBufferDescription vertex_buffer{};
    vertex_buffer.slot = 0;
    vertex_buffer.pitch = static_cast<Uint32>(sizeof(MeshVertex));
    vertex_buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertex_buffer.instance_step_rate = 0;

    SDL_GPUVertexAttribute attributes[2]{};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = static_cast<Uint32>(offsetof(MeshVertex, x));
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = static_cast<Uint32>(offsetof(MeshVertex, r));

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.vertex_buffer_descriptions = &vertex_buffer;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attributes;
    vertex_input.num_vertex_attributes = 2;

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = swapchain_format_;
    color_target.blend_state.enable_blend = false;

    SDL_GPUGraphicsPipelineCreateInfo pipeline{};
    pipeline.vertex_shader = chunk_vs;
    pipeline.fragment_shader = chunk_fs;
    pipeline.vertex_input_state = vertex_input;
    pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    pipeline.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline.rasterizer_state.enable_depth_clip = true;
    pipeline.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipeline.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipeline.depth_stencil_state.enable_depth_test = true;
    pipeline.depth_stencil_state.enable_depth_write = true;
    pipeline.target_info.color_target_descriptions = &color_target;
    pipeline.target_info.num_color_targets = 1;
    pipeline.target_info.depth_stencil_format = depth_format_;
    pipeline.target_info.has_depth_stencil_target = true;

    chunk_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline);

    SDL_GPUVertexInputState empty_vertex_input{};
    SDL_GPUGraphicsPipelineCreateInfo sky_pipeline_info{};
    sky_pipeline_info.vertex_shader = sky_vs;
    sky_pipeline_info.fragment_shader = sky_fs;
    sky_pipeline_info.vertex_input_state = empty_vertex_input;
    sky_pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    sky_pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    sky_pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    sky_pipeline_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    sky_pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    sky_pipeline_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    sky_pipeline_info.depth_stencil_state.enable_depth_test = false;
    sky_pipeline_info.depth_stencil_state.enable_depth_write = false;
    sky_pipeline_info.target_info.color_target_descriptions = &color_target;
    sky_pipeline_info.target_info.num_color_targets = 1;
    sky_pipeline_info.target_info.depth_stencil_format = depth_format_;
    sky_pipeline_info.target_info.has_depth_stencil_target = true;

    sky_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &sky_pipeline_info);

    SDL_GPUGraphicsPipelineCreateInfo hud_pipeline_info = sky_pipeline_info;
    hud_pipeline_info.vertex_shader = hud_vs;
    hud_pipeline_info.fragment_shader = hud_fs;
    hud_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &hud_pipeline_info);

    SDL_ReleaseGPUShader(device_, chunk_vs);
    SDL_ReleaseGPUShader(device_, chunk_fs);
    SDL_ReleaseGPUShader(device_, sky_vs);
    SDL_ReleaseGPUShader(device_, sky_fs);
    SDL_ReleaseGPUShader(device_, hud_vs);
    SDL_ReleaseGPUShader(device_, hud_fs);

    if (chunk_pipeline_ == nullptr || sky_pipeline_ == nullptr ||
        hud_pipeline_ == nullptr) {
        VOX_LOG_ERROR("Pipeline creation failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void Renderer::update_world(const World& world) {
    if (device_ == nullptr) {
        return;
    }

    const auto chunks = world.snapshot();
    std::unordered_map<ChunkCoord, std::shared_ptr<Chunk>, ChunkCoordHash> by_coord;
    by_coord.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk) {
            by_coord.emplace(chunk->coord(), chunk);
        }
    }

    std::size_t rebuilds = 0;
    for (const auto& [coord, chunk] : by_coord) {
        const auto neighbours = neighbours_3x3x3(coord);
        std::array<std::uint32_t, 27> revisions{};
        for (std::size_t i = 0; i < neighbours.size(); ++i) {
            const auto it = by_coord.find(neighbours[i]);
            if (it != by_coord.end() && it->second) {
                revisions[i] = it->second->revision();
            }
        }

        GpuMesh& gpu_mesh = meshes_[coord];
        if (gpu_mesh.initialized && gpu_mesh.revision == chunk->revision() &&
            gpu_mesh.neighbour_revisions == revisions) {
            continue;
        }
        if (rebuilds >= max_mesh_rebuilds_per_frame_) {
            break;
        }

        rebuild_mesh(world, coord, gpu_mesh);
        gpu_mesh.revision = chunk->revision();
        gpu_mesh.neighbour_revisions = revisions;
        gpu_mesh.initialized = true;
        ++rebuilds;
    }
}

void Renderer::update_players(std::span<const PlayerRenderInfo> players) {
    if (device_ == nullptr) {
        return;
    }
    if (players.size() == last_players_.size() &&
        std::equal(players.begin(), players.end(), last_players_.begin())) {
        return;
    }
    last_players_.assign(players.begin(), players.end());

    ChunkMesh mesh;
    mesh.bounds.min = Vec3{std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::max()};
    mesh.bounds.max = Vec3{-std::numeric_limits<float>::max(),
                           -std::numeric_limits<float>::max(),
                           -std::numeric_limits<float>::max()};

    const Color body_top{86, 126, 210, 255};
    const Color body_side{62, 96, 166, 255};
    const Color body_bottom{40, 62, 110, 255};
    const Color head_top{224, 184, 150, 255};
    const Color head_side{196, 154, 122, 255};
    const Color head_bottom{160, 124, 100, 255};

    for (const PlayerRenderInfo& player : players) {
        const Vec3 feet = player.feet;
        const Vec3 body_min{feet.x - 0.3f, feet.y, feet.z - 0.3f};
        const Vec3 body_max{feet.x + 0.3f, feet.y + 1.5f, feet.z + 0.3f};
        const Vec3 head_min{feet.x - 0.24f, feet.y + 1.5f, feet.z - 0.24f};
        const Vec3 head_max{feet.x + 0.24f, feet.y + 1.8f, feet.z + 0.24f};

        append_box(mesh, body_min, body_max, body_top, body_side, body_bottom);
        append_box(mesh, head_min, head_max, head_top, head_side, head_bottom);
        mesh.bounds.min.x = std::min(mesh.bounds.min.x, body_min.x);
        mesh.bounds.min.y = std::min(mesh.bounds.min.y, body_min.y);
        mesh.bounds.min.z = std::min(mesh.bounds.min.z, body_min.z);
        mesh.bounds.max.x = std::max(mesh.bounds.max.x, body_max.x);
        mesh.bounds.max.y = std::max(mesh.bounds.max.y, head_max.y);
        mesh.bounds.max.z = std::max(mesh.bounds.max.z, body_max.z);
    }

    upload_player_mesh(mesh);
}

void Renderer::rebuild_mesh(const World& world, ChunkCoord coord, GpuMesh& gpu_mesh) {
    ChunkMesh mesh = mesh_chunk(world, coord);
    upload_mesh(gpu_mesh, mesh);
}

void Renderer::upload_mesh(GpuMesh& gpu_mesh, const ChunkMesh& mesh) {
    if (mesh.empty()) {
        if (gpu_mesh.vertex_buffer != nullptr || gpu_mesh.index_buffer != nullptr) {
            SDL_WaitForGPUIdle(device_);
            release_mesh(gpu_mesh);
        }
        gpu_mesh.index_count = 0;
        return;
    }

    const std::size_t vertex_bytes = mesh.vertices.size() * sizeof(MeshVertex);
    const std::size_t index_bytes = mesh.indices.size() * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<Uint32>::max() ||
        index_bytes > std::numeric_limits<Uint32>::max()) {
        VOX_LOG_ERROR("Chunk mesh too large");
        return;
    }

    if (gpu_mesh.vertex_buffer != nullptr || gpu_mesh.index_buffer != nullptr) {
        SDL_WaitForGPUIdle(device_);
        release_mesh(gpu_mesh);
    }

    SDL_GPUBufferCreateInfo vertex_info{};
    vertex_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertex_info.size = static_cast<Uint32>(vertex_bytes);
    SDL_GPUBuffer* vertex_buffer = SDL_CreateGPUBuffer(device_, &vertex_info);

    SDL_GPUBufferCreateInfo index_info{};
    index_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    index_info.size = static_cast<Uint32>(index_bytes);
    SDL_GPUBuffer* index_buffer = SDL_CreateGPUBuffer(device_, &index_info);

    if (vertex_buffer == nullptr || index_buffer == nullptr) {
        VOX_LOG_ERROR("Cannot create chunk GPU buffers: {}", SDL_GetError());
        if (vertex_buffer) SDL_ReleaseGPUBuffer(device_, vertex_buffer);
        if (index_buffer) SDL_ReleaseGPUBuffer(device_, index_buffer);
        return;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(vertex_bytes + index_bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (transfer == nullptr) {
        VOX_LOG_ERROR("Cannot create transfer buffer: {}", SDL_GetError());
        SDL_ReleaseGPUBuffer(device_, vertex_buffer);
        SDL_ReleaseGPUBuffer(device_, index_buffer);
        return;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (mapped == nullptr) {
        VOX_LOG_ERROR("Cannot map transfer buffer: {}", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUBuffer(device_, vertex_buffer);
        SDL_ReleaseGPUBuffer(device_, index_buffer);
        return;
    }
    std::memcpy(mapped, mesh.vertices.data(), vertex_bytes);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vertex_bytes,
                mesh.indices.data(), index_bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (command == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUBuffer(device_, vertex_buffer);
        SDL_ReleaseGPUBuffer(device_, index_buffer);
        return;
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transfer;
    source.offset = 0;

    SDL_GPUBufferRegion vertex_region{};
    vertex_region.buffer = vertex_buffer;
    vertex_region.offset = 0;
    vertex_region.size = static_cast<Uint32>(vertex_bytes);
    SDL_UploadToGPUBuffer(copy, &source, &vertex_region, false);

    source.offset = static_cast<Uint32>(vertex_bytes);
    SDL_GPUBufferRegion index_region{};
    index_region.buffer = index_buffer;
    index_region.offset = 0;
    index_region.size = static_cast<Uint32>(index_bytes);
    SDL_UploadToGPUBuffer(copy, &source, &index_region, false);

    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(command)) {
        VOX_LOG_ERROR("Cannot submit chunk upload: {}", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUBuffer(device_, vertex_buffer);
        SDL_ReleaseGPUBuffer(device_, index_buffer);
        return;
    }

    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    gpu_mesh.vertex_buffer = vertex_buffer;
    gpu_mesh.index_buffer = index_buffer;
    gpu_mesh.index_count = static_cast<std::uint32_t>(mesh.indices.size());
}

void Renderer::upload_player_mesh(const ChunkMesh& mesh) {
    if (mesh.empty()) {
        player_mesh_.index_count = 0;
        return;
    }

    const std::size_t vertex_bytes = mesh.vertices.size() * sizeof(MeshVertex);
    const std::size_t index_bytes = mesh.indices.size() * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<Uint32>::max() ||
        index_bytes > std::numeric_limits<Uint32>::max()) {
        return;
    }

    const auto round_up = [](std::uint32_t value) {
        constexpr std::uint32_t alignment = 4096;
        return (value + alignment - 1) & ~(alignment - 1);
    };
    const std::uint32_t vertex_size = round_up(static_cast<std::uint32_t>(vertex_bytes));
    const std::uint32_t index_size = round_up(static_cast<std::uint32_t>(index_bytes));

    if (player_mesh_.vertex_buffer == nullptr || player_vertex_capacity_ < vertex_size) {
        SDL_WaitForGPUIdle(device_);
        if (player_mesh_.vertex_buffer != nullptr) {
            SDL_ReleaseGPUBuffer(device_, player_mesh_.vertex_buffer);
            player_mesh_.vertex_buffer = nullptr;
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = vertex_size;
        player_mesh_.vertex_buffer = SDL_CreateGPUBuffer(device_, &info);
        player_vertex_capacity_ = vertex_size;
    }
    if (player_mesh_.index_buffer == nullptr || player_index_capacity_ < index_size) {
        SDL_WaitForGPUIdle(device_);
        if (player_mesh_.index_buffer != nullptr) {
            SDL_ReleaseGPUBuffer(device_, player_mesh_.index_buffer);
            player_mesh_.index_buffer = nullptr;
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        info.size = index_size;
        player_mesh_.index_buffer = SDL_CreateGPUBuffer(device_, &info);
        player_index_capacity_ = index_size;
    }
    if (player_mesh_.vertex_buffer == nullptr || player_mesh_.index_buffer == nullptr) {
        VOX_LOG_ERROR("Cannot create player GPU buffers: {}", SDL_GetError());
        return;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(vertex_bytes + index_bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (transfer == nullptr) {
        return;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return;
    }
    std::memcpy(mapped, mesh.vertices.data(), vertex_bytes);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vertex_bytes, mesh.indices.data(),
                index_bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    if (command == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transfer;
    source.offset = 0;

    SDL_GPUBufferRegion vertex_region{};
    vertex_region.buffer = player_mesh_.vertex_buffer;
    vertex_region.offset = 0;
    vertex_region.size = static_cast<Uint32>(vertex_bytes);
    SDL_UploadToGPUBuffer(copy, &source, &vertex_region, true);

    source.offset = static_cast<Uint32>(vertex_bytes);
    SDL_GPUBufferRegion index_region{};
    index_region.buffer = player_mesh_.index_buffer;
    index_region.offset = 0;
    index_region.size = static_cast<Uint32>(index_bytes);
    SDL_UploadToGPUBuffer(copy, &source, &index_region, true);

    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(command)) {
        VOX_LOG_ERROR("Cannot submit player upload: {}", SDL_GetError());
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    player_mesh_.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    player_mesh_.initialized = true;
}

void Renderer::release_mesh(GpuMesh& gpu_mesh) {
    if (gpu_mesh.vertex_buffer != nullptr) {
        SDL_ReleaseGPUBuffer(device_, gpu_mesh.vertex_buffer);
        gpu_mesh.vertex_buffer = nullptr;
    }
    if (gpu_mesh.index_buffer != nullptr) {
        SDL_ReleaseGPUBuffer(device_, gpu_mesh.index_buffer);
        gpu_mesh.index_buffer = nullptr;
    }
    gpu_mesh.index_count = 0;
    gpu_mesh.initialized = false;
}

void Renderer::release_all_meshes() {
    for (auto& [coord, mesh] : meshes_) {
        (void)coord;
        release_mesh(mesh);
    }
    meshes_.clear();
    release_mesh(player_mesh_);
    player_vertex_capacity_ = 0;
    player_index_capacity_ = 0;
    last_players_.clear();
}

bool Renderer::begin_frame() {
    if (device_ == nullptr || window_ == nullptr) {
        return false;
    }

    command_buffer_ = SDL_AcquireGPUCommandBuffer(device_);
    if (command_buffer_ == nullptr) {
        return false;
    }

    Uint32 swapchain_width = 0;
    Uint32 swapchain_height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer_, window_,
                                               &swapchain_texture_, &swapchain_width,
                                               &swapchain_height)) {
        SDL_SubmitGPUCommandBuffer(command_buffer_);
        command_buffer_ = nullptr;
        return false;
    }

    if (swapchain_texture_ == nullptr) {
        SDL_SubmitGPUCommandBuffer(command_buffer_);
        command_buffer_ = nullptr;
        return false;
    }

    if (static_cast<int>(swapchain_width) != width_ ||
        static_cast<int>(swapchain_height) != height_) {
        SDL_WaitForGPUIdle(device_);
        if (depth_texture_ != nullptr) {
            SDL_ReleaseGPUTexture(device_, depth_texture_);
            depth_texture_ = nullptr;
        }
        width_ = static_cast<int>(swapchain_width);
        height_ = static_cast<int>(swapchain_height);
        if (!create_depth_texture()) {
            SDL_SubmitGPUCommandBuffer(command_buffer_);
            command_buffer_ = nullptr;
            return false;
        }
    }
    return true;
}

void Renderer::render(const Camera& camera) {
    if (command_buffer_ == nullptr || swapchain_texture_ == nullptr) {
        return;
    }

    CameraUniforms camera_uniforms{};
    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const Mat4 view_projection = camera.view_projection(aspect);
    std::memcpy(camera_uniforms.view_proj, view_projection.m, sizeof(camera_uniforms.view_proj));
    SDL_PushGPUVertexUniformData(command_buffer_, 0, &camera_uniforms,
                                 sizeof(camera_uniforms));

    FogUniforms fog{};
    fog.fog_color[0] = 0.55f;
    fog.fog_color[1] = 0.72f;
    fog.fog_color[2] = 0.92f;
    fog.fog_color[3] = 1.0f;
    fog.camera_pos[0] = camera.position.x;
    fog.camera_pos[1] = camera.position.y;
    fog.camera_pos[2] = camera.position.z;
    fog.camera_pos[3] = 0.0f;
    fog.params[0] = 96.0f;
    fog.params[1] = 448.0f;
    fog.params[2] = static_cast<float>(SDL_GetTicks()) * 0.001f;
    fog.params[3] = 0.0f;
    SDL_PushGPUFragmentUniformData(command_buffer_, 0, &fog, sizeof(fog));

    SDL_GPUColorTargetInfo color_target{};
    color_target.texture = swapchain_texture_;
    color_target.clear_color = SDL_FColor{0.55f, 0.72f, 0.92f, 1.0f};
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth_target{};
    depth_target.texture = depth_texture_;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer_, &color_target, 1,
                                                     &depth_target);
    if (pass == nullptr) {
        return;
    }

    if (sky_pipeline_ != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, sky_pipeline_);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }

    if (chunk_pipeline_ != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, chunk_pipeline_);
        const auto draw_mesh = [&](const GpuMesh& mesh) {
            if (mesh.index_count == 0 || mesh.vertex_buffer == nullptr ||
                mesh.index_buffer == nullptr) {
                return;
            }
            SDL_GPUBufferBinding vertex_binding{};
            vertex_binding.buffer = mesh.vertex_buffer;
            vertex_binding.offset = 0;
            SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);

            SDL_GPUBufferBinding index_binding{};
            index_binding.buffer = mesh.index_buffer;
            index_binding.offset = 0;
            SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

            SDL_DrawGPUIndexedPrimitives(pass, mesh.index_count, 1, 0, 0, 0);
        };

        for (const auto& [coord, mesh] : meshes_) {
            (void)coord;
            draw_mesh(mesh);
        }
        draw_mesh(player_mesh_);
    }

    if (hud_pipeline_ != nullptr) {
        SDL_BindGPUGraphicsPipeline(pass, hud_pipeline_);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }

    SDL_EndGPURenderPass(pass);
}

void Renderer::end_frame() {
    if (command_buffer_ != nullptr) {
        SDL_SubmitGPUCommandBuffer(command_buffer_);
        command_buffer_ = nullptr;
    }
    swapchain_texture_ = nullptr;
}

} // namespace vox
