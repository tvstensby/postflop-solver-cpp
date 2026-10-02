#include "harness.hpp"

#include <pfs/card_config.hpp>

#include <vector>

using namespace pfs;

namespace {

PrivateCards enumerate(const CardConfig& cfg, std::array<std::vector<float>, 2>& weights) {
    uint64_t board_mask = 0;
    for (Card c : cfg.flop) board_mask |= uint64_t{1} << c;
    if (cfg.turn != NOT_DEALT) board_mask |= uint64_t{1} << cfg.turn;
    if (cfg.river != NOT_DEALT) board_mask |= uint64_t{1} << cfg.river;

    PrivateCards out;
    for (size_t p = 0; p < 2; ++p) cfg.range[p].get_hands_weights(board_mask, out[p], weights[p]);
    return out;
}

CardConfig make(const char* oop, const char* ip, const char* flop, const char* turn = nullptr,
                const char* river = nullptr) {
    CardConfig c;
    c.range[0] = Range::parse(oop).value();
    c.range[1] = Range::parse(ip).value();
    c.flop = flop_from_str(flop).value();
    if (turn) c.turn = card_from_str(turn).value();
    if (river) c.river = card_from_str(river).value();
    return c;
}

}  // namespace

PFS_TEST(card_config, private_hand_enumeration_excludes_board) {
    // "AA" on a board containing two aces leaves only one ace combination.
    CardConfig cfg = make("AA", "KK", "AcAdKh");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);

    CHECK_EQ(pc[0].size(), 1u);  // AhAs
    CHECK_EQ(hole_to_string(pc[0][0]).value(), std::string("AsAh"));
    CHECK_EQ(pc[1].size(), 3u);  // KK minus the king on the board
    // Hands are (low, high) ordered and lexicographically sorted.
    for (const Hole& h : pc[0]) CHECK(h.first < h.second);
    for (size_t i = 1; i < pc[1].size(); ++i) CHECK(pc[1][i - 1] < pc[1][i]);
}

PFS_TEST(card_config, hand_strength_is_sorted_and_sentinel_bracketed) {
    CardConfig cfg = make("AA,KK,72o", "QQ,JJ,54s", "Td9d6h");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const std::vector<HandStrength> hs = cfg.hand_strength(pc);

    CHECK_EQ(hs.size(), kNumHandIndices);

    // Pick a (turn, river) pair that does not collide with the flop.
    const Card turn = card_from_str("2c").value();
    const Card river = card_from_str("3c").value();
    const HandStrength& s = hs[card_pair_to_index(turn, river)];

    for (size_t p = 0; p < 2; ++p) {
        CHECK(s[p].size() >= 2u);
        // The two sentinels bracket the list after sorting.
        CHECK_EQ(s[p].front().strength, 0);
        CHECK_EQ(s[p].front().index, 0);
        CHECK_EQ(s[p].back().strength, UINT16_MAX);
        CHECK_EQ(s[p].back().index, UINT16_MAX);
        // Ascending by (strength, index).
        for (size_t i = 1; i < s[p].size(); ++i) CHECK(!(s[p][i] < s[p][i - 1]));
        // Every real strength is >= 1, so nothing collides with the weak sentinel.
        for (size_t i = 1; i + 1 < s[p].size(); ++i) CHECK(s[p][i].strength >= 1);
    }

    // A board pair colliding with the flop is left empty.
    const Card flop_card = cfg.flop[0];
    const Card other = card_from_str("2d").value();
    CHECK(hs[card_pair_to_index(flop_card, other)][0].empty());
}

PFS_TEST(card_config, hand_strength_ranks_correctly) {
    // AA vs KK on a blank board: aces must outrank kings.
    CardConfig cfg = make("AA", "KK", "2c6dTh");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const std::vector<HandStrength> hs = cfg.hand_strength(pc);

    const Card turn = card_from_str("3d").value();
    const Card river = card_from_str("4h").value();
    const HandStrength& s = hs[card_pair_to_index(turn, river)];

    // Strip the sentinels; each range has 6 combos.
    CHECK_EQ(s[0].size(), 8u);
    CHECK_EQ(s[1].size(), 8u);
    const uint16_t weakest_aces = s[0][1].strength;
    const uint16_t strongest_kings = s[1][s[1].size() - 2].strength;
    CHECK(weakest_aces > strongest_kings);
}

PFS_TEST(card_config, monotone_flop_has_isomorphic_turn_suits) {
    // A monotone flop with suit-symmetric ranges: the three non-board suits are
    // interchangeable, so two of the three should be folded away.
    CardConfig cfg = make("AKs", "AKs", "2c3c4c");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::Isomorphism iso = cfg.isomorphism(pc);

    // 49 turn cards remain; the eliminated ones are those in the two isomorphic suits.
    CHECK(!iso.ref_turn.empty());
    CHECK_EQ(iso.ref_turn.size(), iso.card_turn.size());
    // Each eliminated card refers to a representative child index.
    for (size_t i = 0; i < iso.ref_turn.size(); ++i) CHECK(iso.ref_turn[i] < 49);

    // Two of the four suits are isomorphic to a lower one, and each contributes a
    // non-empty swap list for both players.
    size_t suits_with_swaps = 0;
    for (uint8_t s = 0; s < 4; ++s)
        if (!iso.swap_turn[s][0].empty()) {
            ++suits_with_swaps;
            CHECK(!iso.swap_turn[s][1].empty());
            // Swaps are transpositions with i < j, listed once each.
            for (const Swap& p : iso.swap_turn[s][0]) CHECK(p.first < p.second);
        }
    CHECK_EQ(suits_with_swaps, 2u);
}

PFS_TEST(card_config, rainbow_flop_has_no_turn_isomorphism) {
    // A rainbow flop with three distinct ranks: no two suits share a rankset.
    CardConfig cfg = make("AA", "KK", "2c6dTh");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::Isomorphism iso = cfg.isomorphism(pc);

    // The spade suit is empty on the flop, as is... none of the others, so only
    // suits with equal ranksets can pair up. Here 2c/6d/Th all differ.
    for (uint8_t s = 0; s < 4; ++s) CHECK(iso.swap_turn[s][0].empty());
    CHECK(iso.ref_turn.empty());
}

PFS_TEST(card_config, fixed_turn_suppresses_turn_isomorphism) {
    // With the turn already dealt there are no turn events to merge, but river
    // isomorphism is still computed.
    CardConfig cfg = make("AKs", "AKs", "2c3c4c", "5c");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::Isomorphism iso = cfg.isomorphism(pc);

    CHECK(iso.ref_turn.empty());
    CHECK(iso.card_turn.empty());
    // The dealt turn is the only one with river data.
    const Card turn = card_from_str("5c").value();
    CHECK(!iso.ref_river[turn].empty());
    for (Card t = 0; t < 52; ++t)
        if (t != turn) CHECK(iso.ref_river[t].empty());
}

PFS_TEST(card_config, fixed_river_suppresses_river_isomorphism) {
    CardConfig cfg = make("AKs", "AKs", "2c3c4c", "5c", "6h");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::Isomorphism iso = cfg.isomorphism(pc);

    for (Card t = 0; t < 52; ++t) CHECK(iso.ref_river[t].empty());
}

PFS_TEST(card_config, valid_indices_track_the_board) {
    CardConfig cfg = make("AA,KK", "QQ,JJ", "Td9d6h");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::ValidIndices vi = cfg.valid_indices(pc);

    // Turn undealt, so the flop list is every hand.
    CHECK_EQ(vi.flop[0].size(), pc[0].size());
    CHECK_EQ(vi.flop[1].size(), pc[1].size());

    CHECK_EQ(vi.turn.size(), 52u);
    CHECK_EQ(vi.river.size(), kNumHandIndices);

    // Dealing the ace of clubs must remove every AA combo containing it.
    const Card ac = card_from_str("Ac").value();
    CHECK(vi.turn[ac].size() != 0u);
    CHECK(vi.turn[ac][0].size() < pc[0].size());

    // A flop card can never be a turn card.
    CHECK(vi.turn[cfg.flop[0]][0].empty());
    CHECK(vi.turn[cfg.flop[0]][1].empty());
}

PFS_TEST(card_config, suit_specific_range_breaks_isomorphism) {
    // A range that is not suit-symmetric must defeat the suit isomorphism even on
    // a monotone flop.
    CardConfig cfg = make("AsKs", "AKs", "2c3c4c");
    std::array<std::vector<float>, 2> weights;
    const PrivateCards pc = enumerate(cfg, weights);
    const CardConfig::Isomorphism iso = cfg.isomorphism(pc);

    // The OOP range distinguishes spades, so fewer suits can be merged than in the
    // symmetric case.
    size_t suits_with_swaps = 0;
    for (uint8_t s = 0; s < 4; ++s)
        if (!iso.swap_turn[s][0].empty()) ++suits_with_swaps;
    CHECK(suits_with_swaps < 2u);
}
