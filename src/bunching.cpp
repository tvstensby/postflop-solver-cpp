#include <pfs/bunching.hpp>

#include "core/numeric.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdio>

namespace pfs {

// --------------------------------------------------------------------------
// Combinadics
// --------------------------------------------------------------------------

size_t mask_to_index(uint64_t mask, size_t k) noexcept {
    size_t index = 0;
    for (size_t i = 0; i < k; ++i) {
        assert(mask != 0 && "mask_to_index: fewer set bits than requested");
        const unsigned tz = static_cast<unsigned>(std::countr_zero(mask));
        index += static_cast<size_t>(kCombTable[i][tz]);
        mask &= mask - 1;  // clear the lowest set bit
    }
    return index;
}

uint64_t index_to_mask(size_t index, size_t k) noexcept {
    uint64_t mask = 0;
    for (size_t i = k; i-- > 0;) {
        const auto& rowr = kCombTable[i];
        // Rust: partition_point(|&x| x <= index) - 1, i.e. the last n with
        // row[n] <= index. The rows are non-decreasing, so upper_bound matches.
        const size_t n = static_cast<size_t>(
                             std::upper_bound(rowr.begin(), rowr.end(),
                                              static_cast<uint64_t>(index)) -
                             rowr.begin()) -
                         1;
        index -= static_cast<size_t>(rowr[n]);
        mask |= uint64_t{1} << n;
    }
    return mask;
}

uint64_t next_combination(uint64_t mask) noexcept {
    assert(mask != 0);
    const uint64_t t = mask | (mask - 1);
    return (t + 1) | (((~t & (t + 1)) - 1) >> (static_cast<unsigned>(std::countr_zero(mask)) + 1));
}

uint64_t compress_mask(uint64_t mask, std::array<Card, 3> flop) noexcept {
    assert(flop[0] < flop[1] && flop[1] < flop[2]);
    for (size_t i = 0; i < 3; ++i) {
        const uint64_t m = (uint64_t{1} << (static_cast<size_t>(flop[i]) - i)) - 1;
        mask = (mask & m) | ((mask >> 1) & ~m);
    }
    return mask;
}

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

namespace {

uint64_t flop_mask_of(std::array<Card, 3> flop) {
    return (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) | (uint64_t{1} << flop[2]);
}

// Peak resident bytes, so create() can refuse rather than throw bad_alloc.
uint64_t estimated_peak_bytes(size_t num_players) {
    uint64_t peak = 0;
    // Phase 3 results, resident for the object's lifetime.
    peak += static_cast<uint64_t>(COMB_49_4 + COMB_49_5 + COMB_49_6) * sizeof(float);
    // Phase 2 subset sums.
    uint64_t sums = static_cast<uint64_t>(1 + COMB_49_1) * sizeof(double);
    if (num_players >= 2) sums += static_cast<uint64_t>(COMB_49_2 + COMB_49_3) * sizeof(double);
    if (num_players >= 3) sums += static_cast<uint64_t>(COMB_49_4 + COMB_49_5) * sizeof(double);
    if (num_players == 4) sums += static_cast<uint64_t>(COMB_49_6) * sizeof(double);
    peak += sums;
    // The phase 1 joint table, which dominates everything for four players.
    if (num_players == 4) peak += static_cast<uint64_t>(COMB_49_8) * sizeof(double);
    return peak;
}

}  // namespace

Result<BunchingData> BunchingData::create(const std::vector<Range>& fold_ranges,
                                        std::array<Card, 3> flop) {
    std::vector<Range> kept;
    for (const Range& range : fold_ranges) {
        if (range.is_empty()) continue;  // empty ranges are dropped, not rejected
        if (!range.is_suit_symmetric())
            return Result<BunchingData>::err("Fold ranges must be suit-symmetric");
        kept.push_back(range);
    }

    if (kept.empty()) return Result<BunchingData>::err("Fold ranges is empty");
    if (kept.size() > 4)
        return Result<BunchingData>::err("The number of folded players must be at most 4");

    std::sort(flop.begin(), flop.end());
    if (flop[0] == flop[1] || flop[1] == flop[2] || flop[2] >= 52)
        return Result<BunchingData>::err("Invalid flop");

    BunchingData d;
    d.fold_ranges_ = std::move(kept);
    d.flop_ = flop;
    return Result<BunchingData>(std::move(d));
}

uint64_t BunchingData::memory_usage() const {
    uint64_t m = 0;
    m += static_cast<uint64_t>(result4_.capacity() + result5_.capacity() + result6_.capacity()) *
         sizeof(float);
    m += static_cast<uint64_t>(temp_table1_.capacity() + temp_table2_.capacity()) * sizeof(double);
    m += static_cast<uint64_t>(temp_table3_.capacity()) * sizeof(AtomicF64);
    for (const auto& s : sum_) m += static_cast<uint64_t>(s.capacity()) * sizeof(AtomicF64);
    return m;
}

float BunchingData::result_4cards(uint64_t mask) const {
    return result4_[mask_to_index(compress_mask(mask, flop_), 4)];
}
float BunchingData::result_5cards(uint64_t mask) const {
    return result5_[mask_to_index(compress_mask(mask, flop_), 5)];
}
float BunchingData::result_6cards(uint64_t mask) const {
    return result6_[mask_to_index(compress_mask(mask, flop_), 6)];
}

// --------------------------------------------------------------------------
// Phase 1: build the joint frequency table over the folded players' cards
// --------------------------------------------------------------------------

namespace {

// Walks the 1326 range slots with a flat cursor, skipping whole rows when the
// first card is on the flop -- the `src_index += 51 - card1` trick.
void phase1_compress(std::vector<double>& table, const Range& range,
                     std::array<Card, 3> flop) {
    const std::span<const float> raw = range.raw_data();
    const uint64_t flop_mask = flop_mask_of(flop);
    size_t src_index = 0;

    for (size_t card1 = 0; card1 < 52; ++card1) {
        const uint64_t mask1 = uint64_t{1} << card1;
        if ((flop_mask & mask1) != 0) {
            src_index += 51 - card1;
            continue;
        }
        for (size_t card2 = card1 + 1; card2 < 52; ++card2) {
            const double freq = static_cast<double>(raw[src_index]);
            ++src_index;
            const uint64_t mask2 = uint64_t{1} << card2;
            if ((flop_mask & mask2) != 0 || freq == 0.0) continue;
            table[mask_to_index(compress_mask(mask1 | mask2, flop), 2)] = freq;
        }
    }
}

void phase1_combine(std::vector<double>& table, const Range& range1, const Range& range2,
                    std::array<Card, 3> flop) {
    const std::span<const float> raw1 = range1.raw_data();
    const std::span<const float> raw2 = range2.raw_data();
    const uint64_t flop_mask = flop_mask_of(flop);
    size_t src_index1 = 0;

    for (size_t card11 = 0; card11 < 52; ++card11) {
        const uint64_t mask11 = uint64_t{1} << card11;
        if ((flop_mask & mask11) != 0) {
            src_index1 += 51 - card11;
            continue;
        }
        for (size_t card12 = card11 + 1; card12 < 52; ++card12) {
            const double freq1 = static_cast<double>(raw1[src_index1]);
            ++src_index1;
            const uint64_t mask12 = uint64_t{1} << card12;
            if ((flop_mask & mask12) != 0 || freq1 == 0.0) continue;

            const uint64_t mask1 = mask11 | mask12;
            size_t src_index2 = 0;

            for (size_t card21 = 0; card21 < 52; ++card21) {
                const uint64_t mask21 = uint64_t{1} << card21;
                if (((flop_mask | mask1) & mask21) != 0) {
                    src_index2 += 51 - card21;
                    continue;
                }
                const uint64_t mask2 = mask1 | mask21;
                for (size_t card22 = card21 + 1; card22 < 52; ++card22) {
                    const double freq2 = static_cast<double>(raw2[src_index2]);
                    ++src_index2;
                    const uint64_t mask22 = uint64_t{1} << card22;
                    if (((flop_mask | mask1) & mask22) != 0 || freq2 == 0.0) continue;
                    table[mask_to_index(compress_mask(mask2 | mask22, flop), 4)] += freq1 * freq2;
                }
            }
        }
    }
}

}  // namespace

void BunchingData::phase1_prepare1() {
    temp_table1_.assign(COMB_49_2, 0.0);
    phase1_compress(temp_table1_, fold_ranges_[0], flop_);
    sum_[2].assign(COMB_49_2, AtomicF64(0.0));
}

void BunchingData::phase1_prepare2() {
    temp_table1_.assign(COMB_49_2, 0.0);
    temp_table2_.assign(COMB_49_2, 0.0);
    phase1_compress(temp_table1_, fold_ranges_[0], flop_);
    phase1_compress(temp_table2_, fold_ranges_[1], flop_);
    sum_[4].assign(COMB_49_4, AtomicF64(0.0));
}

void BunchingData::phase1_prepare3() {
    temp_table1_.assign(COMB_49_4, 0.0);
    temp_table2_.assign(COMB_49_2, 0.0);
    phase1_combine(temp_table1_, fold_ranges_[0], fold_ranges_[1], flop_);
    phase1_compress(temp_table2_, fold_ranges_[2], flop_);
    sum_[6].assign(COMB_49_6, AtomicF64(0.0));
}

void BunchingData::phase1_prepare4() {
    temp_table1_.assign(COMB_49_4, 0.0);
    temp_table2_.assign(COMB_49_4, 0.0);
    phase1_combine(temp_table1_, fold_ranges_[0], fold_ranges_[1], flop_);
    phase1_combine(temp_table2_, fold_ranges_[2], fold_ranges_[3], flop_);
    temp_table3_.assign(COMB_49_8, AtomicF64(0.0));
}

void BunchingData::phase1_prepare() {
    if (phase_ != 0) PFS_PANIC("Invalid state");
    switch (fold_ranges_.size()) {
        case 1: phase1_prepare1(); break;
        case 2: phase1_prepare2(); break;
        case 3: phase1_prepare3(); break;
        default: phase1_prepare4(); break;
    }
    phase_ = 1;
    progress_percent_ = 0;
}

void BunchingData::phase1_process1() {
    if (progress_percent_ != 0) return;
    for (size_t i = 0; i < temp_table1_.size(); ++i) sum_[2][i].store(temp_table1_[i]);
}

template <size_t K>
void BunchingData::phase1_process() {
    size_t k1 = 0, k2 = 0, src_len1 = 0, src_len2 = 0;
    std::vector<AtomicF64>* dst_table = nullptr;
    if constexpr (K == 4) {
        k1 = 2; k2 = 2; src_len1 = COMB_49_2; src_len2 = COMB_49_2; dst_table = &sum_[4];
    } else if constexpr (K == 6) {
        k1 = 4; k2 = 2; src_len1 = COMB_49_4; src_len2 = COMB_49_2; dst_table = &sum_[6];
    } else {
        k1 = 4; k2 = 4; src_len1 = COMB_49_4; src_len2 = COMB_49_4; dst_table = &temp_table3_;
    }

    const size_t start = static_cast<size_t>(static_cast<double>(src_len1) *
                                            static_cast<double>(progress_percent_) / 100.0);
    const size_t end = static_cast<size_t>(static_cast<double>(src_len1) *
                                          static_cast<double>(progress_percent_ + 1) / 100.0);

    parallel_for_range(start, end, [&](size_t src_index1) {
        const double freq1 = temp_table1_[src_index1];
        if (freq1 == 0.0) return;

        const uint64_t mask1 = index_to_mask(src_index1, k1);
        uint64_t mask2 = (uint64_t{1} << k2) - 1;

        for (size_t src_index2 = 0; src_index2 < src_len2; ++src_index2) {
            if ((mask1 & mask2) == 0) {
                const double freq2 = temp_table2_[src_index2];
                if (freq2 > 0.0)
                    (*dst_table)[mask_to_index(mask1 | mask2, K)].add(freq1 * freq2);
            }
            mask2 = next_combination(mask2);
        }
    });
}

void BunchingData::phase1_proceed_by_percent() {
    if (phase_ != 1 || progress_percent_ == 100) PFS_PANIC("Invalid state");

    switch (fold_ranges_.size()) {
        case 1: phase1_process1(); break;
        case 2: phase1_process<4>(); break;
        case 3: phase1_process<6>(); break;
        default: phase1_process<8>(); break;
    }

    ++progress_percent_;
    if (progress_percent_ == 100) {
        // Explicit frees: without these the peak RSS balloons.
        temp_table1_.clear();
        temp_table1_.shrink_to_fit();
        temp_table2_.clear();
        temp_table2_.shrink_to_fit();
    }
}

// --------------------------------------------------------------------------
// Phase 2: subset sums
// --------------------------------------------------------------------------

void BunchingData::phase2_prepare() {
    if (phase_ != 1 || progress_percent_ != 100) PFS_PANIC("Invalid state");

    sum_[0].assign(1, AtomicF64(0.0));
    sum_[1].assign(COMB_49_1, AtomicF64(0.0));
    if (fold_ranges_.size() >= 2) {
        sum_[2].assign(COMB_49_2, AtomicF64(0.0));
        sum_[3].assign(COMB_49_3, AtomicF64(0.0));
    }
    if (fold_ranges_.size() >= 3) {
        sum_[4].assign(COMB_49_4, AtomicF64(0.0));
        sum_[5].assign(COMB_49_5, AtomicF64(0.0));
    }
    if (fold_ranges_.size() == 4) sum_[6].assign(COMB_49_6, AtomicF64(0.0));

    phase_ = 2;
    progress_percent_ = 0;
}

template <size_t K>
void BunchingData::phase2_process() {
    size_t src_len = 0;
    const std::vector<AtomicF64>* src_table = nullptr;
    if constexpr (K == 2) {
        src_len = COMB_49_2; src_table = &sum_[2];
    } else if constexpr (K == 4) {
        src_len = COMB_49_4; src_table = &sum_[4];
    } else if constexpr (K == 6) {
        src_len = COMB_49_6; src_table = &sum_[6];
    } else {
        src_len = COMB_49_8; src_table = &temp_table3_;
    }

    const size_t start = static_cast<size_t>(static_cast<double>(src_len) *
                                            static_cast<double>(progress_percent_) / 100.0);
    const size_t end = static_cast<size_t>(static_cast<double>(src_len) *
                                          static_cast<double>(progress_percent_ + 1) / 100.0);

    // popcount of every proper subset index; depends only on K.
    std::array<uint8_t, (size_t{1} << K) - 1> num_ones{};
    for (size_t i = 0; i + 1 < (size_t{1} << K); ++i)
        num_ones[i] = static_cast<uint8_t>(std::popcount(static_cast<uint32_t>(i)));

    // The chunking is semantic, not a grain hint: each chunk unranks once and then
    // Gosper-walks, so flattening to grain 1 would cost a full unrank per index.
    parallel_for_chunks(start, end, 100, [&](size_t chunk_start, size_t chunk_end) {
        uint64_t src_mask = index_to_mask(chunk_start, K);

        for (size_t src_index = chunk_start; src_index < chunk_end; ++src_index) {
            uint64_t src_mask_copy = src_mask;
            src_mask = next_combination(src_mask);

            const double freq = (*src_table)[src_index].load();
            if (freq == 0.0) continue;

            std::array<uint64_t, K> src_mask_bit{};
            for (size_t i = 0; i < K; ++i) {
                const uint64_t lsb = src_mask_copy & (~src_mask_copy + 1);  // wrapping_neg
                src_mask_copy ^= lsb;
                src_mask_bit[i] = lsb;
            }

            for (size_t i = 0; i + 1 < (size_t{1} << K); ++i) {
                if (num_ones[i] > 6) continue;
                uint64_t dst_mask = 0;
                for (size_t j = 0; j < K; ++j)
                    if ((i & (size_t{1} << j)) != 0) dst_mask |= src_mask_bit[j];
                sum_[num_ones[i]][mask_to_index(dst_mask, num_ones[i])].add(freq);
            }
        }
    });
}

void BunchingData::phase2_proceed_by_percent() {
    if (phase_ != 2 || progress_percent_ == 100) PFS_PANIC("Invalid state");

    switch (fold_ranges_.size()) {
        case 1: phase2_process<2>(); break;
        case 2: phase2_process<4>(); break;
        case 3: phase2_process<6>(); break;
        default: phase2_process<8>(); break;
    }

    ++progress_percent_;
    if (progress_percent_ == 100 && fold_ranges_.size() == 4) {
        temp_table3_.clear();
        temp_table3_.shrink_to_fit();
    }
}

// --------------------------------------------------------------------------
// Phase 3: inclusion-exclusion
// --------------------------------------------------------------------------

void BunchingData::phase3_prepare() {
    if (phase_ != 2 || progress_percent_ != 100) PFS_PANIC("Invalid state");
    result4_.assign(COMB_49_4, 0.0f);
    result5_.assign(COMB_49_5, 0.0f);
    result6_.assign(COMB_49_6, 0.0f);
    phase_ = 3;
    progress_percent_ = 0;
}

template <size_t N>
void BunchingData::phase3_process(size_t start_index, size_t end_index) {
    std::vector<float>* dst_table = nullptr;
    if constexpr (N == 4) dst_table = &result4_;
    else if constexpr (N == 5) dst_table = &result5_;
    else dst_table = &result6_;

    // Subsets filtered by how many cards the folded players actually hold, then
    // sorted by DESCENDING popcount. Rust uses sort_by_key, which is stable, and
    // the tie order fixes the order of the f64 add/subtract chain -- so this must
    // be stable_sort, not sort.
    std::vector<std::pair<uint8_t, uint8_t>> indices;
    indices.reserve(size_t{1} << N);
    for (size_t i = 0; i < (size_t{1} << N); ++i) {
        const uint8_t k = static_cast<uint8_t>(std::popcount(static_cast<uint32_t>(i)));
        if (k <= 2 * fold_ranges_.size())
            indices.emplace_back(static_cast<uint8_t>(i), k);
    }
    std::stable_sort(indices.begin(), indices.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

    parallel_for_chunks(start_index, end_index, 100, [&](size_t chunk_start, size_t chunk_end) {
        uint64_t mask = index_to_mask(chunk_start, N);

        for (size_t dst_index = chunk_start; dst_index < chunk_end; ++dst_index) {
            uint64_t mask_copy = mask;
            mask = next_combination(mask);

            std::array<uint64_t, N> mask_bit{};
            for (size_t i = 0; i < N; ++i) {
                const uint64_t lsb = mask_copy & (~mask_copy + 1);
                mask_copy ^= lsb;
                mask_bit[i] = lsb;
            }

            double result = 0.0;
            for (const auto& [i, k] : indices) {
                uint64_t src_mask = 0;
                for (size_t j = 0; j < N; ++j)
                    if ((i & (uint8_t{1} << j)) != 0) src_mask |= mask_bit[j];
                const size_t src_index = mask_to_index(src_mask, k);
                // Sign by parity of the subset size.
                if ((k & 1) == 0) result += sum_[k][src_index].load();
                else result -= sum_[k][src_index].load();
            }

            // Clamp negative round-off to zero. Note this is Rust's f32::max, not
            // the crate's NaN-unaware helper.
            (*dst_table)[dst_index] = std::fmax(static_cast<float>(result), 0.0f);
        }
    });
}

void BunchingData::phase3_proceed_by_percent() {
    if (phase_ != 3 || progress_percent_ == 100) PFS_PANIC("Invalid state");

    // 1% for the 4-card table, 6% for the 5-card one, 93% for the 6-card one.
    if (progress_percent_ == 0) {
        phase3_process<4>(0, COMB_49_4);
    } else if (progress_percent_ < 7) {
        const size_t s = static_cast<size_t>(static_cast<double>(COMB_49_5) *
                                            static_cast<double>(progress_percent_ - 1) / 6.0);
        const size_t e = static_cast<size_t>(static_cast<double>(COMB_49_5) *
                                            static_cast<double>(progress_percent_) / 6.0);
        phase3_process<5>(s, e);
    } else {
        const size_t s = static_cast<size_t>(static_cast<double>(COMB_49_6) *
                                            static_cast<double>(progress_percent_ - 7) / 93.0);
        const size_t e = static_cast<size_t>(static_cast<double>(COMB_49_6) *
                                            static_cast<double>(progress_percent_ - 6) / 93.0);
        phase3_process<6>(s, e);
    }

    ++progress_percent_;
    if (progress_percent_ == 100)
        for (auto& s : sum_) {
            s.clear();
            s.shrink_to_fit();
        }
}

// --------------------------------------------------------------------------

void BunchingData::process(const std::function<void(int, int)>& on_progress) {
    phase1_prepare();
    while (progress_percent_ < 100) {
        if (on_progress) on_progress(1, progress_percent_);
        phase1_proceed_by_percent();
    }
    phase2_prepare();
    while (progress_percent_ < 100) {
        if (on_progress) on_progress(2, progress_percent_);
        phase2_proceed_by_percent();
    }
    phase3_prepare();
    while (progress_percent_ < 100) {
        if (on_progress) on_progress(3, progress_percent_);
        phase3_proceed_by_percent();
    }
}

}  // namespace pfs
