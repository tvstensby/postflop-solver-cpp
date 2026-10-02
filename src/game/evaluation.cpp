// Port of src/game/evaluation.rs -- counterfactual values at terminal nodes.
//
// The values written are cfvalue increments per unit of opponent reach, normalized
// by num_combinations, and CENTERED: winning is +half the pot and losing is -half,
// so the starting_pot/2 bias is subtracted here and added back by the interpreter.
//
// All three paths rely on the two sentinels that bracket every hand_strength list.
// The sweep loops below are deliberately unguarded -- {0, 0} at the front and
// {u16 max, u16 max} at the back are what stop them.
#include <pfs/game.hpp>

#include "../core/numeric.hpp"
#include "../core/sliceop.hpp"

#include <array>
#include <cstring>

namespace pfs {

void PostFlopGame::evaluate(std::span<float> result, PostFlopNode& node, size_t player,
                           std::span<const float> cfreach) {
    if (bunching_num_dead_cards_ == 0)
        evaluate_internal(result, node, player, cfreach);
    else
        evaluate_internal_bunching(result, node, player, cfreach);
}

void PostFlopGame::evaluate_internal(std::span<float> result, const PostFlopNode& node,
                                    size_t player, std::span<const float> cfreach) {
    const double pot = static_cast<double>(tree_config_.starting_pot + 2 * node.amount());
    const double half_pot = 0.5 * pot;
    const double rake = fmin_raw(pot * tree_config_.rake_rate, tree_config_.rake_cap);
    const double amount_win = (half_pot - rake) / num_combinations_;
    const double amount_lose = -half_pot / num_combinations_;

    const std::vector<Hole>& player_cards = private_cards_[player];
    const std::vector<Hole>& opponent_cards = private_cards_[player ^ 1];

    double cfreach_sum = 0.0;
    std::array<double, 52> cfreach_minus{};

    for (float& r : result) r = 0.0f;

    // ---------------------------------------------------------------- fold ---
    if ((node.player() & PLAYER_FOLD_FLAG) == PLAYER_FOLD_FLAG) {
        const uint8_t folded_player = static_cast<uint8_t>(node.player() & PLAYER_MASK);
        const double payoff = (folded_player != player) ? amount_win : amount_lose;

        const Indices* valid = nullptr;
        if (node.river() != NOT_DEALT)
            valid = &valid_indices_river_[card_pair_to_index(node.turn(), node.river())];
        else if (node.turn() != NOT_DEALT)
            valid = &valid_indices_turn_[node.turn()];
        else
            valid = &valid_indices_flop_;

        for (uint16_t i : (*valid)[player ^ 1]) {
            const float cfreach_i = cfreach[i];
            if (cfreach_i == 0.0f) continue;
            const double v = static_cast<double>(cfreach_i);
            cfreach_sum += v;
            cfreach_minus[opponent_cards[i].first] += v;
            cfreach_minus[opponent_cards[i].second] += v;
        }

        if (cfreach_sum == 0.0) return;

        const std::vector<uint16_t>& same_hand_index = same_hand_index_[player];
        for (uint16_t i : (*valid)[player]) {
            const Card c1 = player_cards[i].first;
            const Card c2 = player_cards[i].second;
            const uint16_t same_i = same_hand_index[i];
            const double cfreach_same =
                same_i == UINT16_MAX ? 0.0 : static_cast<double>(cfreach[same_i]);
            // Inclusion-exclusion: subtracting both card sums removes the identical
            // hand twice, so add it back once.
            const double cf =
                cfreach_sum + cfreach_same - cfreach_minus[c1] - cfreach_minus[c2];
            result[i] = static_cast<float>(payoff * cf);
        }
        return;
    }

    const size_t pair_index = card_pair_to_index(node.turn(), node.river());
    const HandStrength& hs = hand_strength_[pair_index];
    const std::vector<StrengthItem>& player_strength = hs[player];
    const std::vector<StrengthItem>& opponent_strength = hs[player ^ 1];
    // Strip the sentinels from the player's list; keep them on the opponent's,
    // where they act as the sweep bounds.
    const size_t pfirst = 1;
    const size_t plast = player_strength.size() - 1;  // exclusive

    // ------------------------------------------- showdown, no rake (2 sweeps) ---
    if (rake == 0.0) {
        size_t i = 1;
        for (size_t p = pfirst; p < plast; ++p) {
            const uint16_t strength = player_strength[p].strength;
            const uint16_t index = player_strength[p].index;
            while (opponent_strength[i].strength < strength) {
                const size_t oi = opponent_strength[i].index;
                const float cfreach_i = cfreach[oi];
                if (cfreach_i != 0.0f) {
                    const double v = static_cast<double>(cfreach_i);
                    cfreach_sum += v;
                    cfreach_minus[opponent_cards[oi].first] += v;
                    cfreach_minus[opponent_cards[oi].second] += v;
                }
                ++i;
            }
            const Card c1 = player_cards[index].first;
            const Card c2 = player_cards[index].second;
            const double cf = cfreach_sum - cfreach_minus[c1] - cfreach_minus[c2];
            result[index] = static_cast<float>(amount_win * cf);
        }

        cfreach_sum = 0.0;
        cfreach_minus.fill(0.0);
        i = opponent_strength.size() - 2;

        for (size_t p = plast; p-- > pfirst;) {
            const uint16_t strength = player_strength[p].strength;
            const uint16_t index = player_strength[p].index;
            while (opponent_strength[i].strength > strength) {
                const size_t oi = opponent_strength[i].index;
                const float cfreach_i = cfreach[oi];
                if (cfreach_i != 0.0f) {
                    const double v = static_cast<double>(cfreach_i);
                    cfreach_sum += v;
                    cfreach_minus[opponent_cards[oi].first] += v;
                    cfreach_minus[opponent_cards[oi].second] += v;
                }
                --i;
            }
            const Card c1 = player_cards[index].first;
            const Card c2 = player_cards[index].second;
            const double cf = cfreach_sum - cfreach_minus[c1] - cfreach_minus[c2];
            result[index] += static_cast<float>(amount_lose * cf);
        }
        // Ties contribute nothing and need no same-hand correction here: a hand
        // cannot beat or lose to itself, and the two card subtractions cancel the
        // identical-hand term in each sweep.
        return;
    }

    // -------------------------------------------- showdown, raked (3 sweeps) ---
    // The tie bucket now matters, so track two cursors: i is the strictly-worse
    // boundary and j the end of the tie group.
    const double amount_tie = -0.5 * rake / num_combinations_;
    const std::vector<uint16_t>& same_hand_index = same_hand_index_[player];

    for (size_t o = 1; o + 1 < opponent_strength.size(); ++o) {
        const size_t oi = opponent_strength[o].index;
        const float cfreach_i = cfreach[oi];
        if (cfreach_i == 0.0f) continue;
        const double v = static_cast<double>(cfreach_i);
        cfreach_sum += v;
        cfreach_minus[opponent_cards[oi].first] += v;
        cfreach_minus[opponent_cards[oi].second] += v;
    }

    if (cfreach_sum == 0.0) return;

    double cfreach_sum_win = 0.0;
    double cfreach_sum_tie = 0.0;
    std::array<double, 52> cfreach_minus_win{};
    std::array<double, 52> cfreach_minus_tie{};

    size_t i = 1;
    size_t j = 1;
    uint16_t prev_strength = 0;  // real strengths are always > 0

    for (size_t p = pfirst; p < plast; ++p) {
        const uint16_t strength = player_strength[p].strength;
        const uint16_t index = player_strength[p].index;

        if (strength > prev_strength) {
            prev_strength = strength;

            if (i < j) {
                cfreach_sum_win = cfreach_sum_tie;
                cfreach_minus_win = cfreach_minus_tie;
                i = j;
            }

            while (opponent_strength[i].strength < strength) {
                const size_t oi = opponent_strength[i].index;
                const double v = static_cast<double>(cfreach[oi]);
                cfreach_sum_win += v;
                cfreach_minus_win[opponent_cards[oi].first] += v;
                cfreach_minus_win[opponent_cards[oi].second] += v;
                ++i;
            }

            if (j < i) {
                cfreach_sum_tie = cfreach_sum_win;
                cfreach_minus_tie = cfreach_minus_win;
                j = i;
            }

            while (opponent_strength[j].strength == strength) {
                const size_t oj = opponent_strength[j].index;
                const double v = static_cast<double>(cfreach[oj]);
                cfreach_sum_tie += v;
                cfreach_minus_tie[opponent_cards[oj].first] += v;
                cfreach_minus_tie[opponent_cards[oj].second] += v;
                ++j;
            }
        }

        const Card c1 = player_cards[index].first;
        const Card c2 = player_cards[index].second;
        const double cfreach_total = cfreach_sum - cfreach_minus[c1] - cfreach_minus[c2];
        const double cfreach_win =
            cfreach_sum_win - cfreach_minus_win[c1] - cfreach_minus_win[c2];
        const double cfreach_tie =
            cfreach_sum_tie - cfreach_minus_tie[c1] - cfreach_minus_tie[c2];
        const uint16_t same_i = same_hand_index[index];
        const double cfreach_same =
            same_i == UINT16_MAX ? 0.0 : static_cast<double>(cfreach[same_i]);

        result[index] = static_cast<float>(amount_win * cfreach_win +
                                          amount_tie * (cfreach_tie - cfreach_win + cfreach_same) +
                                          amount_lose * (cfreach_total - cfreach_tie));
    }
}

}  // namespace pfs
