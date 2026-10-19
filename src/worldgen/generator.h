#pragma once

#include "voxel/chunk.h"
#include "worldgen/noise.h"

#include <cstdint>
#include <vector>

namespace vox {

enum class Biome : std::uint8_t {
    Ocean,
    Beach,
    Plains,
    Forest,
    Desert,
    Snow,
};

[[nodiscard]] const char* biome_name(Biome biome) noexcept;

struct WorldSpec {
    std::uint32_t seed = 1337;
    float sea_level = 63.0f;
    float base_height = 70.0f;
    float continent_amplitude = 22.0f;
    float hill_amplitude = 7.0f;
    float mountain_amplitude = 45.0f;

    NoiseSpec continent_noise;
    NoiseSpec hill_noise;
    NoiseSpec mountain_noise;
    NoiseSpec temperature_noise;
    NoiseSpec humidity_noise;
    NoiseSpec cave_noise;

    [[nodiscard]] std::vector<NoiseSpec> all_noises() const;
    [[nodiscard]] std::uint32_t hash() const;

    [[nodiscard]] static WorldSpec make_default(std::uint32_t seed = 1337);
};

struct TerrainColumn {
    float height = 0.0f;
    float min_height = 0.0f;
    float max_height = 0.0f;
    Biome biome = Biome::Plains;
};

class WorldGenerator {
public:
    explicit WorldGenerator(WorldSpec spec);

    [[nodiscard]] Chunk generate(ChunkCoord coord) const;

    // Useful for spawn points and gameplay queries. This is an exact sample,
    // not the 4x-upsampled chunk generator path.
    [[nodiscard]] float height_at(std::int64_t world_x, std::int64_t world_z) const;
    [[nodiscard]] Biome biome_at(std::int64_t world_x, std::int64_t world_z) const;

    [[nodiscard]] const WorldSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] std::uint32_t spec_hash() const noexcept { return spec_hash_; }

private:
    [[nodiscard]] float sample_height(const NoiseSampler& noise,
                                      float world_x, float world_z) const;
    [[nodiscard]] Biome sample_biome(const NoiseSampler& noise, float world_x,
                                     float world_z, float height) const;

    WorldSpec spec_;
    NoiseBank noise_bank_;
    std::uint32_t spec_hash_ = 0;
};

} // namespace vox
