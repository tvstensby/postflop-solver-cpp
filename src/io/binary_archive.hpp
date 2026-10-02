// Replaces bincode. Fixed-width little-endian, no varints.
//
// This is NOT wire-compatible with the Rust crate's bincode output, and the magic
// number differs deliberately so that a .bin written by the Rust solver is
// rejected cleanly rather than mis-parsed into garbage.
#pragma once

#include <pfs/common.hpp>
#include <pfs/result.hpp>

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace pfs {

class BinWriter {
public:
    void u8(uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }
    void u16(uint16_t v) { raw(&v, sizeof v); }
    void u32(uint32_t v) { raw(&v, sizeof v); }
    void u64(uint64_t v) { raw(&v, sizeof v); }
    void i32(int32_t v) { raw(&v, sizeof v); }
    void f32(float v) { raw(&v, sizeof v); }
    void f64(double v) { raw(&v, sizeof v); }
    void boolean(bool v) { u8(v ? 1 : 0); }

    void str(const std::string& s) {
        u64(s.size());
        raw(s.data(), s.size());
    }

    void blob(const void* p, size_t n) {
        u64(n);
        raw(p, n);
    }

    // Length-prefixed vector of trivially-copyable elements.
    template <class T>
    void vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        u64(v.size());
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }

    const std::vector<std::byte>& data() const noexcept { return bytes_; }
    std::vector<std::byte> take() noexcept { return std::move(bytes_); }

private:
    void raw(const void* p, size_t n) {
        const size_t at = bytes_.size();
        bytes_.resize(at + n);
        if (n) std::memcpy(bytes_.data() + at, p, n);
    }

    std::vector<std::byte> bytes_;
};

class BinReader {
public:
    BinReader(const std::byte* data, size_t size) : p_(data), end_(data + size) {}

    bool ok() const noexcept { return ok_; }

    uint8_t u8() {
        uint8_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    uint16_t u16() {
        uint16_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    int32_t i32() {
        int32_t v = 0;
        raw(&v, sizeof v);
        return v;
    }
    float f32() {
        float v = 0;
        raw(&v, sizeof v);
        return v;
    }
    double f64() {
        double v = 0;
        raw(&v, sizeof v);
        return v;
    }
    bool boolean() { return u8() != 0; }

    std::string str() {
        const uint64_t n = u64();
        std::string s;
        if (!ok_ || !have(n)) {
            ok_ = false;
            return s;
        }
        s.resize(static_cast<size_t>(n));
        raw(s.data(), static_cast<size_t>(n));
        return s;
    }

    // Reads a length-prefixed blob into a caller-provided buffer of exactly that
    // size; fails if the recorded length differs from `expect`.
    bool blob_into(void* dst, size_t expect) {
        const uint64_t n = u64();
        if (!ok_ || n != expect) {
            ok_ = false;
            return false;
        }
        raw(dst, expect);
        return ok_;
    }

    // Reads a length-prefixed blob whose recorded length may be shorter than the
    // destination (the truncated-storage case); returns the length read.
    bool blob_into_prefix(void* dst, size_t capacity, size_t& read_len) {
        const uint64_t n = u64();
        if (!ok_ || n > capacity) {
            ok_ = false;
            return false;
        }
        read_len = static_cast<size_t>(n);
        raw(dst, read_len);
        return ok_;
    }

    template <class T>
    std::vector<T> vec() {
        static_assert(std::is_trivially_copyable_v<T>);
        const uint64_t n = u64();
        std::vector<T> v;
        if (!ok_ || !have(n * sizeof(T))) {
            ok_ = false;
            return v;
        }
        v.resize(static_cast<size_t>(n));
        if (n) raw(v.data(), static_cast<size_t>(n) * sizeof(T));
        return v;
    }

private:
    bool have(uint64_t n) const noexcept {
        return static_cast<uint64_t>(end_ - p_) >= n;
    }
    void raw(void* dst, size_t n) {
        if (!ok_ || !have(n)) {
            ok_ = false;
            return;
        }
        if (n) std::memcpy(dst, p_, n);
        p_ += n;
    }

    const std::byte* p_;
    const std::byte* end_;
    bool ok_ = true;
};

}  // namespace pfs
