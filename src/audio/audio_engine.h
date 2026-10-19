#pragma once

#include <string_view>

struct ma_engine;

namespace vox {

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();

    [[nodiscard]] bool initialized() const noexcept { return engine_ != nullptr; }
    [[nodiscard]] bool play_sound(std::string_view path);
    void set_volume(float volume);

private:
    ma_engine* engine_ = nullptr;
};

} // namespace vox
