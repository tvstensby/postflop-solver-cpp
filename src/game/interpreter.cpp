// Port of src/game/interpreter.rs -- post-solve navigation and queries.
//
// Errors here are panics, not Results, mirroring the Rust: construction and
// parsing return Result, misuse of the interpreter is a programming error.
#include <pfs/game.hpp>
#include <pfs/utility.hpp>

#include "../core/encoding.hpp"
#include "../core/numeric.hpp"
#include "../core/sliceop.hpp"

#include <algorithm>
#include <array>

namespace pfs {

namespace {

std::vector<float> decode_to_vector(std::span<const int16_t> src, float scale) {
    std::vector<float> out(src.size());
    decode_signed_slice(out, src, scale);
    return out;
}

}  // namespace

const PostFlopNode& PostFlopGame::current_node() const {
    return node_arena_[node_history_.empty() ? 0 : node_history_.back()];
}

PostFlopNode& PostFlopGame::current_node_mut() {
    return node_arena_[node_history_.empty() ? 0 : node_history_.back()];
}

void PostFlopGame::back_to_root() {
    if (state_ <= GameState::Uninitialized) return;  // called during construction too

    action_history_.clear();
    node_history_.clear();
    is_normalized_weight_cached_ = false;
    turn_ = card_config_.turn;
    river_ = card_config_.river;
    turn_swapped_suit_.reset();
    turn_swap_.reset();
    river_swap_.reset();
    total_bet_amount_ = {0, 0};

    for (size_t p = 0; p < 2; ++p) {
        weights_[p].assign(initial_weights_[p].begin(), initial_weights_[p].end());
    }
    assign_zero_weights();
}

void PostFlopGame::apply_history(std::span<const size_t> history) {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    // Copy first: back_to_root clears action_history_, which may alias `history`.
    const std::vector<size_t> hist(history.begin(), history.end());
    back_to_root();
    for (size_t a : hist) play(a);
}

bool PostFlopGame::is_terminal_node() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    const PostFlopNode& node = current_node();
    // A turn/river node reached by calling an all-in counts as terminal.
    return node.is_terminal() || node.amount() == tree_config_.effective_stack;
}

bool PostFlopGame::is_chance_node() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    return current_node().is_chance() && !is_terminal_node();
}

std::vector<Action> PostFlopGame::available_actions() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    if (is_terminal_node()) return {};
    std::vector<Action> out;
    for (const PostFlopNode& c : current_node().children()) out.push_back(c.prev_action());
    return out;
}

size_t PostFlopGame::current_player() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    return current_node().player();
}

std::vector<Card> PostFlopGame::current_board() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    std::vector<Card> ret(card_config_.flop.begin(), card_config_.flop.end());
    if (turn_ != NOT_DEALT) ret.push_back(turn_);
    if (river_ != NOT_DEALT) ret.push_back(river_);
    return ret;
}

uint64_t PostFlopGame::possible_cards() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    if (!is_chance_node()) return 0;

    const std::array<Card, 3>& flop = card_config_.flop;
    uint64_t board_mask =
        (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) | (uint64_t{1} << flop[2]);
    uint64_t dead_mask = 0;

    if (turn_ != NOT_DEALT) board_mask |= uint64_t{1} << turn_;

    // A card is dealable if at least one OOP x IP hand assignment survives it.
    for (Card card = 0; card < 52; ++card) {
        const uint64_t bit_card = uint64_t{1} << card;
        const uint64_t new_board_mask = board_mask | bit_card;
        bool alive = false;

        if (new_board_mask != board_mask) {
            for (const Hole& oop : private_cards_[0]) {
                const uint64_t oop_mask =
                    (uint64_t{1} << oop.first) | (uint64_t{1} << oop.second);
                if ((oop_mask & new_board_mask) != 0) continue;
                const uint64_t combined = oop_mask | new_board_mask;
                for (const Hole& ip : private_cards_[1]) {
                    const uint64_t ip_mask =
                        (uint64_t{1} << ip.first) | (uint64_t{1} << ip.second);
                    if ((ip_mask & combined) == 0) {
                        alive = true;
                        break;
                    }
                }
                if (alive) break;
            }
        }

        if (!alive) dead_mask |= bit_card;
    }

    return ((uint64_t{1} << 52) - 1) ^ dead_mask;
}

void PostFlopGame::assign_zero_weights() {
    if (bunching_num_dead_cards_ != 0) {
        // A hand is impossible when its arena run is absent or entirely zero.
        for (size_t player = 0; player < 2; ++player) {
            const PostFlopNode& node = current_node();
            const size_t opponent_len = num_private_hands(player ^ 1);
            const std::vector<size_t>* indices = nullptr;
            if (node.turn() == NOT_DEALT)
                indices = &bunching_num_flop_[player];
            else if (node.river() == NOT_DEALT)
                indices = &bunching_num_turn_[player][node.turn()];
            else
                indices =
                    &bunching_num_river_[player][card_pair_to_index(node.turn(), node.river())];

            std::vector<float> buf;
            std::vector<float>* w = &weights_[player];
            if (turn_swap_ || river_swap_) {
                buf = weights_[player];
                apply_swap_to(buf, player, true);
                w = &buf;
            }

            for (size_t i = 0; i < w->size() && i < indices->size(); ++i) {
                const size_t index = (*indices)[i];
                if (index == 0) {
                    (*w)[i] = 0.0f;
                    continue;
                }
                bool all_zero = true;
                for (size_t j = 0; j < opponent_len; ++j)
                    if (bunching_arena_[index + j] != 0.0f) {
                        all_zero = false;
                        break;
                    }
                if (all_zero) (*w)[i] = 0.0f;
            }

            if (turn_swap_ || river_swap_) {
                apply_swap_to(buf, player, false);
                weights_[player] = buf;
            }
        }
        return;
    }

    uint64_t board_mask = 0;
    if (turn_ != NOT_DEALT) board_mask |= uint64_t{1} << turn_;
    if (river_ != NOT_DEALT) board_mask |= uint64_t{1} << river_;

    for (size_t player = 0; player < 2; ++player) {
        // Cards the opponent must hold in every surviving combination are dead for us.
        uint64_t dead_mask = (uint64_t{1} << 52) - 1;
        for (const Hole& h : private_cards_[player ^ 1]) {
            const uint64_t mask = (uint64_t{1} << h.first) | (uint64_t{1} << h.second);
            if ((mask & board_mask) == 0) dead_mask &= mask;
            if (dead_mask == 0) break;
        }
        dead_mask |= board_mask;

        for (size_t i = 0; i < private_cards_[player].size(); ++i) {
            const Hole& h = private_cards_[player][i];
            const uint64_t mask = (uint64_t{1} << h.first) | (uint64_t{1} << h.second);
            if ((mask & dead_mask) != 0) weights_[player][i] = 0.0f;
        }
    }
}

// Order matters: forward is [turn, river], reverse is [river, turn].
void PostFlopGame::apply_swap_to(std::span<float> slice, size_t player, bool reverse) const {
    const std::vector<Swap>* turn = nullptr;
    const std::vector<Swap>* river = nullptr;
    if (turn_swap_) turn = &iso_.swap_turn[*turn_swap_][player];
    if (river_swap_) river = &iso_.swap_river[river_swap_->first][river_swap_->second][player];

    const std::vector<Swap>* order[2] = {turn, river};
    if (reverse) {
        order[0] = river;
        order[1] = turn;
    }
    for (const std::vector<Swap>* s : order)
        if (s) apply_swap(slice, std::span<const Swap>(*s));
}

void PostFlopGame::play(size_t action) {
    if (state_ < GameState::MemoryAllocated) PFS_PANIC("Memory is not allocated");
    if (is_terminal_node()) PFS_PANIC("Terminal node is not allowed");

    if (is_chance_node()) {
        const bool is_turn = turn_ == NOT_DEALT;
        if (storage_mode_ == BoardState::Flop ||
            (!is_turn && storage_mode_ == BoardState::Turn))
            PFS_PANIC("Storage mode is not compatible");

        const Card actual_card =
            action == SIZE_MAX
                ? static_cast<Card>(std::countr_zero(possible_cards()))
                : static_cast<Card>(action);

        // If a suit was swapped on the turn, translate into the tree's naming.
        Card action_card = actual_card;
        if (turn_swapped_suit_) {
            const uint8_t s1 = turn_swapped_suit_->first;
            const uint8_t s2 = turn_swapped_suit_->second;
            if ((actual_card & 3) == s1) action_card = static_cast<Card>(actual_card - s1 + s2);
            else if ((actual_card & 3) == s2) action_card = static_cast<Card>(actual_card + s1 - s2);
        }

        const std::vector<Action> actions = available_actions();
        size_t action_index = SIZE_MAX;

        for (size_t i = 0; i < actions.size(); ++i)
            if (actions[i] == Action::chance(action_card)) {
                action_index = i;
                break;
            }

        if (action_index == SIZE_MAX) {
            // Not a represented card: find it among the isomorphic ones and record
            // the swap so later queries can be translated back.
            const PostFlopNode& node = current_node();
            const std::span<const uint8_t> isomorphism = isomorphic_chances(node);
            const std::vector<Card>& iso_cards =
                node.turn() == NOT_DEALT ? iso_.card_turn : iso_.card_river[node.turn() & 3];
            for (size_t i = 0; i < isomorphism.size(); ++i) {
                if (action_card != iso_cards[i]) continue;
                action_index = isomorphism[i];
                if (is_turn) {
                    const Action repr = actions[action_index];
                    if (repr.kind == ActionKind::Chance)
                        turn_swapped_suit_ = std::pair<uint8_t, uint8_t>(
                            static_cast<uint8_t>(action_card & 3),
                            static_cast<uint8_t>(repr.amount & 3));
                    turn_swap_ = static_cast<uint8_t>(action_card & 3);
                } else {
                    // turn_ may differ from node.turn() when turn_swap_ is set; that
                    // only happens on a monotone flop, where exactly one suit can be
                    // swapped, so this indexing is correct.
                    river_swap_ = std::pair<uint8_t, uint8_t>(
                        static_cast<uint8_t>(turn_ & 3),
                        static_cast<uint8_t>(iso_.card_river[turn_ & 3][i] & 3));
                }
                break;
            }
        }

        if (action_index == SIZE_MAX) PFS_PANIC("Invalid action");

        node_history_.push_back(node_index(current_node_mut().play(action_index)));
        if (is_turn) turn_ = actual_card;
        else river_ = actual_card;

        assign_zero_weights();
    } else {
        PostFlopNode& node = current_node_mut();
        if (action >= node.num_actions()) PFS_PANIC("Invalid action");

        const size_t player = node.player();
        const size_t num_hands = num_private_hands(player);

        if (node.num_actions() > 1) {
            const std::vector<float> strat = strategy();
            mul_slice(weights_[player],
                      row(std::span<const float>(strat), action, num_hands));
        }

        // Cache the chosen action's cfvalue row for the non-acting player's queries.
        std::vector<float> vec;
        if (is_compression_enabled_) {
            const std::span<int16_t> all = node.cfvalues_compressed();
            vec = decode_to_vector(row(std::span<const int16_t>(all), action, num_hands),
                                  node.cfvalue_scale());
        } else {
            const std::span<float> all = node.cfvalues();
            const std::span<const float> r = row(std::span<const float>(all), action, num_hands);
            vec.assign(r.begin(), r.end());
        }
        cfvalues_cache_[player].assign(vec.begin(), vec.end());

        const Action next = node.play(action).prev_action();
        if (next.kind == ActionKind::Call) {
            total_bet_amount_[player] = total_bet_amount_[player ^ 1];
        } else if (next.is_bet_like()) {
            const Action prev = node.prev_action();
            const int32_t prev_bet_amount = prev.is_bet_like() ? prev.amount : 0;
            const int32_t to_call = total_bet_amount_[player ^ 1] - total_bet_amount_[player];
            total_bet_amount_[player] += next.amount - prev_bet_amount + to_call;
        }

        node_history_.push_back(node_index(current_node_mut().play(action)));
    }

    action_history_.push_back(action);
    is_normalized_weight_cached_ = false;
}

void PostFlopGame::cache_normalized_weights() {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    if (is_normalized_weight_cached_) return;

    if (bunching_num_dead_cards_ != 0) {
        // O(#oop * #ip): the compatible-opponent weight is a dot product against
        // the arena run rather than an inclusion-exclusion correction.
        std::array<std::vector<float>, 2> buf;
        const std::array<std::vector<float>, 2>* w = &weights_;
        if (turn_swap_ || river_swap_) {
            buf[0] = weights_[0];
            buf[1] = weights_[1];
            apply_swap_to(buf[0], 0, true);
            apply_swap_to(buf[1], 1, true);
            w = &buf;
        }

        for (size_t player = 0; player < 2; ++player) {
            const PostFlopNode& node = current_node();
            const std::vector<size_t>* indices = nullptr;
            if (node.river() != NOT_DEALT)
                indices =
                    &bunching_num_river_[player][card_pair_to_index(node.turn(), node.river())];
            else if (node.turn() != NOT_DEALT)
                indices = &bunching_num_turn_[player][node.turn()];
            else
                indices = &bunching_num_flop_[player];

            const size_t opponent_len = num_private_hands(player ^ 1);
            std::vector<float> normalized(indices->size(), 0.0f);
            for (size_t i = 0; i < indices->size(); ++i) {
                const size_t index = (*indices)[i];
                if (index == 0) continue;
                normalized[i] =
                    (*w)[player][i] *
                    inner_product((*w)[player ^ 1],
                                  std::span<const float>(bunching_arena_.data() + index,
                                                         opponent_len));
            }
            apply_swap_to(normalized, player, false);
            normalized_weights_[player] = std::move(normalized);
        }

        is_normalized_weight_cached_ = true;
        return;
    }

    uint64_t board_mask = 0;
    if (turn_ != NOT_DEALT) board_mask |= uint64_t{1} << turn_;
    if (river_ != NOT_DEALT) board_mask |= uint64_t{1} << river_;

    std::array<double, 2> weight_sum{0.0, 0.0};
    std::array<std::array<double, 52>, 2> weight_sum_minus{};

    for (size_t player = 0; player < 2; ++player) {
        for (size_t i = 0; i < private_cards_[player].size(); ++i) {
            const Hole& h = private_cards_[player][i];
            const uint64_t mask = (uint64_t{1} << h.first) | (uint64_t{1} << h.second);
            if ((mask & board_mask) != 0) continue;
            const double w = static_cast<double>(weights_[player][i]);
            weight_sum[player] += w;
            weight_sum_minus[player][h.first] += w;
            weight_sum_minus[player][h.second] += w;
        }
    }

    // "Normalized weight" is the actual number of combinations of each hand: our
    // own weight times the total compatible opponent weight, by inclusion-exclusion.
    for (size_t player = 0; player < 2; ++player) {
        const std::vector<Hole>& cards = private_cards_[player];
        const std::vector<uint16_t>& same_hand_index = same_hand_index_[player];
        const std::vector<float>& opp_weights = weights_[player ^ 1];
        const double opp_sum = weight_sum[player ^ 1];
        const std::array<double, 52>& opp_minus = weight_sum_minus[player ^ 1];

        for (size_t i = 0; i < cards.size(); ++i) {
            const Card c1 = cards[i].first;
            const Card c2 = cards[i].second;
            const uint64_t mask = (uint64_t{1} << c1) | (uint64_t{1} << c2);
            if ((mask & board_mask) != 0) {
                normalized_weights_[player][i] = 0.0f;
                continue;
            }
            const uint16_t same_i = same_hand_index[i];
            const double opp_same =
                same_i == UINT16_MAX ? 0.0 : static_cast<double>(opp_weights[same_i]);
            const double opp = opp_sum + opp_same - opp_minus[c1] - opp_minus[c2];
            normalized_weights_[player][i] =
                weights_[player][i] * static_cast<float>(opp);
        }
    }

    is_normalized_weight_cached_ = true;
}

std::span<const float> PostFlopGame::weights(size_t player) const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    return weights_[player];
}

std::span<const float> PostFlopGame::normalized_weights(size_t player) const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    if (!is_normalized_weight_cached_) PFS_PANIC("Normalized weights are not cached");
    return normalized_weights_[player];
}

void PostFlopGame::equity_internal(std::span<double> result, size_t player, Card turn, Card river,
                                  double amount) const {
    const size_t pair_index = card_pair_to_index(turn, river);
    const HandStrength& hs = hand_strength_[pair_index];
    const std::vector<StrengthItem>& ps = hs[player];
    const std::vector<StrengthItem>& os = hs[player ^ 1];
    if (ps.empty() || os.empty()) return;

    const std::vector<Hole>& player_cards = private_cards_[player];
    const std::vector<Hole>& opponent_cards = private_cards_[player ^ 1];
    const std::vector<float>& opp_weights = weights_[player ^ 1];

    double weight_sum = 0.0;
    std::array<double, 52> weight_minus{};

    size_t i = 1;
    for (size_t p = 1; p + 1 < ps.size(); ++p) {
        const uint16_t strength = ps[p].strength;
        const uint16_t index = ps[p].index;
        while (os[i].strength < strength) {
            const size_t oi = os[i].index;
            const double w = static_cast<double>(opp_weights[oi]);
            weight_sum += w;
            weight_minus[opponent_cards[oi].first] += w;
            weight_minus[opponent_cards[oi].second] += w;
            ++i;
        }
        const Card c1 = player_cards[index].first;
        const Card c2 = player_cards[index].second;
        result[index] += amount * (weight_sum - weight_minus[c1] - weight_minus[c2]);
    }

    weight_sum = 0.0;
    weight_minus.fill(0.0);
    i = os.size() - 2;

    for (size_t p = ps.size() - 1; p-- > 1;) {
        const uint16_t strength = ps[p].strength;
        const uint16_t index = ps[p].index;
        while (os[i].strength > strength) {
            const size_t oi = os[i].index;
            const double w = static_cast<double>(opp_weights[oi]);
            weight_sum += w;
            weight_minus[opponent_cards[oi].first] += w;
            weight_minus[opponent_cards[oi].second] += w;
            --i;
        }
        const Card c1 = player_cards[index].first;
        const Card c2 = player_cards[index].second;
        result[index] -= amount * (weight_sum - weight_minus[c1] - weight_minus[c2]);
    }
}

std::vector<float> PostFlopGame::equity(size_t player) const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    if (!is_normalized_weight_cached_) PFS_PANIC("Normalized weights are not cached");

    const size_t num_hands = num_private_hands(player);

    if (bunching_num_dead_cards_ != 0) {
        std::vector<float> tmp = equity_internal_bunching(player);
        apply_swap_to(tmp, player, false);
        std::vector<float> out(num_hands, 0.0f);
        for (size_t i = 0; i < num_hands && i < tmp.size(); ++i) {
            const float w_raw = weights_[player][i];
            const float w_norm = normalized_weights_[player][i];
            out[i] = w_norm > 0.0f ? tmp[i] * (w_raw / w_norm) + 0.5f : 0.0f;
        }
        return out;
    }

    std::vector<double> tmp(num_hands, 0.0);

    if (river_ != NOT_DEALT) {
        equity_internal(tmp, player, turn_, river_, 0.5);
    } else if (turn_ != NOT_DEALT) {
        for (Card river = 0; river < 52; ++river)
            if (turn_ != river) equity_internal(tmp, player, turn_, river, 0.5 / 44.0);
    } else {
        for (Card turn = 0; turn < 52; ++turn)
            for (Card river = static_cast<Card>(turn + 1); river < 52; ++river)
                equity_internal(tmp, player, turn, river, 1.0 / (45.0 * 44.0));
    }

    // Un-bias: the sweeps produce a centered +-0.5 value, so add 0.5 to land in [0, 1].
    std::vector<float> out(num_hands);
    for (size_t i = 0; i < num_hands; ++i) {
        const float w_raw = weights_[player][i];
        const float w_norm = normalized_weights_[player][i];
        out[i] = w_norm > 0.0f
                     ? static_cast<float>(tmp[i]) * (w_raw / w_norm) + 0.5f
                     : 0.0f;
    }
    return out;
}

std::vector<float> PostFlopGame::expected_values_detail(size_t player) const {
    if (state_ != GameState::Solved) PFS_PANIC("Game is not solved");
    if (!is_normalized_weight_cached_) PFS_PANIC("Normalized weights are not cached");

    // node() is const here but the accessors are non-const, mirroring the Rust's
    // use of MutexLike to reach through a shared reference.
    PostFlopNode& node = const_cast<PostFlopNode&>(current_node());
    const size_t num_hands = num_private_hands(player);

    size_t chance_factor = 1;
    if (card_config_.turn == NOT_DEALT && turn_ != NOT_DEALT)
        chance_factor *= 45 - bunching_num_dead_cards_;
    if (card_config_.river == NOT_DEALT && river_ != NOT_DEALT)
        chance_factor *= 44 - bunching_num_dead_cards_;

    // With bunching enabled the terminal evaluation divided by the bunching count,
    // so the same count must be multiplied back here.
    const double num_combinations =
        bunching_num_dead_cards_ == 0 ? num_combinations_ : bunching_num_combinations_;

    bool have_actions = false;
    float normalizer =
        static_cast<float>(num_combinations * static_cast<double>(chance_factor));

    std::vector<float> ret;

    if (node.is_terminal()) {
        normalizer = static_cast<float>(num_combinations);
        ret.assign(num_hands, 0.0f);
        std::vector<float> cfreach(weights_[player ^ 1].begin(), weights_[player ^ 1].end());
        apply_swap_to(cfreach, player ^ 1, true);
        const_cast<PostFlopGame*>(this)->evaluate(ret, node, player,
                                                 std::span<const float>(cfreach));
    } else if (node.is_chance() &&
               node.cfvalue_storage_player() == std::optional<size_t>(player)) {
        if (is_compression_enabled_)
            ret = decode_to_vector(node.cfvalues_chance_compressed(), node.cfvalue_chance_scale());
        else {
            const std::span<float> s = node.cfvalues_chance();
            ret.assign(s.begin(), s.end());
        }
    } else if (node.has_cfvalues_ip() && player == PLAYER_IP) {
        if (is_compression_enabled_)
            ret = decode_to_vector(node.cfvalues_ip_compressed(), node.cfvalue_ip_scale());
        else {
            const std::span<float> s = node.cfvalues_ip();
            ret.assign(s.begin(), s.end());
        }
    } else if (player == current_player()) {
        have_actions = true;
        if (is_compression_enabled_)
            ret = decode_to_vector(node.cfvalues_compressed(), node.cfvalue_scale());
        else {
            const std::span<float> s = node.cfvalues();
            ret.assign(s.begin(), s.end());
        }
    } else {
        ret.assign(cfvalues_cache_[player].begin(), cfvalues_cache_[player].end());
    }

    const int32_t starting_pot = tree_config_.starting_pot;
    const int32_t bias = std::max(total_bet_amount_[player] - total_bet_amount_[player ^ 1], 0);

    const size_t num_rows = num_hands == 0 ? 0 : ret.size() / num_hands;
    for (size_t action = 0; action < num_rows; ++action) {
        const bool is_fold =
            have_actions && node.play(action).prev_action().kind == ActionKind::Fold;
        std::span<float> r = row_mut(std::span<float>(ret), action, num_hands);
        apply_swap_to(r, player, false);
        for (size_t i = 0; i < num_hands; ++i) {
            const float w_raw = weights_[player][i];
            const float w_norm = normalized_weights_[player][i];
            if (is_fold || w_norm == 0.0f) {
                r[i] = 0.0f;
            } else {
                r[i] *= normalizer * (w_raw / w_norm);
                // Add back the pot bias the terminal evaluation subtracted.
                r[i] += static_cast<float>(starting_pot) * 0.5f +
                        static_cast<float>(node.amount() + bias);
            }
        }
    }

    return ret;
}

std::vector<float> PostFlopGame::expected_values(size_t player) const {
    if (state_ != GameState::Solved) PFS_PANIC("Game is not solved");
    if (!is_normalized_weight_cached_) PFS_PANIC("Normalized weights are not cached");

    const std::vector<float> detail = expected_values_detail(player);

    if (is_terminal_node() || is_chance_node() || current_player() != player) return detail;

    const size_t num_actions = current_node().num_actions();
    const size_t num_hands = num_private_hands(player);
    const std::vector<float> strat = strategy();

    std::vector<float> ret(num_hands, 0.0f);
    for (size_t i = 0; i < num_hands; ++i) {
        float ev = 0.0f;
        for (size_t j = 0; j < num_actions; ++j) {
            const size_t index = i + j * num_hands;
            ev += detail[index] * strat[index];
        }
        ret[i] = ev;
    }
    return ret;
}

std::vector<float> PostFlopGame::strategy() const {
    if (state_ < GameState::MemoryAllocated) PFS_PANIC("Memory is not allocated");
    if (is_terminal_node()) PFS_PANIC("Terminal node is not allowed");
    if (is_chance_node()) PFS_PANIC("Chance node is not allowed");

    PostFlopNode& node = const_cast<PostFlopNode&>(current_node());
    const size_t player = current_player();
    const size_t num_actions = node.num_actions();
    const size_t num_hands = num_private_hands(player);

    ScratchScope scratch;
    const std::span<float> normalized =
        is_compression_enabled_
            ? normalized_strategy<uint16_t>(
                  scratch, std::span<const uint16_t>(node.strategy_compressed()), num_actions)
            : normalized_strategy<float>(scratch, std::span<const float>(node.strategy()),
                                         num_actions);

    std::vector<float> ret(normalized.begin(), normalized.end());
    apply_locking_strategy(ret, locking_strategy(node));

    for (size_t a = 0; a < num_actions; ++a)
        apply_swap_to(row_mut(std::span<float>(ret), a, num_hands), player, false);

    return ret;
}

void PostFlopGame::lock_current_strategy(std::span<const float> strategy_in) {
    if (state_ < GameState::MemoryAllocated) PFS_PANIC("Memory is not allocated");
    if (state_ == GameState::Solved) PFS_PANIC("Game is already solved");
    if (is_terminal_node()) PFS_PANIC("Terminal node is not allowed");
    if (is_chance_node()) PFS_PANIC("Chance node is not allowed");

    PostFlopNode& node = current_node_mut();
    const size_t player = current_player();
    const size_t num_actions = node.num_actions();
    const size_t num_hands = num_private_hands(player);

    if (strategy_in.size() != num_actions * num_hands) PFS_PANIC("Invalid strategy length");

    // -1.0 means "this hand is not locked". A hand is locked iff any of its action
    // frequencies is strictly positive, in which case they are normalized to sum 1.
    std::vector<float> locking(num_actions * num_hands, -1.0f);

    for (size_t hand = 0; hand < num_hands; ++hand) {
        double sum = 0.0;
        bool lock = false;
        for (size_t action = 0; action < num_actions; ++action) {
            const float freq = strategy_in[action * num_hands + hand];
            if (freq > 0.0f) {
                sum += static_cast<double>(freq);
                lock = true;
            }
        }
        if (lock) {
            for (size_t action = 0; action < num_actions; ++action) {
                const float freq = fmax_raw(strategy_in[action * num_hands + hand], 0.0f);
                locking[action * num_hands + hand] =
                    static_cast<float>(static_cast<double>(freq) / sum);
            }
        }
    }

    for (size_t a = 0; a < num_actions; ++a)
        apply_swap_to(row_mut(std::span<float>(locking), a, num_hands), player, true);

    node.is_locked_ = true;
    locking_strategy_[node_index(node)] = std::move(locking);
}

void PostFlopGame::unlock_current_strategy() {
    if (state_ < GameState::MemoryAllocated) PFS_PANIC("Memory is not allocated");
    if (state_ == GameState::Solved) PFS_PANIC("Game is already solved");
    if (is_terminal_node()) PFS_PANIC("Terminal node is not allowed");
    if (is_chance_node()) PFS_PANIC("Chance node is not allowed");

    PostFlopNode& node = current_node_mut();
    if (!node.is_locked()) return;
    node.is_locked_ = false;
    locking_strategy_.erase(node_index(node));
}

std::optional<std::vector<float>> PostFlopGame::current_locking_strategy() const {
    if (state_ < GameState::MemoryAllocated) PFS_PANIC("Memory is not allocated");
    if (is_terminal_node()) PFS_PANIC("Terminal node is not allowed");
    if (is_chance_node()) PFS_PANIC("Chance node is not allowed");

    const auto it = locking_strategy_.find(node_index(current_node()));
    if (it == locking_strategy_.end()) return std::nullopt;

    std::vector<float> ret = it->second;
    const size_t player = current_player();
    const size_t num_hands = num_private_hands(player);
    for (size_t a = 0; a * num_hands < ret.size(); ++a)
        apply_swap_to(row_mut(std::span<float>(ret), a, num_hands), player, false);
    return ret;
}

}  // namespace pfs
