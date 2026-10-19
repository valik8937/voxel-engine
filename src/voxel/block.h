#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace vox {

using BlockId = std::uint16_t;

namespace blocks {
inline constexpr BlockId Air = 0;
inline constexpr BlockId Stone = 1;
inline constexpr BlockId Dirt = 2;
inline constexpr BlockId Grass = 3;
inline constexpr BlockId Sand = 4;
inline constexpr BlockId Water = 5;
inline constexpr BlockId Snow = 6;
inline constexpr BlockId Bedrock = 7;
inline constexpr BlockId Gravel = 8;
inline constexpr BlockId Clay = 9;
inline constexpr BlockId Wood = 10;
inline constexpr BlockId Leaves = 11;
inline constexpr BlockId Count = 12;
} // namespace blocks

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct BlockDefinition {
    std::string_view name = "air";
    bool solid = false;
    bool opaque = false;
    bool liquid = false;
    // Ramp from most occluded (index 0) to least occluded (index 3).
    std::array<Color, 4> ramp{};
};

[[nodiscard]] const BlockDefinition& block_definition(BlockId id) noexcept;
[[nodiscard]] bool block_is_solid(BlockId id) noexcept;
[[nodiscard]] bool block_is_opaque(BlockId id) noexcept;
[[nodiscard]] bool block_is_liquid(BlockId id) noexcept;
[[nodiscard]] std::string_view block_name(BlockId id) noexcept;

} // namespace vox
