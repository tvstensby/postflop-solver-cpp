// Port of src/alloc.rs -- a thread-local LIFO bump arena for the solver's
// per-node temporaries (cfv_actions, cfreach_updated, result_f64, strategy,
// denom). In Rust this is the `custom-alloc` feature, optional only because
// `Allocator` needs nightly.
//
// Two deliberate departures from the Rust:
//   1. Rust enforces LIFO by panicking in `deallocate` if the cursor does not
//      land back on the pointer being freed. Here `Arena::Scope` restores the
//      saved cursor in its destructor, so the discipline is structural and
//      cannot be violated by reordering locals.
//   2. Rust's allocator FAILS for any request over the 1 MB chunk size. Here an
//      oversized request gets its own right-sized chunk, so no caller has to
//      reason about the limit.
#pragma once

#include <pfs/common.hpp>

#include <cstdlib>
#include <new>
#include <span>
#include <vector>

namespace pfs {

class Arena {
public:
    // 64 rather than Rust's 16: the slice kernels want AVX2-aligned starts.
    static constexpr size_t kAlignment = 64;
    static constexpr size_t kChunkSize = size_t{1} << 20;  // 1 MB, as in Rust

    Arena() = default;
    ~Arena() { release(); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    static Arena& local() {
        static thread_local Arena arena;
        return arena;
    }

    // Frees every chunk. Called on each pool thread via ThreadPool::broadcast at
    // the end of finalize(), mirroring rayon::broadcast(free_custom_alloc_buffer).
    void release();

    // RAII bump marker. All allocations made through a Scope are reclaimed when
    // it goes out of scope, in reverse order automatically.
    class Scope {
    public:
        Scope() noexcept : Scope(Arena::local()) {}
        explicit Scope(Arena& a) noexcept
            : arena_(a), chunk_(a.chunk_index_), cursor_(a.cursor_) {}
        ~Scope() {
            arena_.chunk_index_ = chunk_;
            arena_.cursor_ = cursor_;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        // Uninitialized storage, exactly like Rust's `spare_capacity_mut()`.
        // Callers must write every element before reading, which is what the
        // *_into slice kernels guarantee.
        std::span<float> floats(size_t n) { return {arena_.alloc<float>(n), n}; }
        std::span<double> doubles(size_t n) { return {arena_.alloc<double>(n), n}; }

        // Zero-filled variants, for the few places that accumulate in place.
        std::span<float> zeroed_floats(size_t n) {
            std::span<float> s = floats(n);
            for (size_t i = 0; i < n; ++i) s[i] = 0.0f;
            return s;
        }

    private:
        Arena& arena_;
        size_t chunk_;
        size_t cursor_;
    };

private:
    template <class T>
    T* alloc(size_t n) {
        static_assert(alignof(T) <= kAlignment);
        return static_cast<T*>(allocate_bytes(n * sizeof(T)));
    }

    void* allocate_bytes(size_t bytes);
    void grow(size_t min_bytes);

    struct Chunk {
        std::byte* base = nullptr;
        size_t size = 0;
    };

    static size_t align_up(size_t n) noexcept {
        return (n + (kAlignment - 1)) & ~(kAlignment - 1);
    }

    std::vector<Chunk> chunks_;
    // Index of the chunk currently being filled; kNoChunk when none exists yet.
    static constexpr size_t kNoChunk = static_cast<size_t>(-1);
    size_t chunk_index_ = kNoChunk;
    size_t cursor_ = 0;  // byte offset into chunks_[chunk_index_]
};

// When PFS_CUSTOM_ALLOC is off, ScratchScope hands out plain heap vectors so the
// reference numbers can be produced without the arena in the picture at all.
// Both variants are default-constructible, so call sites read the same either
// way: `ScratchScope scratch;`.
#if PFS_CUSTOM_ALLOC

using ScratchScope = Arena::Scope;

#else

class ScratchScope {
public:
    std::span<float> floats(size_t n) {
        buffers_f_.emplace_back(n);
        return {buffers_f_.back().data(), n};
    }
    std::span<double> doubles(size_t n) {
        buffers_d_.emplace_back(n);
        return {buffers_d_.back().data(), n};
    }
    std::span<float> zeroed_floats(size_t n) {
        buffers_f_.emplace_back(n, 0.0f);
        return {buffers_f_.back().data(), n};
    }

private:
    // Moving the outer vector transfers each inner vector's heap buffer, so
    // pointers handed out earlier stay valid across later requests.
    std::vector<std::vector<float>> buffers_f_;
    std::vector<std::vector<double>> buffers_d_;
};

#endif

}  // namespace pfs
