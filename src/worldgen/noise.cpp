#include "worldgen/noise.h"

#include <FastNoiseLite.h>

#include <algorithm>
#include <memory>

namespace vox {
namespace {

FastNoiseLite make_noise(const NoiseSpec& spec) {
    FastNoiseLite noise;
    noise.SetSeed(spec.seed);
    noise.SetFrequency(spec.frequency);

    switch (spec.kind) {
        case NoiseKind::OpenSimplex2: noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2); break;
        case NoiseKind::OpenSimplex2S: noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S); break;
        case NoiseKind::Perlin: noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin); break;
        case NoiseKind::ValueCubic: noise.SetNoiseType(FastNoiseLite::NoiseType_ValueCubic); break;
        case NoiseKind::Value: noise.SetNoiseType(FastNoiseLite::NoiseType_Value); break;
        case NoiseKind::Cellular: noise.SetNoiseType(FastNoiseLite::NoiseType_Cellular); break;
    }

    switch (spec.fractal) {
        case FractalKind::None: noise.SetFractalType(FastNoiseLite::FractalType_None); break;
        case FractalKind::Fbm: noise.SetFractalType(FastNoiseLite::FractalType_FBm); break;
        case FractalKind::Ridged: noise.SetFractalType(FastNoiseLite::FractalType_Ridged); break;
        case FractalKind::PingPong: noise.SetFractalType(FastNoiseLite::FractalType_PingPong); break;
    }

    noise.SetFractalOctaves(spec.octaves);
    noise.SetFractalGain(spec.gain);
    noise.SetFractalLacunarity(spec.lacunarity);
    return noise;
}

} // namespace

NoiseSampler::NoiseSampler(const std::vector<NoiseSpec>& specs) {
    noises_.reserve(specs.size());
    owned_.reserve(specs.size());
    for (const NoiseSpec& spec : specs) {
        auto noise = std::make_unique<FastNoiseLite>(make_noise(spec));
        owned_.push_back(noise.get());
        noises_.emplace(spec.id, Entry{noise.release()});
    }
}

NoiseSampler::~NoiseSampler() {
    for (void* noise : owned_) {
        delete static_cast<FastNoiseLite*>(noise);
    }
}

NoiseSampler::NoiseSampler(NoiseSampler&& other) noexcept
    : noises_(std::move(other.noises_)), owned_(std::move(other.owned_)) {
    other.noises_.clear();
    other.owned_.clear();
}

NoiseSampler& NoiseSampler::operator=(NoiseSampler&& other) noexcept {
    if (this != &other) {
        for (void* noise : owned_) {
            delete static_cast<FastNoiseLite*>(noise);
        }
        noises_ = std::move(other.noises_);
        owned_ = std::move(other.owned_);
        other.noises_.clear();
        other.owned_.clear();
    }
    return *this;
}

float NoiseSampler::sample2d(std::uint32_t id, float x, float z) const {
    const auto it = noises_.find(id);
    if (it == noises_.end()) {
        return 0.0f;
    }
    const auto* noise = static_cast<const FastNoiseLite*>(it->second.noise);
    return std::clamp(noise->GetNoise(x, z), -1.0f, 1.0f);
}

float NoiseSampler::sample3d(std::uint32_t id, float x, float y, float z) const {
    const auto it = noises_.find(id);
    if (it == noises_.end()) {
        return 0.0f;
    }
    const auto* noise = static_cast<const FastNoiseLite*>(it->second.noise);
    return std::clamp(noise->GetNoise(x, y, z), -1.0f, 1.0f);
}

NoiseBank::NoiseBank(std::vector<NoiseSpec> specs) : specs_(std::move(specs)) {}

NoiseSampler NoiseBank::make_sampler() const {
    return NoiseSampler(specs_);
}

} // namespace vox
