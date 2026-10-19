#pragma once

namespace vox::net {

// ENet must be initialized exactly once per process. The runtime is never torn
// down explicitly; the OS reclaims it at exit.
[[nodiscard]] bool enet_runtime_initialize() noexcept;

} // namespace vox::net
