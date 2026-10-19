#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vox {

class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(std::size_t reserve_bytes) { data_.reserve(reserve_bytes); }

    void u8(std::uint8_t value) { data_.push_back(value); }
    void boolean(bool value) { u8(value ? 1 : 0); }

    void u16(std::uint16_t value) {
        data_.push_back(static_cast<std::uint8_t>(value));
        data_.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            data_.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            data_.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void varuint(std::uint64_t value) {
        while (value >= 0x80) {
            data_.push_back(static_cast<std::uint8_t>(value) | 0x80);
            value >>= 7;
        }
        data_.push_back(static_cast<std::uint8_t>(value));
    }

    void bytes(std::span<const std::uint8_t> bytes) {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    void string(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    std::vector<std::uint8_t> data_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return position_ <= data_.size() ? data_.size() - position_ : 0;
    }

    std::uint8_t u8() {
        if (!require(1)) {
            return 0;
        }
        return data_[position_++];
    }

    bool boolean() { return u8() != 0; }

    std::uint16_t u16() {
        if (!require(2)) {
            return 0;
        }
        const std::uint16_t value = static_cast<std::uint16_t>(data_[position_]) |
                                    (static_cast<std::uint16_t>(data_[position_ + 1]) << 8);
        position_ += 2;
        return value;
    }

    std::uint32_t u32() {
        if (!require(4)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(data_[position_ + i]) << (i * 8);
        }
        position_ += 4;
        return value;
    }

    std::uint64_t u64() {
        if (!require(8)) {
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(data_[position_ + i]) << (i * 8);
        }
        position_ += 8;
        return value;
    }

    std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }

    std::uint64_t varuint() {
        std::uint64_t value = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            const std::uint8_t byte = u8();
            if (!ok_) {
                return 0;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
            shift += 7;
        }
        ok_ = false;
        return 0;
    }

    std::span<const std::uint8_t> bytes(std::size_t count) {
        if (!require(count)) {
            return {};
        }
        const auto result = data_.subspan(position_, count);
        position_ += count;
        return result;
    }

    std::string string(std::size_t max_length = 4096) {
        const std::uint32_t length = u32();
        if (!ok_ || length > max_length) {
            ok_ = false;
            return {};
        }
        const auto raw = bytes(length);
        if (!ok_) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
    }

    void skip(std::size_t count) {
        if (require(count)) {
            position_ += count;
        }
    }

    void fail() noexcept { ok_ = false; }

private:
    bool require(std::size_t count) {
        if (!ok_ || position_ + count > data_.size()) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

} // namespace vox
