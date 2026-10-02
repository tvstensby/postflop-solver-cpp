// Port of src/bunching.rs.
//
// Given up to four folded players' ranges and the flop, this precomputes, for
// every set of 4/5/6 "dead" cards, the total weight of consistent hand assignments
// to those folded players -- the bunching effect. It uses full inclusion-exclusion
// over subsets, so a query is O(2^6) lookups and exact, with no heuristic
// manipulation of the deck distribution.
//
// The three flop cards are removed from the deck up front, so all the
// combinatorics are over 49 cards.
#pragma once

#include <pfs/card.hpp>
#include <pfs/common.hpp>
#include <pfs/range.hpp>
#include <pfs/result.hpp>

#include <pfs/atomic_float.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace pfs {

// C(49, k)
inline constexpr size_t COMB_49_1 = 49;
inline constexpr size_t COMB_49_2 = 1176;
inline constexpr size_t COMB_49_3 = 18424;
inline constexpr size_t COMB_49_4 = 211876;
inline constexpr size_t COMB_49_5 = 1906884;
inline constexpr size_t COMB_49_6 = 13983816;
inline constexpr size_t COMB_49_8 = 450978066;

// Row i holds C(n, i+1) for n in [0, 49). Generated rather than transcribed --
// unlike HAND_TABLE this is just binomials.
inline constexpr std::array<std::array<uint64_t, 49>, 8> make_comb_table() {
    std::array<std::array<uint64_t, 49>, 8> t{};
    for (size_t k = 0; k < 8; ++k)
        for (size_t n = 0; n < 49; ++n) {
            if (n < k + 1) {
                t[k][n] = 0;
            } else {
                // C(n, k+1) via Pascal, staying in integers.
                uint64_t v = 1;
                for (size_t i = 0; i < k + 1; ++i) v = v * (n - i) / (i + 1);
                t[k][n] = v;
            }
        }
    return t;
}
inline constexpr std::array<std::array<uint64_t, 49>, 8> kCombTable = make_comb_table();

// Combinadic rank of a k-bit mask.
size_t mask_to_index(uint64_t mask, size_t k) noexcept;
// The inverse.
uint64_t index_to_mask(size_t index, size_t k) noexcept;
// Gosper's hack: next mask with the same popcount.
uint64_t next_combination(uint64_t mask) noexcept;
// Deletes the three flop bit positions, mapping a 52-bit mask to a 49-bit one.
// Requires a sorted flop and a mask containing no flop bits.
uint64_t compress_mask(uint64_t mask, std::array<Card, 3> flop) noexcept;

class BunchingData {
public:
    // At most four ranges (6-max). Every range must be suit-symmetric.
    static Result<BunchingData> create(const std::vector<Range>& fold_ranges,
                                      std::array<Card, 3> flop);

    const std::vector<Range>& fold_ranges() const noexcept { return fold_ranges_; }
    std::array<Card, 3> flop() const noexcept { return flop_; }
    bool is_ready() const noexcept { return phase_ == 3 && progress_percent_ == 100; }
    uint8_t phase() const noexcept { return phase_; }
    uint8_t progress_percent() const noexcept { return progress_percent_; }
    uint64_t memory_usage() const;

    // Runs all three phases. The callback, if given, receives (phase, percent).
    void process(const std::function<void(int, int)>& on_progress = {});

    // The stepped API, so a UI can drive progress itself. Calling these out of
    // order is a programming error and throws.
    void phase1_prepare();
    void phase1_proceed_by_percent();
    void phase2_prepare();
    void phase2_proceed_by_percent();
    void phase3_prepare();
    void phase3_proceed_by_percent();

    // Internal: weight of consistent assignments given these dead cards.
    float result_4cards(uint64_t mask) const;
    float result_5cards(uint64_t mask) const;
    float result_6cards(uint64_t mask) const;

private:
    BunchingData() = default;

    void phase1_prepare1();
    void phase1_prepare2();
    void phase1_prepare3();
    void phase1_prepare4();
    void phase1_process1();
    template <size_t K>
    void phase1_process();
    template <size_t K>
    void phase2_process();
    template <size_t N>
    void phase3_process(size_t start_index, size_t end_index);

    std::vector<Range> fold_ranges_;
    std::array<Card, 3> flop_{NOT_DEALT, NOT_DEALT, NOT_DEALT};
    uint8_t phase_ = 0;
    uint8_t progress_percent_ = 0;

    // Phase 1 scratch. temp_table1/2 are written serially during preparation and
    // only read afterwards, so they stay plain.
    std::vector<double> temp_table1_;
    std::vector<double> temp_table2_;
    // temp_table3_ and sum_ ARE contended: the same destination index arises from
    // several (mask1, mask2) splits, so they accumulate through AtomicF64::add.
    // Because floating-point addition is not associative, phases 1 and 2 are
    // therefore not bitwise reproducible across runs -- same as the Rust.
    //
    // With four folded players temp_table3_ is 450,978,066 entries = 3.6 GB, which
    // is why create() runs a pre-flight memory check.
    std::vector<AtomicF64> temp_table3_;

    // Phase 2 subset sums, indexed by popcount.
    std::array<std::vector<AtomicF64>, 7> sum_;

    // Phase 3 results. Each destination is written exactly once by exactly one
    // task, so these are plain floats rather than atomics.
    std::vector<float> result4_;
    std::vector<float> result5_;
    std::vector<float> result6_;
};

}  // namespace pfs
