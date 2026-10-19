#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace vox {

enum class LogLevel : unsigned char {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
};

void log_message(LogLevel level, std::string_view message) noexcept;

template <typename... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    log_message(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace vox

#define VOX_LOG_TRACE(...) ::vox::log(::vox::LogLevel::Trace, __VA_ARGS__)
#define VOX_LOG_DEBUG(...) ::vox::log(::vox::LogLevel::Debug, __VA_ARGS__)
#define VOX_LOG_INFO(...) ::vox::log(::vox::LogLevel::Info, __VA_ARGS__)
#define VOX_LOG_WARN(...) ::vox::log(::vox::LogLevel::Warn, __VA_ARGS__)
#define VOX_LOG_ERROR(...) ::vox::log(::vox::LogLevel::Error, __VA_ARGS__)
