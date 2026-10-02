// Port of src/sliceop.rs.
//
// The `*_slices_*` family treats `src` as a row-major matrix whose row width is
// `dst.size()`: row 0 is written into `dst`, then the remaining rows are folded
// into it. `dst.size()` is the implicit num_hands.
//
// Rust passes `&mut [MaybeUninit<f32>]` for the destinations and requires every
// element to be written. Here that is just a `std::span<float>` over
// uninitialized arena memory, with the same requirement.
#pragma once

#include "numeric.hpp"

#include <cstddef>
#include <span>

// Rust/LLVM does not fuse `a * b + c` into an FMA unless the `contract`
// fast-math flag is set, and rustc does not set it. MSVC under /arch:AVX2 will
// happily fuse the `dst[j] += src1[] * src2[]` accumulations below, which would
// change the intermediate rounding and therefore every value the ported tests
// pin. `/fp:precise` alone is not a documented guarantee against this, so say it
// explicitly. GCC/Clang get -ffp-contract=off from CMake.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace pfs {

inline void sub_slice(std::span<float> lhs, std::span<const float> rhs) noexcept {
    const size_t n = lhs.size();
    for (size_t i = 0; i < n; ++i) lhs[i] -= rhs[i];
}

inline void mul_slice(std::span<float> lhs, std::span<const float> rhs) noexcept {
    const size_t n = lhs.size();
    for (size_t i = 0; i < n; ++i) lhs[i] *= rhs[i];
}

inline void div_slice(std::span<float> lhs, std::span<const float> rhs, float dflt) noexcept {
    const size_t n = lhs.size();
    for (size_t i = 0; i < n; ++i) lhs[i] = is_zero(rhs[i]) ? dflt : lhs[i] / rhs[i];
}

inline void div_slice_into(std::span<float> dst, std::span<const float> lhs,
                           std::span<const float> rhs, float dflt) noexcept {
    const size_t n = dst.size();
    for (size_t i = 0; i < n; ++i) dst[i] = is_zero(rhs[i]) ? dflt : lhs[i] / rhs[i];
}

inline void mul_slice_scalar_into(std::span<float> dst, std::span<const float> src,
                                  float scalar) noexcept {
    const size_t n = dst.size();
    for (size_t i = 0; i < n; ++i) dst[i] = src[i] * scalar;
}

// dst[j] = sum over rows r of src[r * len + j]
inline void sum_slices_into(std::span<float> dst, std::span<const float> src) noexcept {
    const size_t len = dst.size();
    if (len == 0) return;
    for (size_t j = 0; j < len; ++j) dst[j] = src[j];
    for (size_t base = len; base + len <= src.size(); base += len)
        for (size_t j = 0; j < len; ++j) dst[j] += src[base + j];
}

// Same, accumulating in double. Used at chance nodes.
inline void sum_slices_f64_into(std::span<double> dst, std::span<const float> src) noexcept {
    const size_t len = dst.size();
    if (len == 0) return;
    for (size_t j = 0; j < len; ++j) dst[j] = static_cast<double>(src[j]);
    for (size_t base = len; base + len <= src.size(); base += len)
        for (size_t j = 0; j < len; ++j) dst[j] += static_cast<double>(src[base + j]);
}

// dst[j] = sum over rows r of src1[r*len + j] * src2[r*len + j]
inline void fma_slices_into(std::span<float> dst, std::span<const float> src1,
                            std::span<const float> src2) noexcept {
    const size_t len = dst.size();
    if (len == 0) return;
    for (size_t j = 0; j < len; ++j) dst[j] = src1[j] * src2[j];
    for (size_t base = len; base + len <= src1.size() && base + len <= src2.size(); base += len)
        for (size_t j = 0; j < len; ++j) dst[j] += src1[base + j] * src2[base + j];
}

// Element-wise max across rows -- the best-response reduction.
inline void max_slices_into(std::span<float> dst, std::span<const float> src) noexcept {
    const size_t len = dst.size();
    if (len == 0) return;
    for (size_t j = 0; j < len; ++j) dst[j] = src[j];
    for (size_t base = len; base + len <= src.size(); base += len)
        for (size_t j = 0; j < len; ++j) dst[j] = fmax_raw(dst[j], src[base + j]);
}

// Locked best response: where src2 is sign-positive the action is locked and
// mixed in by probability; elsewhere we maximize.
inline void max_fma_slices_into(std::span<float> dst, std::span<const float> src1,
                                std::span<const float> src2) noexcept {
    const size_t len = dst.size();
    if (len == 0) return;
    for (size_t j = 0; j < len; ++j)
        dst[j] = is_sign_positive(src2[j]) ? src1[j] * src2[j] : src1[j];
    for (size_t base = len; base + len <= src1.size() && base + len <= src2.size(); base += len) {
        for (size_t j = 0; j < len; ++j) {
            const float s1 = src1[base + j];
            const float s2 = src2[base + j];
            if (is_sign_positive(s2)) dst[j] += s1 * s2;
            else dst[j] = fmax_raw(dst[j], s1);
        }
    }
}

// 8-wide double accumulator, mirroring the Rust version. The manual unroll is
// what makes this vectorize without fast-math.
inline float inner_product(std::span<const float> src1, std::span<const float> src2) noexcept {
    constexpr size_t kChunk = 8;
    const size_t len = src1.size();
    const size_t len_chunk = len / kChunk * kChunk;
    double acc[kChunk] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (size_t i = 0; i < len_chunk; i += kChunk)
        for (size_t j = 0; j < kChunk; ++j)
            acc[j] += static_cast<double>(src1[i + j] * src2[i + j]);

    for (size_t i = len_chunk; i < len; ++i)
        acc[0] += static_cast<double>(src1[i] * src2[i]);

    double sum = 0.0;
    for (size_t j = 0; j < kChunk; ++j) sum += acc[j];
    return static_cast<float>(sum);
}

// As above, but each term is scaled by `less`/`greater`/`equal` depending on how
// cond[i] compares to `threshold`. The Rust source notes that an if/else-if
// chain is used instead of `match` because `match` prevents vectorization.
inline float inner_product_cond(std::span<const float> src1, std::span<const float> src2,
                                std::span<const uint16_t> cond, uint16_t threshold, float less,
                                float greater, float equal) noexcept {
    constexpr size_t kChunk = 8;
    const size_t len = src1.size();
    const size_t len_chunk = len / kChunk * kChunk;
    double acc[kChunk] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (size_t i = 0; i < len_chunk; i += kChunk) {
        for (size_t j = 0; j < kChunk; ++j) {
            const uint16_t c = cond[i + j];
            const float z = c < threshold ? less : (c > threshold ? greater : equal);
            acc[j] += static_cast<double>(src1[i + j] * src2[i + j] * z);
        }
    }

    for (size_t i = len_chunk; i < len; ++i) {
        const uint16_t c = cond[i];
        const float z = c < threshold ? less : (c > threshold ? greater : equal);
        acc[0] += static_cast<double>(src1[i] * src2[i] * z);
    }

    double sum = 0.0;
    for (size_t j = 0; j < kChunk; ++j) sum += acc[j];
    return static_cast<float>(sum);
}

// The only 2D indexing abstraction; everything else is a flat row-major buffer
// of num_actions * num_hands.
template <class T>
PFS_ALWAYS_INLINE std::span<const T> row(std::span<const T> s, size_t index,
                                         size_t row_size) noexcept {
    return s.subspan(index * row_size, row_size);
}

template <class T>
PFS_ALWAYS_INLINE std::span<T> row_mut(std::span<T> s, size_t index, size_t row_size) noexcept {
    return s.subspan(index * row_size, row_size);
}

}  // namespace pfs
