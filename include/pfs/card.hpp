// Port of src/card.rs (plus the card<->string helpers, which the Rust keeps in
// range.rs but which only depend on cards).
#pragma once

#include <pfs/common.hpp>
#include <pfs/result.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pfs {

// card_id = 4 * rank + suit, 0 <= card_id < 52.
//   rank: 2 => 0, 3 => 1, ..., A => 12      (so rank = card >> 2)
//   suit: c => 0, d => 1, h => 2, s => 3    (so suit = card & 3)
// A bare alias, exactly as in Rust -- there is no strong typing to preserve.
using Card = uint8_t;

inline constexpr Card NOT_DEALT = 0xFF;

// A (low_id, high_id) hole-card pair.
using Hole = std::pair<Card, Card>;

// Rust: `pub(crate) struct StrengthItem { strength: u16, index: u16 }` with a
// DERIVED Ord, i.e. lexicographic by (strength, index). Field order is
// load-bearing, so spell the comparison out.
struct StrengthItem {
    uint16_t strength;
    uint16_t index;

    friend bool operator<(const StrengthItem& a, const StrengthItem& b) noexcept {
        if (a.strength != b.strength) return a.strength < b.strength;
        return a.index < b.index;
    }
    friend bool operator==(const StrengthItem& a, const StrengthItem& b) noexcept {
        return a.strength == b.strength && a.index == b.index;
    }
};

// Rust: `pub(crate) type SwapList = [Vec<(u16, u16)>; 2];`
using Swap = std::pair<uint16_t, uint16_t>;
using SwapList = std::array<std::vector<Swap>, 2>;

// --------------------------------------------------------------------------
// The 1326-hand pair index. 2d2c => 0 ... AsAh => 1325.
// --------------------------------------------------------------------------

PFS_ALWAYS_INLINE size_t card_pair_to_index(Card card1, Card card2) noexcept {
    if (card1 > card2) {
        const Card t = card1;
        card1 = card2;
        card2 = t;
    }
    return static_cast<size_t>(card1) * (101 - static_cast<size_t>(card1)) / 2 +
           static_cast<size_t>(card2) - 1;
}

// The inverse. Rust does this with a double sqrt/ceil rather than a table, and
// the closed form is exact only with an IEEE sqrt on an unfused, unreassociated
// expression -- an off-by-one here is a silently wrong board, not a crash. Do
// not "simplify" the algebra, and never compute it in float.
Hole index_to_card_pair(size_t index) noexcept;

// Per-player lists, used by CardConfig (see card_config.hpp) and PostFlopGame.
using PrivateCards = std::array<std::vector<Hole>, 2>;
using Indices = std::array<std::vector<uint16_t>, 2>;
using HandStrength = std::array<std::vector<StrengthItem>, 2>;

// --------------------------------------------------------------------------
// Card <-> string
// --------------------------------------------------------------------------

Result<std::string> card_to_string(Card card);
Result<std::string> hole_to_string(Hole hole);
Result<std::vector<std::string>> holes_to_strings(const std::vector<Hole>& holes);
Result<Card> card_from_str(const std::string& s);
Result<std::array<Card, 3>> flop_from_str(const std::string& s);

}  // namespace pfs
