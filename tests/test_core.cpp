#include "harness.hpp"

#include "core/arena.hpp"
#include <pfs/atomic_float.hpp>
#include "core/numeric.hpp"
#include "core/sliceop.hpp"
#include "core/thread_pool.hpp"

#include <atomic>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace pfs;

// --------------------------------------------------------------------------
// The semantic landmines. These are the checks that would catch an
// "obvious" C++ rewrite of the Rust helpers.
// --------------------------------------------------------------------------

PFS_TEST(numeric, is_zero_rejects_negative_zero) {
    // Rust: is_zero(x) == (x.to_bits() == 0). -0.0 has bit pattern 0x80000000.
    CHECK(is_zero(0.0f));
    CHECK(!is_zero(-0.0f));
    CHECK(!is_zero(1e-45f));  // subnormal is not zero
    // The naive spelling disagrees, which is the entire point.
    CHECK((-0.0f == 0.0f));
}

PFS_TEST(numeric, is_sign_positive_rejects_negative_zero) {
    CHECK(is_sign_positive(0.0f));
    CHECK(!is_sign_positive(-0.0f));
    CHECK(is_sign_positive(1.0f));
    CHECK(!is_sign_positive(-1.0f));
    // Node locking encodes "unlocked" as -1.0 and "locked to zero" as +0.0, so
    // `x >= 0.0f` would treat an unlocked -0.0 entry as locked.
    CHECK((-0.0f >= 0.0f));
}

PFS_TEST(numeric, fmax_raw_matches_rust_branch) {
    CHECK_EQ(fmax_raw(1.0f, 2.0f), 2.0f);
    CHECK_EQ(fmax_raw(2.0f, 1.0f), 2.0f);
    // Rust: `if x > y { x } else { y }`. Any comparison with NaN is false, so the
    // second argument wins when either side is NaN. std::fmax would differ.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(fmax_raw(1.0f, nan)));
    CHECK_EQ(fmax_raw(nan, 1.0f), 1.0f);
}

PFS_TEST(sliceop, div_slice_uses_bitwise_zero_test) {
    // A -0.0 denominator must divide (giving -inf), not select the default.
    std::vector<float> lhs{1.0f, 1.0f, 1.0f};
    const std::vector<float> rhs{0.0f, -0.0f, 2.0f};
    div_slice(lhs, rhs, 0.25f);
    CHECK_EQ(lhs[0], 0.25f);           // +0.0 -> default
    CHECK(std::isinf(lhs[1]));         // -0.0 -> divide
    CHECK(lhs[1] < 0.0f);
    CHECK_EQ(lhs[2], 0.5f);
}

// --------------------------------------------------------------------------
// Slice kernels. `src` is a row-major matrix of dst.size()-wide rows.
// --------------------------------------------------------------------------

PFS_TEST(sliceop, sum_and_fma_fold_rows) {
    const std::vector<float> src{1, 2, 3, 10, 20, 30, 100, 200, 300};
    std::vector<float> dst(3);
    sum_slices_into(dst, src);
    CHECK_EQ(dst[0], 111.0f);
    CHECK_EQ(dst[1], 222.0f);
    CHECK_EQ(dst[2], 333.0f);

    std::vector<double> dst64(3);
    sum_slices_f64_into(dst64, src);
    CHECK_NEAR(dst64[2], 333.0, 1e-12);

    const std::vector<float> w{1, 1, 1, 2, 2, 2, 0, 0, 0};
    fma_slices_into(dst, src, w);
    CHECK_EQ(dst[0], 1.0f + 20.0f);
    CHECK_EQ(dst[1], 2.0f + 40.0f);
    CHECK_EQ(dst[2], 3.0f + 60.0f);
}

PFS_TEST(sliceop, max_slices_and_locked_best_response) {
    const std::vector<float> src{1, 9, 3, 7, 2, 8};
    std::vector<float> dst(3);
    max_slices_into(dst, src);
    CHECK_EQ(dst[0], 7.0f);
    CHECK_EQ(dst[1], 9.0f);
    CHECK_EQ(dst[2], 8.0f);

    // src2 sign-positive -> mix by probability; otherwise maximize.
    const std::vector<float> v{10, 10, 10, 20, 20, 20};
    const std::vector<float> p{0.5f, -1.0f, 0.0f, 0.5f, -1.0f, 1.0f};
    max_fma_slices_into(dst, v, p);
    CHECK_EQ(dst[0], 5.0f + 10.0f);            // both locked: 10*0.5 + 20*0.5
    CHECK_EQ(dst[1], 20.0f);                   // both unlocked: max(10, 20)
    CHECK_EQ(dst[2], 0.0f + 20.0f * 1.0f);     // locked at 0.0 then 1.0
}

PFS_TEST(sliceop, inner_product_handles_tail) {
    // 11 elements exercises the 8-wide body plus the 3-element tail.
    std::vector<float> a(11), b(11);
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<float>(i + 1);
        b[i] = 2.0f;
    }
    CHECK_NEAR(inner_product(a, b), 2.0f * (11 * 12 / 2), 1e-4);

    const std::vector<uint16_t> cond{0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1};
    // threshold 1: cond<1 -> less(=1), cond>1 -> greater(=100), ==1 -> equal(=0)
    const float got = inner_product_cond(a, b, cond, 1, 1.0f, 100.0f, 0.0f);
    float want = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        const float z = cond[i] < 1 ? 1.0f : (cond[i] > 1 ? 100.0f : 0.0f);
        want += a[i] * b[i] * z;
    }
    CHECK_NEAR(got, want, 1e-3);
}

// --------------------------------------------------------------------------
// Atomic floats
// --------------------------------------------------------------------------

PFS_TEST(atomic_float, round_trip_and_add) {
    AtomicF32 a(1.5f);
    CHECK_EQ(a.load(), 1.5f);
    a.store(-2.25f);
    CHECK_EQ(a.load(), -2.25f);

    AtomicF64 d(0.0);
    for (int i = 0; i < 1000; ++i) d.add(0.5);
    CHECK_NEAR(d.load(), 500.0, 1e-9);
}

PFS_TEST(atomic_float, concurrent_add_is_lossless) {
    AtomicF64 d(0.0);
    parallel_for_range(0, 4096, [&](size_t) { d.add(1.0); });
    // Integers this small are exact, so the CAS loop must lose nothing.
    CHECK_NEAR(d.load(), 4096.0, 0.0000001);
}

// --------------------------------------------------------------------------
// Thread pool. The nested case is the one that matters: the CFR recursion
// parallelizes over a flop node's children, and each turn node does so again.
// --------------------------------------------------------------------------

PFS_TEST(thread_pool, parallel_for_visits_every_index_once) {
    constexpr size_t kN = 5000;
    std::vector<std::atomic<int>> hits(kN);
    for (auto& h : hits) h.store(0);
    parallel_for_range(0, kN, [&](size_t i) { hits[i].fetch_add(1); });
    for (size_t i = 0; i < kN; ++i) CHECK_EQ(hits[i].load(), 1);
}

PFS_TEST(thread_pool, nested_parallel_for_does_not_deadlock) {
    // Without a participating join this hangs as soon as every worker is parked
    // inside the outer loop.
    constexpr size_t kOuter = 64, kInner = 64;
    std::atomic<size_t> total{0};
    parallel_for_range(0, kOuter, [&](size_t) {
        parallel_for_range(0, kInner, [&](size_t) {
            // A third level, for good measure.
            parallel_for_range(0, 4, [&](size_t) { total.fetch_add(1); });
        });
    });
    CHECK_EQ(total.load(), kOuter * kInner * 4);
}

PFS_TEST(thread_pool, exception_propagates_out_of_parallel_for) {
    CHECK_THROWS(parallel_for_range(0, 256, [](size_t i) {
        if (i == 200) throw std::runtime_error("boom");
    }));
}

PFS_TEST(thread_pool, broadcast_reaches_every_thread) {
    auto& pool = ThreadPool::global();
    std::atomic<size_t> count{0};
    pool.broadcast([&] { count.fetch_add(1); });
    CHECK_EQ(count.load(), pool.num_threads());
}

// --------------------------------------------------------------------------
// Arena
// --------------------------------------------------------------------------

PFS_TEST(arena, scope_reclaims_in_lifo_order) {
    Arena arena;
    float* first = nullptr;
    {
        Arena::Scope outer(arena);
        std::span<float> a = outer.floats(16);
        first = a.data();
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(i);
        {
            Arena::Scope inner(arena);
            std::span<float> b = inner.floats(16);
            CHECK(b.data() != first);
            // The outer allocation must be untouched by the inner scope.
            CHECK_EQ(a[3], 3.0f);
        }
        // After the inner scope exits its bytes are reusable.
        Arena::Scope again(arena);
        std::span<float> c = again.floats(16);
        CHECK(c.data() != first);
    }
    // Back at the top: the next allocation reuses the very first address.
    Arena::Scope fresh(arena);
    CHECK_EQ(fresh.floats(16).data(), first);
}

PFS_TEST(arena, oversized_request_gets_its_own_chunk) {
    // Rust's allocator fails above 1 MB; ours must simply serve it.
    Arena arena;
    Arena::Scope scope(arena);
    const size_t n = (Arena::kChunkSize / sizeof(float)) * 3;
    std::span<float> big = scope.floats(n);
    CHECK_EQ(big.size(), n);
    big[0] = 1.0f;
    big[n - 1] = 2.0f;
    CHECK_EQ(big[0], 1.0f);
    CHECK_EQ(big[n - 1], 2.0f);
}

PFS_TEST(arena, allocations_are_aligned) {
    Arena arena;
    Arena::Scope scope(arena);
    for (size_t n : {1u, 3u, 7u, 33u, 1000u}) {
        auto p = reinterpret_cast<uintptr_t>(scope.floats(n).data());
        CHECK_EQ(p % Arena::kAlignment, 0u);
    }
}

PFS_TEST(arena, scratch_scope_hands_out_stable_buffers) {
    ScratchScope scratch;
    std::span<float> a = scratch.floats(8);
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(i);
    // Requesting more must not invalidate what we already hold.
    for (int k = 0; k < 32; ++k) (void)scratch.floats(64);
    CHECK_EQ(a[5], 5.0f);

    std::span<float> z = scratch.zeroed_floats(4);
    CHECK_EQ(z[0], 0.0f);
    CHECK_EQ(z[3], 0.0f);
}

PFS_TEST(arena, per_thread_arenas_are_independent) {
    std::atomic<size_t> ok{0};
    parallel_for_range(0, 64, [&](size_t i) {
        Arena::Scope scope(Arena::local());
        std::span<float> s = scope.floats(256);
        const float v = static_cast<float>(i);
        for (auto& x : s) x = v;
        bool good = true;
        for (float x : s) good = good && (x == v);
        if (good) ok.fetch_add(1);
    });
    CHECK_EQ(ok.load(), 64u);
    ThreadPool::global().broadcast([] { Arena::local().release(); });
}
