// Scalar float helpers. These mirror `utility.rs` exactly; the semantics are
// load-bearing and deliberately differ from the obvious C++ spellings.
#pragma once

#include <pfs/common.hpp>

#include <bit>
#include <cmath>
#include <limits>

// The port depends on IEEE-754 semantics that fast-math explicitly discards:
// signed zero (is_zero / is_sign_positive below), and non-reassociated
// reductions in sliceop.hpp. A consumer can still append /fp:fast after our
// flags, so guard here too.
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "postflop-solver-cpp requires IEEE-conformant floating point; remove /fp:fast, -ffast-math or -funsafe-math-optimizations."
#endif

static_assert(std::numeric_limits<float>::is_iec559, "IEEE-754 float required");
static_assert(std::numeric_limits<double>::is_iec559, "IEEE-754 double required");
static_assert(std::endian::native == std::endian::little, "little-endian required");
// On 32-bit x86 the x87 stack gives excess precision to float multiplication,
// which breaks the `(x * y) as f64` accumulation in inner_product.
static_assert(sizeof(void*) == 8, "64-bit target required");

namespace pfs {

// Rust: `fn max(x: f32, y: f32) -> f32 { if x > y { x } else { y } }`.
// NOT std::fmax: fmax(NaN, y) == y, whereas this returns y only because the
// comparison is false, and max(x, NaN) returns NaN. Keep the raw branch.
PFS_ALWAYS_INLINE float fmax_raw(float x, float y) noexcept { return x > y ? x : y; }

// Rust: `fn is_zero(x: f32) -> bool { x.to_bits() == 0 }`.
// True for +0.0 only. is_zero(-0.0f) is FALSE, unlike `x == 0.0f`.
PFS_ALWAYS_INLINE bool is_zero(float x) noexcept { return std::bit_cast<uint32_t>(x) == 0; }

// Rust: `f32::is_sign_positive()` -- signbit based, so -0.0 is NOT positive.
// Used with the -1.0 "unlocked" sentinel in the node-locking strategy, and to
// pick the DCFR alpha/beta coefficient. Never spell this as `x >= 0.0f`.
PFS_ALWAYS_INLINE bool is_sign_positive(float x) noexcept { return !std::signbit(x); }

// Rust: `fn min(x: f64, y: f64) -> f64 { if x < y { x } else { y } }`
// (game/evaluation.rs, used for the rake cap).
PFS_ALWAYS_INLINE double fmin_raw(double x, double y) noexcept { return x < y ? x : y; }

}  // namespace pfs
