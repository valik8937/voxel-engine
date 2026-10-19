#include "core/zstd.h"

#include <zstd.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace vox {

ZstdCompressor::ZstdCompressor(int level) : level_(level) {
    context_ = ZSTD_createCCtx();
    if (context_ != nullptr) {
        ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx*>(context_),
                               ZSTD_c_compressionLevel, level_);
        ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx*>(context_),
                               ZSTD_c_checksumFlag, 1);
    }
}

ZstdCompressor::~ZstdCompressor() {
    if (context_ != nullptr) {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
    }
}

ZstdCompressor::ZstdCompressor(ZstdCompressor&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)), level_(other.level_) {}

ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&& other) noexcept {
    if (this != &other) {
        if (context_ != nullptr) {
            ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
        }
        context_ = std::exchange(other.context_, nullptr);
        level_ = other.level_;
    }
    return *this;
}

std::optional<std::vector<std::uint8_t>> ZstdCompressor::compress(
    std::span<const std::uint8_t> input) {
    if (context_ == nullptr) {
        return std::nullopt;
    }
    if (input.empty()) {
        return std::vector<std::uint8_t>{};
    }

    auto* context = static_cast<ZSTD_CCtx*>(context_);
    ZSTD_CCtx_reset(context, ZSTD_reset_session_only);

    const std::size_t bound = ZSTD_compressBound(input.size());
    std::vector<std::uint8_t> output(bound);
    const std::size_t size = ZSTD_compress2(context, output.data(), output.size(),
                                            input.data(), input.size());
    if (ZSTD_isError(size)) {
        return std::nullopt;
    }
    output.resize(size);
    return output;
}

ZstdDecompressor::ZstdDecompressor() {
    context_ = ZSTD_createDCtx();
}

ZstdDecompressor::~ZstdDecompressor() {
    if (context_ != nullptr) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
    }
}

ZstdDecompressor::ZstdDecompressor(ZstdDecompressor&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

ZstdDecompressor& ZstdDecompressor::operator=(ZstdDecompressor&& other) noexcept {
    if (this != &other) {
        if (context_ != nullptr) {
            ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
        }
        context_ = std::exchange(other.context_, nullptr);
    }
    return *this;
}

std::optional<std::vector<std::uint8_t>> ZstdDecompressor::decompress(
    std::span<const std::uint8_t> input, std::size_t expected_size) {
    if (context_ == nullptr || expected_size > (1u << 30)) {
        return std::nullopt;
    }
    if (expected_size == 0) {
        return std::vector<std::uint8_t>{};
    }

    auto* context = static_cast<ZSTD_DCtx*>(context_);
    ZSTD_DCtx_reset(context, ZSTD_reset_session_only);

    const unsigned long long frame_size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != ZSTD_CONTENTSIZE_ERROR &&
        frame_size != expected_size) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> output(expected_size);
    const std::size_t size = ZSTD_decompressDCtx(context, output.data(), output.size(),
                                                input.data(), input.size());
    if (ZSTD_isError(size) || size != expected_size) {
        return std::nullopt;
    }
    return output;
}

} // namespace vox
