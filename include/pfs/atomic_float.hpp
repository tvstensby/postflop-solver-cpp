// Port of src/atomic_float.rs. Bit-punned atomics, relaxed ordering throughout.
// Used only by the bunching computation, never by the CFR solver.
#pragma once

#include <pfs/common.hpp>

#include <atomic>
#include <bit>

namespace pfs {

class AtomicF32 {
public:
    AtomicF32() noexcept : bits_(0) {}
    explicit AtomicF32(float v) noexcept : bits_(std::bit_cast<uint32_t>(v)) {}

    // Needed because std::atomic is not copyable but the containers holding
    // these are resized/moved wholesale between phases.
    AtomicF32(const AtomicF32& o) noexcept : bits_(o.bits_.load(std::memory_order_relaxed)) {}
    AtomicF32& operator=(const AtomicF32& o) noexcept {
        bits_.store(o.bits_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    float load() const noexcept {
        return std::bit_cast<float>(bits_.load(std::memory_order_relaxed));
    }
    void store(float v) noexcept {
        bits_.store(std::bit_cast<uint32_t>(v), std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> bits_;
};

class AtomicF64 {
public:
    AtomicF64() noexcept : bits_(0) {}
    explicit AtomicF64(double v) noexcept : bits_(std::bit_cast<uint64_t>(v)) {}

    AtomicF64(const AtomicF64& o) noexcept : bits_(o.bits_.load(std::memory_order_relaxed)) {}
    AtomicF64& operator=(const AtomicF64& o) noexcept {
        bits_.store(o.bits_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    double load() const noexcept {
        return std::bit_cast<double>(bits_.load(std::memory_order_relaxed));
    }
    void store(double v) noexcept {
        bits_.store(std::bit_cast<uint64_t>(v), std::memory_order_relaxed);
    }

    // Rust uses `fetch_update`, i.e. a CAS loop. Because floating-point addition
    // is not associative, the result depends on interleaving: bunching results
    // are not bitwise reproducible across runs. That matches the original.
    void add(double v) noexcept {
        uint64_t cur = bits_.load(std::memory_order_relaxed);
        for (;;) {
            const uint64_t next = std::bit_cast<uint64_t>(std::bit_cast<double>(cur) + v);
            if (bits_.compare_exchange_weak(cur, next, std::memory_order_relaxed,
                                            std::memory_order_relaxed))
                return;
        }
    }

private:
    std::atomic<uint64_t> bits_;
};

}  // namespace pfs
