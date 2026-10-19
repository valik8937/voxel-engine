#pragma once

#include "render/camera.h"
#include "voxel/chunk_mesh.h"
#include "voxel/world.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vox {

struct RendererConfig {
    std::string title = "Voxel Engine";
    int width = 1280;
    int height = 720;
    bool vsync = true;
};

struct PlayerRenderInfo {
    std::uint32_t id = 0;
    Vec3 feet{};
    float yaw = 0.0f;

    bool operator==(const PlayerRenderInfo&) const = default;
};

using RenderObjectId = std::uint64_t;
inline constexpr RenderObjectId kInvalidRenderObject = 0;

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] bool initialize(const RendererConfig& config);
    void shutdown();

    // Call before begin_frame(). Uploads/rebuilds a bounded number of chunk
    // meshes so frame time stays predictable.
    void update_world(const World& world);
    void update_players(std::span<const PlayerRenderInfo> players);

    // Local-space meshes with their own model matrix. This is the foundation
    // for ships, rotating stations and other movable constructs.
    [[nodiscard]] RenderObjectId create_object(const ChunkMesh& local_mesh);
    [[nodiscard]] RenderObjectId create_box_object(Vec3 half_extents, Color top,
                                                   Color side, Color bottom);
    void update_object_transform(RenderObjectId object, const Mat4& model);
    void remove_object(RenderObjectId object);

    [[nodiscard]] bool begin_frame();
    void render(const Camera& camera);
    void end_frame();

    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }
    [[nodiscard]] SDL_GPUDevice* device() const noexcept { return device_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] std::size_t mesh_count() const noexcept {
        return meshes_.size() + objects_.size() +
               (player_mesh_.index_count != 0 ? 1u : 0u);
    }

private:
    struct GpuMesh {
        SDL_GPUBuffer* vertex_buffer = nullptr;
        SDL_GPUBuffer* index_buffer = nullptr;
        std::uint32_t index_count = 0;
        std::uint32_t revision = 0;
        std::array<std::uint32_t, 27> neighbour_revisions{};
        bool initialized = false;
    };

    [[nodiscard]] bool create_depth_texture();
    [[nodiscard]] bool create_pipelines();
    void upload_player_mesh(const ChunkMesh& mesh);
    [[nodiscard]] SDL_GPUShader* load_shader(const std::string& name,
                                             SDL_GPUShaderStage stage,
                                             std::uint32_t uniform_buffers);
    void rebuild_mesh(const World& world, ChunkCoord coord, GpuMesh& gpu_mesh);
    void upload_mesh(GpuMesh& gpu_mesh, const ChunkMesh& mesh);
    void release_mesh(GpuMesh& gpu_mesh);
    void release_all_meshes();

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* chunk_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* sky_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* hud_pipeline_ = nullptr;
    SDL_GPUTexture* depth_texture_ = nullptr;
    SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPUCommandBuffer* command_buffer_ = nullptr;
    SDL_GPUTexture* swapchain_texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool vsync_ = true;

    struct GpuObject {
        GpuMesh mesh;
        Mat4 model = Mat4::identity();
    };

    std::unordered_map<ChunkCoord, GpuMesh, ChunkCoordHash> meshes_;
    std::unordered_map<RenderObjectId, GpuObject> objects_;
    RenderObjectId next_object_id_ = 1;
    GpuMesh player_mesh_;
    std::uint32_t player_vertex_capacity_ = 0;
    std::uint32_t player_index_capacity_ = 0;
    std::vector<PlayerRenderInfo> last_players_;
    std::size_t max_mesh_rebuilds_per_frame_ = 4;
};

} // namespace vox
