// The 16-bit quantization helpers and small strategy utilities from utility.rs.
// The exact arithmetic here is observable through the compressed-mode tests, so
// every cast and magic constant is transcribed literally.
#pragma once

#include "arena.hpp"
#include "numeric.hpp"
#include "sliceop.hpp"

#include <cmath>
#include <span>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace pfs {

// 8-wide manual unroll, matching the Rust. The `< 16` scalar path seeds with
// +0.0f, so an all-negative-zero slice yields +0.0 and hence scale == 0.0.
inline float slice_absolute_max(std::span<const float> s) noexcept {
    const size_t n = s.size();
    if (n < 16) {
        float a = 0.0f;
        for (size_t i = 0; i < n; ++i) a = fmax_raw(a, std::fabs(s[i]));
        return a;
    }
    float tmp[8];
    for (size_t j = 0; j < 8; ++j) tmp[j] = std::fabs(s[j]);
    size_t i = 8;
    for (; i + 8 <= n; i += 8)
        for (size_t j = 0; j < 8; ++j) tmp[j] = fmax_raw(tmp[j], std::fabs(s[i + j]));
    float a = 0.0f;
    for (size_t j = 0; j < 8; ++j) a = fmax_raw(a, tmp[j]);
    for (; i < n; ++i) a = fmax_raw(a, std::fabs(s[i]));
    return a;
}

inline float slice_nonnegative_max(std::span<const float> s) noexcept {
    const size_t n = s.size();
    if (n < 16) {
        float a = 0.0f;
        for (size_t i = 0; i < n; ++i) a = fmax_raw(a, s[i]);
        return a;
    }
    float tmp[8];
    for (size_t j = 0; j < 8; ++j) tmp[j] = s[j];
    size_t i = 8;
    for (; i + 8 <= n; i += 8)
        for (size_t j = 0; j < 8; ++j) tmp[j] = fmax_raw(tmp[j], s[i + j]);
    float a = 0.0f;
    for (size_t j = 0; j < 8; ++j) a = fmax_raw(a, tmp[j]);
    for (; i < n; ++i) a = fmax_raw(a, s[i]);
    return a;
}

// Rust: `(s * encoder).round().to_int_unchecked::<i32>() as i16`.
// f32::round is half-AWAY-from-zero, which is std::round -- not std::rint or
// std::nearbyint (half-to-even). Returns the scale, which the caller stores on
// the node; note that an all-zero slice returns 0.0 even though the encoder used
// 1.0 as the divisor.
inline float encode_signed_slice(std::span<int16_t> dst, std::span<const float> src) noexcept {
    const float scale = slice_absolute_max(src);
    const float scale_nonzero = scale == 0.0f ? 1.0f : scale;
    const float encoder = 32767.0f / scale_nonzero;
    const size_t n = dst.size();
    for (size_t i = 0; i < n; ++i)
        dst[i] = static_cast<int16_t>(static_cast<int32_t>(std::round(src[i] * encoder)));
    return scale;
}

// Rust: `(s * encoder + 0.49999997).to_int_unchecked::<i32>() as u16` -- note
// there is NO round() here, it is multiply, add the magic constant, TRUNCATE.
// The constant is chosen so that 0.49999997 + 0.49999997 = 0.99999994 < 1.0
// while 0.5 + 0.49999997 = 1.0. It must stay an `f` literal: written as a double
// it would promote the expression and change the rounding. The multiply and the
// add must also not fuse into an FMA, hence the fp_contract pragma above.
inline float encode_unsigned_slice(std::span<uint16_t> dst, std::span<const float> src) noexcept {
    const float scale = slice_nonnegative_max(src);
    const float scale_nonzero = scale == 0.0f ? 1.0f : scale;
    const float encoder = 65535.0f / scale_nonzero;
    const size_t n = dst.size();
    for (size_t i = 0; i < n; ++i)
        dst[i] = static_cast<uint16_t>(static_cast<int32_t>(src[i] * encoder + 0.49999997f));
    return scale;
}

inline void decode_signed_slice(std::span<float> dst, std::span<const int16_t> src,
                                float scale) noexcept {
    const float decoder = scale / 32767.0f;
    const size_t n = dst.size();
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<float>(src[i]) * decoder;
}

// Swaps are involutions, which is why the callers apply them, accumulate, then
// apply again to restore the buffer. Instantiated for float and for size_t.
template <class T>
inline void apply_swap(std::span<T> s, std::span<const Swap> swap_list) noexcept {
    for (const Swap& p : swap_list) {
        T tmp = s[p.first];
        s[p.first] = s[p.second];
        s[p.second] = tmp;
    }
}

// Overwrites dst only where the locking entry is sign-positive. "Not locked" is
// encoded as -1.0, and "locked to zero frequency" as +0.0 -- which is exactly why
// this must test the sign bit rather than `>= 0.0f`.
inline void apply_locking_strategy(std::span<float> dst, std::span<const float> locking) noexcept {
    if (locking.empty()) return;
    const size_t n = dst.size();
    for (size_t i = 0; i < n && i < locking.size(); ++i)
        if (is_sign_positive(locking[i])) dst[i] = locking[i];
}

// Sums the action rows, then divides each row by the total. When a hand's total
// is exactly +0.0 the uniform 1/num_actions is substituted -- that fallback is
// what makes an unsolved tree produce the clean closed-form EVs the tests pin.
template <class T>
inline std::span<float> normalized_strategy(ScratchScope& scratch, std::span<const T> strategy,
                                            size_t num_actions) {
    const size_t total = strategy.size();
    std::span<float> out = scratch.floats(total);
    for (size_t i = 0; i < total; ++i) out[i] = static_cast<float>(strategy[i]);

    const size_t row_size = total / num_actions;
    {
        // denom is scratch that must die before `out` is returned, so give it its
        // own scope.
        ScratchScope inner;
        std::span<float> denom = inner.floats(row_size);
        sum_slices_into(denom, std::span<const float>(out));
        const float dflt = 1.0f / static_cast<float>(num_actions);
        for (size_t a = 0; a < num_actions; ++a)
            div_slice(row_mut(out, a, row_size), std::span<const float>(denom), dflt);
    }
    return out;
}

// Regret matching: clamp negatives to zero, then normalize as above.
template <class T>
inline std::span<float> regret_matching(ScratchScope& scratch, std::span<const T> regret,
                                        size_t num_actions) {
    const size_t total = regret.size();
    std::span<float> out = scratch.floats(total);
    if constexpr (std::is_same_v<T, float>) {
        for (size_t i = 0; i < total; ++i) out[i] = fmax_raw(regret[i], 0.0f);
    } else {
        // Rust: `r.max(0) as f32` on the integer, before the conversion.
        for (size_t i = 0; i < total; ++i)
            out[i] = static_cast<float>(regret[i] > 0 ? regret[i] : 0);
    }

    const size_t row_size = total / num_actions;
    {
        ScratchScope inner;
        std::span<float> denom = inner.floats(row_size);
        sum_slices_into(denom, std::span<const float>(out));
        const float dflt = 1.0f / static_cast<float>(num_actions);
        for (size_t a = 0; a < num_actions; ++a)
            div_slice(row_mut(out, a, row_size), std::span<const float>(denom), dflt);
    }
    return out;
}

// Sequential f64 fold; cold, but must stay sequential for parity.
inline float weighted_sum(std::span<const float> values, std::span<const float> weights) noexcept {
    double sum = 0.0;
    const size_t n = values.size();
    for (size_t i = 0; i < n; ++i)
        sum += static_cast<double>(values[i]) * static_cast<double>(weights[i]);
    return static_cast<float>(sum);
}

}  // namespace pfs
