#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vox {

class ZstdCompressor {
public:
    explicit ZstdCompressor(int level = 3);
    ~ZstdCompressor();

    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;
    ZstdCompressor(ZstdCompressor&&) noexcept;
    ZstdCompressor& operator=(ZstdCompressor&&) noexcept;

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> compress(
        std::span<const std::uint8_t> input);

    [[nodiscard]] int level() const noexcept { return level_; }

private:
    void* context_ = nullptr;
    int level_ = 3;
};

class ZstdDecompressor {
public:
    ZstdDecompressor();
    ~ZstdDecompressor();

    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;
    ZstdDecompressor(ZstdDecompressor&&) noexcept;
    ZstdDecompressor& operator=(ZstdDecompressor&&) noexcept;

    // expected_size must be the decompressed size. It is validated against the
    // frame header when the frame stores its content size.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> decompress(
        std::span<const std::uint8_t> input, std::size_t expected_size);

private:
    void* context_ = nullptr;
};

} // namespace vox
