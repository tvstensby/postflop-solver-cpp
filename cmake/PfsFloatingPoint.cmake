# Floating-point configuration.
#
# These flags are the difference between reproducing the Rust reference numbers
# and quietly drifting from them. They are attached PUBLIC, not PRIVATE, because
# solve<G>() and the whole of sliceop.hpp are templates/inline and therefore get
# compiled in the *consumer's* translation unit -- the tests instantiate the
# entire solver themselves.
#
# Why fast-math is forbidden:
#   1. is_zero(x) is `bit_cast<uint32_t>(x) == 0`, so -0.0 must NOT compare as
#      zero. Fast-math licenses the compiler to assume no signed zeros.
#   2. is_sign_positive() is signbit-based. Node locking encodes "unlocked" as
#      -1.0 and "locked to zero frequency" as +0.0, and the DCFR update branches
#      on the sign of a cumulative regret that can legitimately be -0.0.
#   3. index_to_card_pair() inverts a triangular number with sqrt/ceil in double
#      and indexes a 1326-entry table; an off-by-one is a silently wrong solve.
#   4. Every reduction in sliceop.hpp is deliberately ordered, with explicit
#      double accumulators. Reassociation changes every pinned number.
#
# Why contraction must also be off: under /arch:AVX2 MSVC will happily fuse
# `d += s1 * s2` in fma_slices_into into a single FMA instruction. Rust/LLVM
# does not do this, so the results would differ in the low bits.

# Note: MSVC has no `/fp:contract-` (it is silently ignored as an unknown option).
# `/fp:precise` is the closest switch, but it does not reliably disable
# contraction on x64 with /arch:AVX2, so the kernels additionally carry
# `#pragma fp_contract(off)` -- see src/core/sliceop.hpp. GCC defaults to
# `-ffp-contract=fast` for C++ and clang to `on`, so both need it explicitly.
function(pfs_apply_floating_point target)
  if(MSVC)
    target_compile_options(${target} PUBLIC /fp:precise)
  else()
    target_compile_options(${target} PUBLIC
      -ffp-contract=off -fno-fast-math -fno-finite-math-only)
  endif()
endfunction()

# A consumer can still append /fp:fast after us, so fail loudly at configure
# time. src/core/numeric.hpp additionally #errors on __FAST_MATH__ / _M_FP_FAST.
function(pfs_guard_floating_point)
  set(_all "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${CMAKE_CXX_FLAGS_DEBUG}")
  if(_all MATCHES "fp:fast|ffast-math|funsafe-math|freciprocal-math|Ofast")
    message(FATAL_ERROR
      "postflop-solver-cpp requires IEEE-conformant floating point, but a "
      "fast-math flag was found in CMAKE_CXX_FLAGS*. The solver depends on "
      "signed-zero semantics and on non-reassociated reductions.")
  endif()
endfunction()
