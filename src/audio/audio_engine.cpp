#include "audio/audio_engine.h"

#include "core/log.h"

#include <miniaudio.h>

#include <string>

namespace vox {

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    if (engine_ != nullptr) {
        return true;
    }

    engine_ = new ma_engine{};
    const ma_result result = ma_engine_init(nullptr, engine_);
    if (result != MA_SUCCESS) {
        VOX_LOG_WARN("miniaudio initialization failed: {}", ma_result_description(result));
        delete engine_;
        engine_ = nullptr;
        return false;
    }
    return true;
}

void AudioEngine::shutdown() {
    if (engine_ != nullptr) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
}

bool AudioEngine::play_sound(std::string_view path) {
    if (engine_ == nullptr || path.empty()) {
        return false;
    }
    const std::string path_string(path);
    const ma_result result = ma_engine_play_sound(engine_, path_string.c_str(), nullptr);
    if (result != MA_SUCCESS) {
        VOX_LOG_WARN("Cannot play '{}': {}", path_string, ma_result_description(result));
        return false;
    }
    return true;
}

void AudioEngine::set_volume(float volume) {
    if (engine_ != nullptr) {
        ma_engine_set_volume(engine_, volume);
    }
}

} // namespace vox
