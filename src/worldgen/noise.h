#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vox {

enum class NoiseKind : std::uint8_t {
    OpenSimplex2,
    OpenSimplex2S,
    Perlin,
    ValueCubic,
    Value,
    Cellular,
};

enum class FractalKind : std::uint8_t {
    None,
    Fbm,
    Ridged,
    PingPong,
};

struct NoiseSpec {
    std::uint32_t id = 0;
    std::int32_t seed = 0;
    float frequency = 0.01f;
    int octaves = 4;
    float gain = 0.5f;
    float lacunarity = 2.0f;
    NoiseKind kind = NoiseKind::OpenSimplex2;
    FractalKind fractal = FractalKind::Fbm;
};

class NoiseSampler {
public:
    NoiseSampler() = default;
    explicit NoiseSampler(const std::vector<NoiseSpec>& specs);
    ~NoiseSampler();

    NoiseSampler(const NoiseSampler&) = delete;
    NoiseSampler& operator=(const NoiseSampler&) = delete;
    NoiseSampler(NoiseSampler&& other) noexcept;
    NoiseSampler& operator=(NoiseSampler&& other) noexcept;

    [[nodiscard]] float sample2d(std::uint32_t id, float x, float z) const;
    [[nodiscard]] float sample3d(std::uint32_t id, float x, float y, float z) const;

private:
    struct Entry {
        void* noise = nullptr;
    };

    std::unordered_map<std::uint32_t, Entry> noises_;
    std::vector<void*> owned_;
};

class NoiseBank {
public:
    NoiseBank() = default;
    explicit NoiseBank(std::vector<NoiseSpec> specs);

    [[nodiscard]] NoiseSampler make_sampler() const;
    [[nodiscard]] const std::vector<NoiseSpec>& specs() const noexcept { return specs_; }

private:
    std::vector<NoiseSpec> specs_;
};

} // namespace vox
