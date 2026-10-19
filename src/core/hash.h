#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vox {

constexpr std::uint32_t fnv1a32(std::span<const std::uint8_t> data,
                                std::uint32_t seed = 2166136261u) {
    std::uint32_t hash = seed;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

constexpr std::uint64_t fnv1a64(std::span<const std::uint8_t> data,
                                std::uint64_t seed = 1469598103934665603ull) {
    std::uint64_t hash = seed;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T>
constexpr std::uint64_t hash_value(const T& value) {
    return fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(&value), sizeof(T)));
}

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

} // namespace detail

constexpr std::uint32_t crc32(std::span<const std::uint8_t> data) {
    constexpr auto table = detail::make_crc32_table();
    std::uint32_t crc = 0xffffffffu;
    for (const std::uint8_t byte : data) {
        crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

} // namespace vox
