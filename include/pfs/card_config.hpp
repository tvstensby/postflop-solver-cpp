// Port of the CardConfig half of src/card.rs. Separate from card.hpp because it
// needs Range, and range.hpp needs the card primitives.
#pragma once

#include <pfs/card.hpp>
#include <pfs/common.hpp>
#include <pfs/range.hpp>

#include <array>
#include <vector>

namespace pfs {

struct CardConfig {
    std::array<Range, 2> range;  // [OOP, IP]
    // NOTE: the default is all-NOT_DEALT, not all-zero. Rust hand-writes Default
    // for exactly this reason.
    std::array<Card, 3> flop{NOT_DEALT, NOT_DEALT, NOT_DEALT};
    Card turn = NOT_DEALT;
    Card river = NOT_DEALT;

    // Hand indices that do not collide with the board: for the flop, for each of
    // the 52 turns, and for each of the 1326 (turn, river) pairs.
    struct ValidIndices {
        Indices flop;
        std::vector<Indices> turn;   // 52
        std::vector<Indices> river;  // 1326, keyed by card_pair_to_index
    };
    ValidIndices valid_indices(const PrivateCards& private_cards) const;

    // Per (turn, river) pair, each player's hands sorted ascending by strength.
    //
    // Every list is bracketed by two sentinels, {0, 0} and {u16 max, u16 max}, and
    // the unguarded sweep loops in the terminal evaluation use them as their loop
    // bounds -- dropping them turns clean loops into out-of-bounds reads. Real
    // strengths are `Hand::evaluate() + 1` so nothing can collide with the weak
    // sentinel.
    std::vector<HandStrength> hand_strength(const PrivateCards& private_cards) const;

    // Suit isomorphism: a turn or river suit is isomorphic to a lower one when the
    // board ranksets agree and both players' ranges are symmetric under the swap.
    struct Isomorphism {
        std::vector<uint8_t> ref_turn;                      // eliminated event -> representative child
        std::vector<Card> card_turn;                        // the eliminated cards
        std::array<SwapList, 4> swap_turn;                  // by suit
        std::vector<std::vector<uint8_t>> ref_river;        // [turn card][i]
        std::array<std::vector<Card>, 4> card_river;        // [turn suit]
        std::array<std::array<SwapList, 4>, 4> swap_river;  // [turn suit][river suit]
    };
    Isomorphism isomorphism(const PrivateCards& private_cards) const;
};

}  // namespace pfs
