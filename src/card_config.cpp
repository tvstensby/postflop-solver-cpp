#include <pfs/card_config.hpp>

#include "hand.hpp"

#include <algorithm>

namespace pfs {

namespace {

bool flop_contains(const std::array<Card, 3>& flop, Card c) {
    return flop[0] == c || flop[1] == c || flop[2] == c;
}

Indices valid_indices_internal(const PrivateCards& private_cards, Card board1, Card board2) {
    Indices ret;

    uint64_t board_mask = 0;
    if (board1 != NOT_DEALT) board_mask |= uint64_t{1} << board1;
    if (board2 != NOT_DEALT) board_mask |= uint64_t{1} << board2;

    for (size_t player = 0; player < 2; ++player) {
        const std::vector<Hole>& cards = private_cards[player];
        ret[player].reserve(cards.size());
        for (size_t i = 0; i < cards.size(); ++i) {
            const uint64_t hand_mask =
                (uint64_t{1} << cards[i].first) | (uint64_t{1} << cards[i].second);
            if ((hand_mask & board_mask) == 0) ret[player].push_back(static_cast<uint16_t>(i));
        }
        ret[player].shrink_to_fit();
    }
    return ret;
}

}  // namespace

CardConfig::ValidIndices CardConfig::valid_indices(const PrivateCards& private_cards) const {
    ValidIndices out;

    if (turn == NOT_DEALT) {
        for (size_t player = 0; player < 2; ++player) {
            out.flop[player].resize(private_cards[player].size());
            for (size_t i = 0; i < private_cards[player].size(); ++i)
                out.flop[player][i] = static_cast<uint16_t>(i);
        }
    }

    out.turn.assign(52, Indices{});
    for (Card board = 0; board < 52; ++board)
        if (!flop_contains(flop, board) && (turn == NOT_DEALT || turn == board) &&
            river == NOT_DEALT)
            out.turn[board] = valid_indices_internal(private_cards, board, NOT_DEALT);

    out.river.assign(kNumHandIndices, Indices{});
    for (Card b1 = 0; b1 < 52; ++b1)
        for (Card b2 = static_cast<Card>(b1 + 1); b2 < 52; ++b2)
            if (!flop_contains(flop, b1) && !flop_contains(flop, b2) &&
                (turn == NOT_DEALT || b1 == turn || b2 == turn) &&
                (river == NOT_DEALT || b1 == river || b2 == river))
                out.river[card_pair_to_index(b1, b2)] =
                    valid_indices_internal(private_cards, b1, b2);

    return out;
}

std::vector<HandStrength> CardConfig::hand_strength(const PrivateCards& private_cards) const {
    std::vector<HandStrength> ret(kNumHandIndices);

    Hand flop_board;
    for (Card c : flop) flop_board = flop_board.add_card(c);

    for (Card b1 = 0; b1 < 52; ++b1) {
        for (Card b2 = static_cast<Card>(b1 + 1); b2 < 52; ++b2) {
            if (flop_board.contains(b1) || flop_board.contains(b2)) continue;
            if (!(turn == NOT_DEALT || b1 == turn || b2 == turn)) continue;
            if (!(river == NOT_DEALT || b1 == river || b2 == river)) continue;

            const Hand board = flop_board.add_card(b1).add_card(b2);
            HandStrength strength;

            for (size_t player = 0; player < 2; ++player) {
                std::vector<StrengthItem>& s = strength[player];
                s.reserve(private_cards[player].size() + 2);
                // The two sentinels go in first; the sort then puts them at the ends.
                s.push_back(StrengthItem{0, 0});
                s.push_back(StrengthItem{UINT16_MAX, UINT16_MAX});

                for (size_t i = 0; i < private_cards[player].size(); ++i) {
                    const size_t c1 = private_cards[player][i].first;
                    const size_t c2 = private_cards[player][i].second;
                    if (board.contains(c1) || board.contains(c2)) continue;
                    const Hand hand = board.add_card(c1).add_card(c2);
                    s.push_back(StrengthItem{static_cast<uint16_t>(hand.evaluate() + 1),
                                             static_cast<uint16_t>(i)});
                }

                s.shrink_to_fit();
                std::sort(s.begin(), s.end());
            }

            ret[card_pair_to_index(b1, b2)] = std::move(strength);
        }
    }

    return ret;
}

namespace {

// Builds the transposition list that maps each hand index to the index of the
// same hand with suit1 and suit2 exchanged. Only pairs with i < index are pushed,
// so each transposition appears once and the list is its own inverse.
void isomorphism_swap_internal(std::array<SwapList, 4>& swap_lists,
                               std::vector<size_t>& reverse_table, uint8_t suit1, uint8_t suit2,
                               const PrivateCards& private_cards) {
    SwapList& swap_list = swap_lists[suit1];

    auto replacer = [suit1, suit2](Card card) -> Card {
        if ((card & 3) == suit1) return static_cast<Card>(card - suit1 + suit2);
        if ((card & 3) == suit2) return static_cast<Card>(card + suit1 - suit2);
        return card;
    };

    for (size_t player = 0; player < 2; ++player) {
        if (!swap_list[player].empty()) continue;

        std::fill(reverse_table.begin(), reverse_table.end(), static_cast<size_t>(-1));
        const std::vector<Hole>& cards = private_cards[player];
        for (size_t i = 0; i < cards.size(); ++i)
            reverse_table[card_pair_to_index(cards[i].first, cards[i].second)] = i;

        for (size_t i = 0; i < cards.size(); ++i) {
            const Card c1 = replacer(cards[i].first);
            const Card c2 = replacer(cards[i].second);
            const size_t index = reverse_table[card_pair_to_index(c1, c2)];
            if (i < index)
                swap_list[player].emplace_back(static_cast<uint16_t>(i),
                                               static_cast<uint16_t>(index));
        }
    }
}

// For every dealable card, either record it as a fresh event (assigning it the
// next child index) or, if its suit is isomorphic to a lower one, record which
// existing child it refers to.
void isomorphism_internal(std::vector<uint8_t>& isomorphism_ref,
                          std::vector<Card>& isomorphism_card, uint64_t mask,
                          const std::array<int8_t, 4>& isomorphic_suit) {
    const bool push_card = isomorphism_card.empty();
    uint8_t counter = 0;
    std::array<uint8_t, 52> indices{};

    for (Card card = 0; card < 52; ++card) {
        if (((uint64_t{1} << card) & mask) != 0) continue;
        const uint8_t suit = static_cast<uint8_t>(card & 3);

        if (isomorphic_suit[suit] >= 0) {
            const Card replace_card =
                static_cast<Card>(card - suit + static_cast<uint8_t>(isomorphic_suit[suit]));
            isomorphism_ref.push_back(indices[replace_card]);
            if (push_card) isomorphism_card.push_back(card);
        } else {
            indices[card] = counter;
            ++counter;
        }
    }
}

}  // namespace

CardConfig::Isomorphism CardConfig::isomorphism(const PrivateCards& private_cards) const {
    Isomorphism out;

    // Which suits are interchangeable as far as both ranges are concerned.
    std::array<uint8_t, 4> suit_isomorphism{0, 0, 0, 0};
    uint8_t next_index = 1;
    for (uint8_t suit2 = 1; suit2 < 4; ++suit2) {
        bool matched = false;
        for (uint8_t suit1 = 0; suit1 < suit2; ++suit1) {
            if (range[0].is_suit_isomorphic(suit1, suit2) &&
                range[1].is_suit_isomorphic(suit1, suit2)) {
                suit_isomorphism[suit2] = suit_isomorphism[suit1];
                matched = true;
                break;  // Rust: `continue 'outer`
            }
        }
        if (!matched) suit_isomorphism[suit2] = next_index++;
    }

    const uint64_t flop_mask =
        (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) | (uint64_t{1} << flop[2]);
    std::array<int32_t, 4> flop_rankset{0, 0, 0, 0};
    for (Card card : flop) flop_rankset[card & 3] |= int32_t{1} << (card >> 2);

    // -1 means "not isomorphic to anything"; Rust uses Option<u8>.
    std::array<int8_t, 4> isomorphic_suit{-1, -1, -1, -1};
    std::vector<size_t> reverse_table(kNumHandIndices, static_cast<size_t>(-1));

    if (turn == NOT_DEALT) {
        for (uint8_t suit1 = 1; suit1 < 4; ++suit1) {
            for (uint8_t suit2 = 0; suit2 < suit1; ++suit2) {
                if (flop_rankset[suit1] == flop_rankset[suit2] &&
                    suit_isomorphism[suit1] == suit_isomorphism[suit2]) {
                    isomorphic_suit[suit1] = static_cast<int8_t>(suit2);
                    isomorphism_swap_internal(out.swap_turn, reverse_table, suit1, suit2,
                                              private_cards);
                    break;
                }
            }
        }
        isomorphism_internal(out.ref_turn, out.card_turn, flop_mask, isomorphic_suit);
    }

    out.ref_river.assign(52, std::vector<uint8_t>{});

    if (river == NOT_DEALT) {
        for (Card t = 0; t < 52; ++t) {
            if (((uint64_t{1} << t) & flop_mask) != 0) continue;
            if (turn != NOT_DEALT && turn != t) continue;

            const uint64_t turn_mask = flop_mask | (uint64_t{1} << t);
            std::array<int32_t, 4> turn_rankset = flop_rankset;
            turn_rankset[t & 3] |= int32_t{1} << (t >> 2);

            isomorphic_suit.fill(-1);

            for (uint8_t suit1 = 1; suit1 < 4; ++suit1) {
                for (uint8_t suit2 = 0; suit2 < suit1; ++suit2) {
                    if ((flop_rankset[suit1] == flop_rankset[suit2] || turn != NOT_DEALT) &&
                        turn_rankset[suit1] == turn_rankset[suit2] &&
                        suit_isomorphism[suit1] == suit_isomorphism[suit2]) {
                        isomorphic_suit[suit1] = static_cast<int8_t>(suit2);
                        isomorphism_swap_internal(out.swap_river[t & 3], reverse_table, suit1,
                                                  suit2, private_cards);
                        break;
                    }
                }
            }

            isomorphism_internal(out.ref_river[t], out.card_river[t & 3], turn_mask,
                                 isomorphic_suit);
        }
    }

    return out;
}

}  // namespace pfs
