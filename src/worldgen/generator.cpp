#include "worldgen/generator.h"

#include "core/binary.h"
#include "core/hash.h"
#include "core/math.h"
#include "voxel/block.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace vox {
namespace {

constexpr int kHeightGrid = kChunkSizeX / 4 + 1; // 9
constexpr int kCaveGridY = kChunkSizeY / 4 + 1;  // 65
constexpr int kGridStep = 4;

float smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float bilerp(const std::array<float, kHeightGrid * kHeightGrid>& grid,
             int ix, int iz, float tx, float tz) {
    const float a = grid[static_cast<std::size_t>(iz) * kHeightGrid + ix];
    const float b = grid[static_cast<std::size_t>(iz) * kHeightGrid + ix + 1];
    const float c = grid[static_cast<std::size_t>(iz + 1) * kHeightGrid + ix];
    const float d = grid[static_cast<std::size_t>(iz + 1) * kHeightGrid + ix + 1];
    return lerp(lerp(a, b, tx), lerp(c, d, tx), tz);
}

float trilerp(const std::array<float, kHeightGrid * kCaveGridY * kHeightGrid>& grid,
              int ix, int iy, int iz, float tx, float ty, float tz) {
    const auto at = [&](int x, int y, int z) -> float {
        const std::size_t index =
            (static_cast<std::size_t>(z) * kCaveGridY + static_cast<std::size_t>(y)) * kHeightGrid +
            static_cast<std::size_t>(x);
        return grid[index];
    };

    const float x00 = lerp(at(ix, iy, iz), at(ix + 1, iy, iz), tx);
    const float x10 = lerp(at(ix, iy, iz + 1), at(ix + 1, iy, iz + 1), tx);
    const float x01 = lerp(at(ix, iy + 1, iz), at(ix + 1, iy + 1, iz), tx);
    const float x11 = lerp(at(ix, iy + 1, iz + 1), at(ix + 1, iy + 1, iz + 1), tx);
    return lerp(lerp(x00, x10, tz), lerp(x01, x11, tz), ty);
}

BlockId surface_block(Biome biome, bool underwater) {
    switch (biome) {
        case Biome::Desert:
        case Biome::Beach:
            return blocks::Sand;
        case Biome::Ocean:
            return blocks::Sand;
        case Biome::Snow:
            return underwater ? blocks::Gravel : blocks::Snow;
        case Biome::Forest:
        case Biome::Plains:
            return underwater ? blocks::Sand : blocks::Grass;
    }
    return blocks::Grass;
}

BlockId filler_block(Biome biome, bool underwater) {
    switch (biome) {
        case Biome::Desert:
        case Biome::Beach:
        case Biome::Ocean:
            return blocks::Sand;
        case Biome::Snow:
            return underwater ? blocks::Gravel : blocks::Dirt;
        case Biome::Forest:
        case Biome::Plains:
            return underwater ? blocks::Sand : blocks::Dirt;
    }
    return blocks::Dirt;
}

} // namespace

const char* biome_name(Biome biome) noexcept {
    switch (biome) {
        case Biome::Ocean: return "ocean";
        case Biome::Beach: return "beach";
        case Biome::Plains: return "plains";
        case Biome::Forest: return "forest";
        case Biome::Desert: return "desert";
        case Biome::Snow: return "snow";
    }
    return "unknown";
}

std::vector<NoiseSpec> WorldSpec::all_noises() const {
    return {continent_noise, hill_noise, mountain_noise,
            temperature_noise, humidity_noise, cave_noise};
}

std::uint32_t WorldSpec::hash() const {
    BinaryWriter writer(256);
    writer.u32(seed);
    writer.f32(sea_level);
    writer.f32(base_height);
    writer.f32(continent_amplitude);
    writer.f32(hill_amplitude);
    writer.f32(mountain_amplitude);
    for (const NoiseSpec& noise : all_noises()) {
        writer.u32(noise.id);
        writer.i32(noise.seed);
        writer.f32(noise.frequency);
        writer.i32(noise.octaves);
        writer.f32(noise.gain);
        writer.f32(noise.lacunarity);
        writer.u8(static_cast<std::uint8_t>(noise.kind));
        writer.u8(static_cast<std::uint8_t>(noise.fractal));
    }
    return fnv1a32(writer.data());
}

WorldSpec WorldSpec::make_default(std::uint32_t seed) {
    WorldSpec spec;
    spec.seed = seed;

    spec.continent_noise = NoiseSpec{
        .id = 1,
        .seed = static_cast<std::int32_t>(seed + 11),
        .frequency = 0.0022f,
        .octaves = 4,
        .gain = 0.5f,
        .lacunarity = 2.0f,
        .kind = NoiseKind::OpenSimplex2,
        .fractal = FractalKind::Fbm,
    };
    spec.hill_noise = NoiseSpec{
        .id = 2,
        .seed = static_cast<std::int32_t>(seed + 23),
        .frequency = 0.018f,
        .octaves = 4,
        .gain = 0.5f,
        .lacunarity = 2.0f,
        .kind = NoiseKind::OpenSimplex2,
        .fractal = FractalKind::Fbm,
    };
    spec.mountain_noise = NoiseSpec{
        .id = 3,
        .seed = static_cast<std::int32_t>(seed + 37),
        .frequency = 0.006f,
        .octaves = 5,
        .gain = 0.5f,
        .lacunarity = 2.1f,
        .kind = NoiseKind::OpenSimplex2S,
        .fractal = FractalKind::Fbm,
    };
    spec.temperature_noise = NoiseSpec{
        .id = 4,
        .seed = static_cast<std::int32_t>(seed + 41),
        .frequency = 0.0012f,
        .octaves = 3,
        .gain = 0.5f,
        .lacunarity = 2.0f,
        .kind = NoiseKind::OpenSimplex2,
        .fractal = FractalKind::Fbm,
    };
    spec.humidity_noise = NoiseSpec{
        .id = 5,
        .seed = static_cast<std::int32_t>(seed + 53),
        .frequency = 0.0016f,
        .octaves = 3,
        .gain = 0.5f,
        .lacunarity = 2.0f,
        .kind = NoiseKind::OpenSimplex2,
        .fractal = FractalKind::Fbm,
    };
    spec.cave_noise = NoiseSpec{
        .id = 6,
        .seed = static_cast<std::int32_t>(seed + 67),
        .frequency = 0.025f,
        .octaves = 3,
        .gain = 0.5f,
        .lacunarity = 2.0f,
        .kind = NoiseKind::OpenSimplex2,
        .fractal = FractalKind::Fbm,
    };
    return spec;
}

WorldGenerator::WorldGenerator(WorldSpec spec)
    : spec_(std::move(spec)), noise_bank_(spec_.all_noises()), spec_hash_(spec_.hash()) {}

float WorldGenerator::sample_height(const NoiseSampler& noise,
                                    float world_x, float world_z) const {
    const float continent = noise.sample2d(spec_.continent_noise.id, world_x, world_z);
    const float hills = noise.sample2d(spec_.hill_noise.id, world_x, world_z);
    const float mountains = noise.sample2d(spec_.mountain_noise.id, world_x, world_z);

    const float mountain_mask = smoothstep(0.12f, 0.7f, continent);
    const float mountain_value = std::max(0.0f, mountains);

    float height = spec_.base_height;
    height += continent * spec_.continent_amplitude;
    height += hills * spec_.hill_amplitude;
    height += mountain_mask * mountain_value * spec_.mountain_amplitude;
    return std::clamp(height, 2.0f, static_cast<float>(kChunkSizeY - 2));
}

Biome WorldGenerator::sample_biome(const NoiseSampler& noise, float world_x,
                                   float world_z, float height) const {
    if (height < spec_.sea_level - 1.0f) {
        return Biome::Ocean;
    }
    if (height < spec_.sea_level + 2.0f) {
        return Biome::Beach;
    }

    const float temperature = noise.sample2d(spec_.temperature_noise.id, world_x, world_z);
    const float humidity = noise.sample2d(spec_.humidity_noise.id, world_x, world_z);

    if (temperature < -0.35f) {
        return Biome::Snow;
    }
    if (humidity < -0.2f && temperature > 0.15f) {
        return Biome::Desert;
    }
    if (humidity > 0.15f) {
        return Biome::Forest;
    }
    return Biome::Plains;
}

float WorldGenerator::height_at(std::int64_t world_x, std::int64_t world_z) const {
    const NoiseSampler noise = noise_bank_.make_sampler();
    return sample_height(noise, static_cast<float>(world_x), static_cast<float>(world_z));
}

Biome WorldGenerator::biome_at(std::int64_t world_x, std::int64_t world_z) const {
    const NoiseSampler noise = noise_bank_.make_sampler();
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float height = sample_height(noise, x, z);
    return sample_biome(noise, x, z, height);
}

Chunk WorldGenerator::generate(ChunkCoord coord) const {
    const NoiseSampler noise = noise_bank_.make_sampler();
    const float origin_x = static_cast<float>(coord.x * kChunkSizeX);
    const float origin_z = static_cast<float>(coord.z * kChunkSizeZ);

    std::array<float, kHeightGrid * kHeightGrid> height_grid{};
    std::array<Biome, kHeightGrid * kHeightGrid> biome_grid{};
    std::array<float, kHeightGrid * kCaveGridY * kHeightGrid> cave_grid{};

    for (int gz = 0; gz < kHeightGrid; ++gz) {
        for (int gx = 0; gx < kHeightGrid; ++gx) {
            const float wx = origin_x + static_cast<float>(gx * kGridStep);
            const float wz = origin_z + static_cast<float>(gz * kGridStep);
            const float height = sample_height(noise, wx, wz);
            height_grid[static_cast<std::size_t>(gz) * kHeightGrid + gx] = height;
            biome_grid[static_cast<std::size_t>(gz) * kHeightGrid + gx] =
                sample_biome(noise, wx, wz, height);

            for (int gy = 0; gy < kCaveGridY; ++gy) {
                const float wy = static_cast<float>(gy * kGridStep);
                const float cave = noise.sample3d(spec_.cave_noise.id, wx, wy, wz);
                const std::size_t index =
                    (static_cast<std::size_t>(gz) * kCaveGridY +
                     static_cast<std::size_t>(gy)) * kHeightGrid +
                    static_cast<std::size_t>(gx);
                cave_grid[index] = cave;
            }
        }
    }

    std::vector<Run> runs;
    runs.reserve(kChunkColumns * 8);
    std::vector<Run> column;
    column.reserve(16);

    const int sea_level = static_cast<int>(std::lround(spec_.sea_level));

    const auto push_block = [&column](BlockId block) {
        if (!column.empty() && column.back().block == block) {
            ++column.back().length;
        } else {
            column.push_back(Run{block, 1});
        }
    };

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            const int gx = x / kGridStep;
            const int gz = z / kGridStep;
            const float tx = static_cast<float>(x % kGridStep) / kGridStep;
            const float tz = static_cast<float>(z % kGridStep) / kGridStep;

            const float height = bilerp(height_grid, gx, gz, tx, tz);
            const float h00 = height_grid[static_cast<std::size_t>(gz) * kHeightGrid + gx];
            const float h10 = height_grid[static_cast<std::size_t>(gz) * kHeightGrid + gx + 1];
            const float h01 = height_grid[static_cast<std::size_t>(gz + 1) * kHeightGrid + gx];
            const float h11 = height_grid[static_cast<std::size_t>(gz + 1) * kHeightGrid + gx + 1];
            const float min_height = std::min(std::min(h00, h10), std::min(h01, h11));
            const float max_height = std::max(std::max(h00, h10), std::max(h01, h11));

            const Biome biome = biome_grid[static_cast<std::size_t>(gz) * kHeightGrid + gx];
            const bool underwater = height < static_cast<float>(sea_level);
            const int surface_y = static_cast<int>(std::floor(height));
            const int detail_start = std::max(0, static_cast<int>(std::floor(min_height)) - 5);
            const int detail_end = std::min(kChunkSizeY - 1,
                                            std::max(static_cast<int>(std::ceil(max_height)) + 1,
                                                     sea_level + 1));

            column.clear();

            for (int y = 0; y < detail_start; ++y) {
                push_block(y == 0 ? blocks::Bedrock : blocks::Stone);
            }

            for (int y = detail_start; y <= detail_end; ++y) {
                BlockId block = blocks::Air;

                if (y == 0) {
                    block = blocks::Bedrock;
                } else if (y <= surface_y) {
                    if (y >= surface_y - 3) {
                        block = (y == surface_y) ? surface_block(biome, underwater)
                                                 : filler_block(biome, underwater);
                    } else {
                        block = blocks::Stone;
                    }

                    if (y > 2 && y < surface_y - 2) {
                        const int gy = y / kGridStep;
                        const float ty = static_cast<float>(y % kGridStep) / kGridStep;
                        const float cave = trilerp(cave_grid, gx, gy, gz, tx, ty, tz);
                        if (cave > 0.56f) {
                            block = blocks::Air;
                        }
                    }
                } else if (y <= sea_level && underwater) {
                    block = blocks::Water;
                }

                if (block == blocks::Air && y <= sea_level && underwater) {
                    block = blocks::Water;
                }
                push_block(block);
            }

            const BlockId above = (detail_end < sea_level) ? blocks::Water : blocks::Air;
            for (int y = detail_end + 1; y < kChunkSizeY; ++y) {
                push_block(above);
            }

            if (column.size() > 256) {
                // Should be impossible: one run per voxel is the worst case.
                column.resize(256);
            }

            runs.insert(runs.end(), column.begin(), column.end());
        }
    }

    return Chunk::from_runs(coord, 0, std::move(runs));
}

} // namespace vox
