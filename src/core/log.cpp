#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace vox {
namespace {

std::mutex g_log_mutex;

constexpr std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

} // namespace

void log_message(LogLevel level, std::string_view message) noexcept {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto time = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    const std::lock_guard lock(g_log_mutex);
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [%s] %.*s\n",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(millis.count()),
                 level_name(level).data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

} // namespace vox
