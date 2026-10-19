#include "net/enet_runtime.h"

#include <enet/enet.h>

#include <mutex>

namespace vox::net {
namespace {

std::once_flag g_init_flag;
bool g_initialized = false;

} // namespace

bool enet_runtime_initialize() noexcept {
    std::call_once(g_init_flag, [] {
        g_initialized = enet_initialize() == 0;
    });
    return g_initialized;
}

} // namespace vox::net
