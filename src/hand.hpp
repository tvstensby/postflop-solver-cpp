// Port of src/hand.rs -- the 7-card evaluator. Internal (Rust: pub(crate)).
//
// Cards are NOT bit-packed: Hand is a plain value type holding 7 slots, with a
// functional add_card that returns a copy, exactly as in Rust.
#pragma once

#include "hand_table.hpp"

#include <pfs/common.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>

namespace pfs {

// Rust: keep_n_msb. `1 << (x.leading_zeros() ^ 31)` is the MSB of x.
// PRECONDITION: x must have at least n bits set. With x == 0 Rust computes
// `1i32 << 63`, which is nonsense either way -- callers guarantee it.
PFS_ALWAYS_INLINE int32_t keep_n_msb(int32_t x, int n) noexcept {
    int32_t ret = 0;
    for (int i = 0; i < n; ++i) {
        assert(x != 0 && "keep_n_msb: fewer set bits than requested");
        const int32_t bit = int32_t{1} << (31 - std::countl_zero(static_cast<uint32_t>(x)));
        x ^= bit;
        ret |= bit;
    }
    return ret;
}

PFS_ALWAYS_INLINE int32_t find_straight(int32_t rankset) noexcept {
    constexpr int32_t kWheel = 0b1'0000'0000'1111;  // A5432
    const int32_t is_straight =
        rankset & (rankset << 1) & (rankset << 2) & (rankset << 3) & (rankset << 4);
    if (is_straight != 0) return keep_n_msb(is_straight, 1);
    if ((rankset & kWheel) == kWheel) return 1 << 3;
    return 0;
}

class Hand {
public:
    Hand() = default;

    PFS_ALWAYS_INLINE Hand add_card(size_t card) const noexcept {
        Hand h = *this;
        h.cards_[h.num_cards_] = card;
        ++h.num_cards_;
        return h;
    }

    PFS_ALWAYS_INLINE bool contains(size_t card) const noexcept {
        for (size_t i = 0; i < num_cards_; ++i)
            if (cards_[i] == card) return true;
        return false;
    }

    size_t num_cards() const noexcept { return num_cards_; }

    // Dense rank in [0, 4824). Only meaningful on a full 7-card hand: like the
    // Rust, evaluate_internal reads all 7 slots regardless of num_cards, and
    // empty slots read as card 0 (2c).
    PFS_ALWAYS_INLINE uint16_t evaluate() const noexcept {
        const int32_t v = evaluate_internal();
        const auto it = std::lower_bound(kHandTable.begin(), kHandTable.end(), v);
        assert(it != kHandTable.end() && *it == v && "hand value not in table");
        return static_cast<uint16_t>(it - kHandTable.begin());
    }

    // The packed 32-bit value: [category:5][primary ranks:13][secondary ranks:13].
    int32_t evaluate_internal() const noexcept {
        int32_t rankset = 0;
        int32_t rankset_suit[4] = {0, 0, 0, 0};
        int32_t rankset_of_count[5] = {0, 0, 0, 0, 0};
        int32_t rank_count[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

        // Rust iterates `&self.cards`, i.e. all 7 slots, ignoring num_cards.
        for (size_t i = 0; i < kMaxCards; ++i) {
            const size_t card = cards_[i];
            const size_t rank = card / 4;
            const size_t suit = card % 4;
            rankset |= int32_t{1} << rank;
            rankset_suit[suit] |= int32_t{1} << rank;
            rank_count[rank] += 1;
        }

        for (size_t rank = 0; rank < 13; ++rank)
            rankset_of_count[static_cast<size_t>(rank_count[rank])] |= int32_t{1} << rank;

        int32_t flush_suit = -1;
        for (int32_t suit = 0; suit < 4; ++suit)
            if (std::popcount(static_cast<uint32_t>(rankset_suit[suit])) >= 5) flush_suit = suit;

        const int32_t is_straight = find_straight(rankset);

        if (flush_suit >= 0) {
            const int32_t suited = rankset_suit[flush_suit];
            const int32_t is_straight_flush = find_straight(suited);
            if (is_straight_flush != 0) return (8 << 26) | is_straight_flush;  // straight flush
            return (5 << 26) | keep_n_msb(suited, 5);                          // flush
        }
        if (rankset_of_count[4] != 0) {  // four of a kind
            const int32_t remaining = keep_n_msb(rankset ^ rankset_of_count[4], 1);
            return (7 << 26) | (rankset_of_count[4] << 13) | remaining;
        }
        if (std::popcount(static_cast<uint32_t>(rankset_of_count[3])) == 2) {  // two sets of trips
            const int32_t trips = keep_n_msb(rankset_of_count[3], 1);
            const int32_t pair = rankset_of_count[3] ^ trips;
            return (6 << 26) | (trips << 13) | pair;
        }
        if (rankset_of_count[3] != 0 && rankset_of_count[2] != 0) {  // full house
            const int32_t pair = keep_n_msb(rankset_of_count[2], 1);
            return (6 << 26) | (rankset_of_count[3] << 13) | pair;
        }
        if (is_straight != 0) return (4 << 26) | is_straight;
        if (rankset_of_count[3] != 0) {  // three of a kind
            const int32_t remaining = keep_n_msb(rankset_of_count[1], 2);
            return (3 << 26) | (rankset_of_count[3] << 13) | remaining;
        }
        if (std::popcount(static_cast<uint32_t>(rankset_of_count[2])) >= 2) {  // two pair
            const int32_t pairs = keep_n_msb(rankset_of_count[2], 2);
            const int32_t remaining = keep_n_msb(rankset ^ pairs, 1);
            return (2 << 26) | (pairs << 13) | remaining;
        }
        if (rankset_of_count[2] != 0) {  // one pair
            const int32_t remaining = keep_n_msb(rankset_of_count[1], 3);
            return (1 << 26) | (rankset_of_count[2] << 13) | remaining;
        }
        return keep_n_msb(rankset, 5);  // high card
    }

private:
    static constexpr size_t kMaxCards = 7;
    std::array<size_t, kMaxCards> cards_{};
    size_t num_cards_ = 0;
};

}  // namespace pfs
