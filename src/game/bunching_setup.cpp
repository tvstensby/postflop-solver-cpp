// Port of the bunching half of src/game/base.rs, plus
// evaluate_internal_bunching from src/game/evaluation.rs.
//
// Everything is packed into one flat `bunching_arena_` of floats, with index
// tables pointing into it. Index 0 is a dummy element so that 0 can mean
// "absent" -- that sentinel is load-bearing throughout.
#include <pfs/bunching.hpp>
#include <pfs/game.hpp>

#include "../core/encoding.hpp"
#include "../core/numeric.hpp"
#include "../core/sliceop.hpp"
#include "../core/thread_pool.hpp"

#include <algorithm>
#include <numeric>

namespace pfs {

namespace {

uint64_t hole_mask(const Hole& h) {
    return (uint64_t{1} << h.first) | (uint64_t{1} << h.second);
}

uint64_t mask_of_cards(const std::vector<Card>& cards) {
    uint64_t m = 0;
    for (Card c : cards) m |= uint64_t{1} << c;
    return m;
}

// Appends each non-empty inner run to the arena and records its start index; an
// empty run records 0, the "absent" sentinel.
std::vector<std::vector<size_t>> push_to_arena(std::vector<float>& arena,
                                              std::vector<std::vector<std::vector<float>>> buf) {
    std::vector<std::vector<size_t>> ret;
    ret.reserve(buf.size());
    for (auto& outer : buf) {
        std::vector<size_t> indices;
        indices.reserve(outer.size());
        for (auto& inner : outer) {
            if (inner.empty()) {
                indices.push_back(0);
            } else {
                indices.push_back(arena.size());
                arena.insert(arena.end(), inner.begin(), inner.end());
            }
        }
        ret.push_back(std::move(indices));
    }
    return ret;
}

std::vector<std::vector<size_t>> push_to_arena_f64(
    std::vector<float>& arena, std::vector<std::vector<std::vector<double>>> buf) {
    std::vector<std::vector<size_t>> ret;
    ret.reserve(buf.size());
    for (auto& outer : buf) {
        std::vector<size_t> indices;
        indices.reserve(outer.size());
        for (auto& inner : outer) {
            if (inner.empty()) {
                indices.push_back(0);
            } else {
                indices.push_back(arena.size());
                for (double v : inner) arena.push_back(static_cast<float>(v));
            }
        }
        ret.push_back(std::move(indices));
    }
    return ret;
}

}  // namespace

void PostFlopGame::clear_bunching_effect() {
    bunching_num_dead_cards_ = 0;
    bunching_num_combinations_ = 0.0;
    bunching_arena_.clear();
    bunching_arena_.shrink_to_fit();
    bunching_strength_.clear();
    for (size_t p = 0; p < 2; ++p) {
        bunching_num_flop_[p].clear();
        bunching_num_turn_[p].clear();
        bunching_num_river_[p].clear();
        bunching_coef_flop_[p].clear();
        bunching_coef_turn_[p].clear();
    }
    back_to_root();
}

Status PostFlopGame::set_bunching_effect(const BunchingData& data) {
    if (state_ <= GameState::Uninitialized)
        return Status::err("Game is not successfully initialized");
    if (!data.is_ready()) return Status::err("Bunching configuration is not ready");

    std::array<Card, 3> flop_sorted = card_config_.flop;
    std::sort(flop_sorted.begin(), flop_sorted.end());
    if (flop_sorted != data.flop()) return Status::err("Flop cards do not match");

    clear_bunching_effect();
    return set_bunching_effect_internal(data);
}

uint64_t PostFlopGame::memory_usage_bunching() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    uint64_t m = static_cast<uint64_t>(bunching_arena_.capacity()) * sizeof(float);
    for (const auto& s : bunching_strength_)
        for (const auto& v : s) m += static_cast<uint64_t>(v.capacity()) * sizeof(uint16_t);
    for (size_t p = 0; p < 2; ++p) {
        m += static_cast<uint64_t>(bunching_num_flop_[p].capacity()) * sizeof(size_t);
        m += static_cast<uint64_t>(bunching_coef_flop_[p].capacity()) * sizeof(size_t);
        for (const auto& v : bunching_num_turn_[p])
            m += static_cast<uint64_t>(v.capacity()) * sizeof(size_t);
        for (const auto& v : bunching_num_river_[p])
            m += static_cast<uint64_t>(v.capacity()) * sizeof(size_t);
        for (const auto& v : bunching_coef_turn_[p])
            m += static_cast<uint64_t>(v.capacity()) * sizeof(size_t);
    }
    return m;
}

Status PostFlopGame::set_bunching_effect_internal(const BunchingData& data) {
    bunching_num_dead_cards_ = data.fold_ranges().size() * 2;
    std::vector<float> arena{0.0f};  // index 0 is the "absent" dummy

    const size_t oop_len = num_private_hands(0);
    const size_t ip_len = num_private_hands(1);
    const std::array<size_t, 2> lens{oop_len, ip_len};

    // --- hand strength, densified by hand index (0 means the hand is impossible) ---
    bunching_strength_.assign(hand_strength_.size(), {});
    for (size_t pair_index = 0; pair_index < hand_strength_.size(); ++pair_index) {
        const HandStrength& strength = hand_strength_[pair_index];
        if (strength[0].empty()) continue;
        for (size_t player = 0; player < 2; ++player) {
            bunching_strength_[pair_index][player].assign(lens[player], 0);
            // Skip the two sentinels.
            for (size_t i = 1; i + 1 < strength[player].size(); ++i)
                bunching_strength_[pair_index][player][strength[player][i].index] =
                    strength[player][i].strength;
        }
    }

    // --- flop combination counts ---
    if (card_config_.turn == NOT_DEALT) {
        for (size_t player = 0; player < 2; ++player) {
            const std::vector<Hole>& mine = private_cards_[player];
            const std::vector<Hole>& theirs = private_cards_[player ^ 1];
            std::vector<size_t> indices;
            indices.reserve(mine.size());

            for (const Hole& h : mine) {
                indices.push_back(arena.size());
                const uint64_t player_mask = hole_mask(h);
                for (const Hole& o : theirs) {
                    const uint64_t opp_mask = hole_mask(o);
                    arena.push_back((player_mask & opp_mask) != 0
                                        ? 0.0f
                                        : data.result_4cards(player_mask | opp_mask));
                }
            }

            if (player == 0) {
                bunching_num_combinations_ =
                    std::accumulate(arena.begin(), arena.end(), 0.0,
                                    [](double a, float x) { return a + static_cast<double>(x); });
                if (bunching_num_combinations_ == 0.0) {
                    clear_bunching_effect();
                    return Status::err("Valid combination not found");
                }
            }
            bunching_num_flop_[player] = std::move(indices);
        }
    }

    const uint64_t flop_mask = (uint64_t{1} << card_config_.flop[0]) |
                               (uint64_t{1} << card_config_.flop[1]) |
                               (uint64_t{1} << card_config_.flop[2]);
    const uint64_t skip_turn_mask = mask_of_cards(iso_.card_turn);

    // --- turn combination counts ---
    if (card_config_.river == NOT_DEALT) {
        for (size_t player = 0; player < 2; ++player) {
            const std::vector<Hole>& mine = private_cards_[player];
            const std::vector<Hole>& theirs = private_cards_[player ^ 1];

            std::vector<std::vector<std::vector<float>>> buf(52);
            parallel_for_range(0, 52, [&](size_t turn) {
                const uint64_t bit_turn = uint64_t{1} << turn;
                if ((bit_turn & (flop_mask | skip_turn_mask)) != 0) return;
                if (card_config_.turn != NOT_DEALT && card_config_.turn != turn) return;

                std::vector<std::vector<float>> outer;
                outer.reserve(mine.size());
                for (const Hole& h : mine) {
                    const uint64_t player_mask = hole_mask(h);
                    if ((player_mask & bit_turn) != 0) {
                        outer.emplace_back();
                        continue;
                    }
                    std::vector<float> inner;
                    inner.reserve(theirs.size());
                    for (const Hole& o : theirs) {
                        const uint64_t opp_mask = hole_mask(o);
                        inner.push_back(((player_mask | bit_turn) & opp_mask) != 0
                                            ? 0.0f
                                            : data.result_5cards(player_mask | opp_mask | bit_turn));
                    }
                    outer.push_back(std::move(inner));
                }
                buf[turn] = std::move(outer);
            });

            bunching_num_turn_[player] = push_to_arena(arena, std::move(buf));

            if (card_config_.turn != NOT_DEALT && player == 0) {
                bunching_num_combinations_ =
                    std::accumulate(arena.begin(), arena.end(), 0.0,
                                    [](double a, float x) { return a + static_cast<double>(x); });
                if (bunching_num_combinations_ == 0.0) {
                    clear_bunching_effect();
                    return Status::err("Valid combination not found");
                }
            }
        }
    }

    auto is_board_possible = [&](Card turn, Card river) {
        const uint64_t bit_turn = uint64_t{1} << turn;
        const uint64_t bit_river = uint64_t{1} << river;
        const std::vector<Card>& iso_card = iso_.card_river[turn & 3];
        return (bit_turn & (flop_mask | skip_turn_mask)) == 0 && (bit_river & flop_mask) == 0 &&
               std::find(iso_card.begin(), iso_card.end(), river) == iso_card.end() &&
               (card_config_.turn == NOT_DEALT || card_config_.turn == turn) &&
               (card_config_.river == NOT_DEALT || card_config_.river == river);
    };

    // --- river combination counts ---
    for (size_t player = 0; player < 2; ++player) {
        const std::vector<Hole>& mine = private_cards_[player];
        const std::vector<Hole>& theirs = private_cards_[player ^ 1];

        std::vector<std::vector<std::vector<float>>> buf(kNumHandIndices);
        parallel_for_range(0, kNumHandIndices, [&](size_t index) {
            const Hole board = index_to_card_pair(index);
            if (!is_board_possible(board.first, board.second) &&
                !is_board_possible(board.second, board.first))
                return;

            const uint64_t board_mask =
                (uint64_t{1} << board.first) | (uint64_t{1} << board.second);
            std::vector<std::vector<float>> outer;
            outer.reserve(mine.size());
            for (const Hole& h : mine) {
                const uint64_t player_mask = hole_mask(h);
                if ((player_mask & board_mask) != 0) {
                    outer.emplace_back();
                    continue;
                }
                std::vector<float> inner;
                inner.reserve(theirs.size());
                for (const Hole& o : theirs) {
                    const uint64_t opp_mask = hole_mask(o);
                    inner.push_back(((player_mask | board_mask) & opp_mask) != 0
                                        ? 0.0f
                                        : data.result_6cards(player_mask | opp_mask | board_mask));
                }
                outer.push_back(std::move(inner));
            }
            buf[index] = std::move(outer);
        });

        bunching_num_river_[player] = push_to_arena(arena, std::move(buf));

        if (card_config_.river != NOT_DEALT && player == 0) {
            bunching_num_combinations_ =
                std::accumulate(arena.begin(), arena.end(), 0.0,
                                [](double a, float x) { return a + static_cast<double>(x); });
            if (bunching_num_combinations_ == 0.0) {
                clear_bunching_effect();
                return Status::err("Valid combination not found");
            }
        }
    }

    if (card_config_.river != NOT_DEALT) {
        bunching_arena_ = std::move(arena);
        assign_zero_weights();
        return Status::ok();
    }

    // --- turn equity coefficients ---
    // Signed sum over rivers of the opponent-weight contributions, so that equity
    // at a turn node is a single dot product.
    for (size_t player = 0; player < 2; ++player) {
        const std::vector<Hole>& mine = private_cards_[player];
        const size_t player_len = mine.size();
        const size_t opponent_len = private_cards_[player ^ 1].size();

        std::vector<std::vector<std::vector<double>>> buf(52);
        parallel_for_range(0, 52, [&](size_t turn) {
            const uint64_t bit_turn = uint64_t{1} << turn;
            if ((bit_turn & (flop_mask | skip_turn_mask)) != 0) return;
            if (card_config_.turn != NOT_DEALT && card_config_.turn != turn) return;

            std::vector<std::vector<double>> outer;
            outer.reserve(player_len);
            for (const Hole& h : mine) {
                if ((hole_mask(h) & bit_turn) != 0) outer.emplace_back();
                else outer.emplace_back(opponent_len, 0.0);
            }

            std::vector<Card> children;
            children.reserve(48);
            const std::vector<uint8_t>& iso_ref = iso_.ref_river[turn];
            const std::vector<Card>& iso_card = iso_.card_river[turn & 3];
            const std::array<SwapList, 4>& iso_swap = iso_.swap_river[turn & 3];

            for (Card river = 0; river < 52; ++river) {
                const uint64_t bit_river = uint64_t{1} << river;
                if ((bit_river & (flop_mask | bit_turn)) != 0) continue;

                const auto pos = std::find(iso_card.begin(), iso_card.end(), river);
                Card river_ref = river;
                const SwapList* swap = nullptr;
                if (pos != iso_card.end()) {
                    const size_t p = static_cast<size_t>(pos - iso_card.begin());
                    river_ref = children[iso_ref[p]];
                    swap = &iso_swap[river & 3];
                } else {
                    children.push_back(river);
                }

                // Map our own hand indices through the swap once, up front.
                std::vector<size_t> player_map;
                if (swap) {
                    player_map.resize(player_len);
                    std::iota(player_map.begin(), player_map.end(), size_t{0});
                    apply_swap(std::span<size_t>(player_map),
                               std::span<const Swap>((*swap)[player]));
                }

                const size_t pair_index = card_pair_to_index(static_cast<Card>(turn), river_ref);
                const std::vector<size_t>& arena_indices = bunching_num_river_[player][pair_index];
                const std::vector<uint16_t>& player_strength =
                    bunching_strength_[pair_index][player];
                const std::vector<uint16_t>& opponent_strength =
                    bunching_strength_[pair_index][player ^ 1];

                for (size_t i = 0; i < outer.size(); ++i) {
                    const size_t player_index = swap ? player_map[i] : i;
                    const size_t index = arena_indices[player_index];
                    if (index == 0) continue;
                    const uint16_t threshold = player_strength[player_index];

                    std::vector<float> nums;
                    std::vector<uint16_t> strengths;
                    const float* num_ptr = nullptr;
                    const uint16_t* str_ptr = nullptr;
                    if (swap) {
                        nums.assign(arena.begin() + static_cast<ptrdiff_t>(index),
                                    arena.begin() + static_cast<ptrdiff_t>(index + opponent_len));
                        strengths = opponent_strength;
                        apply_swap(std::span<float>(nums),
                                   std::span<const Swap>((*swap)[player ^ 1]));
                        apply_swap(std::span<uint16_t>(strengths),
                                   std::span<const Swap>((*swap)[player ^ 1]));
                        num_ptr = nums.data();
                        str_ptr = strengths.data();
                    } else {
                        num_ptr = arena.data() + index;
                        str_ptr = opponent_strength.data();
                    }

                    std::vector<double>& inner = outer[i];
                    for (size_t j = 0; j < opponent_len; ++j) {
                        // +1 when we win, -1 when we lose, 0 on a tie.
                        if (str_ptr[j] < threshold) inner[j] += static_cast<double>(num_ptr[j]);
                        else if (str_ptr[j] > threshold) inner[j] -= static_cast<double>(num_ptr[j]);
                    }
                }
            }

            const double num_possible_river =
                static_cast<double>(44 - bunching_num_dead_cards_);
            for (std::vector<double>& inner : outer)
                for (double& c : inner) c /= num_possible_river;

            buf[turn] = std::move(outer);
        });

        bunching_coef_turn_[player] = push_to_arena_f64(arena, std::move(buf));
    }

    if (card_config_.turn != NOT_DEALT) {
        bunching_arena_ = std::move(arena);
        assign_zero_weights();
        return Status::ok();
    }

    // --- flop equity coefficients: fold the turn coefficients over turns ---
    for (size_t player = 0; player < 2; ++player) {
        const size_t player_len = private_cards_[player].size();
        const size_t opponent_len = private_cards_[player ^ 1].size();

        std::vector<std::vector<double>> outer(player_len, std::vector<double>(opponent_len, 0.0));
        std::vector<Card> children;
        children.reserve(49);

        for (Card turn = 0; turn < 52; ++turn) {
            if (((uint64_t{1} << turn) & flop_mask) != 0) continue;

            const auto pos = std::find(iso_.card_turn.begin(), iso_.card_turn.end(), turn);
            Card turn_ref = turn;
            const SwapList* swap = nullptr;
            if (pos != iso_.card_turn.end()) {
                const size_t p = static_cast<size_t>(pos - iso_.card_turn.begin());
                turn_ref = children[iso_.ref_turn[p]];
                swap = &iso_.swap_turn[turn & 3];
            } else {
                children.push_back(turn);
            }

            std::vector<size_t> player_map;
            if (swap) {
                player_map.resize(player_len);
                std::iota(player_map.begin(), player_map.end(), size_t{0});
                apply_swap(std::span<size_t>(player_map), std::span<const Swap>((*swap)[player]));
            }

            const std::vector<size_t>& arena_indices = bunching_coef_turn_[player][turn_ref];

            for (size_t i = 0; i < outer.size(); ++i) {
                const size_t player_index = swap ? player_map[i] : i;
                const size_t index = arena_indices[player_index];
                if (index == 0) continue;

                std::vector<float> nums;
                const float* num_ptr = nullptr;
                if (swap) {
                    nums.assign(arena.begin() + static_cast<ptrdiff_t>(index),
                                arena.begin() + static_cast<ptrdiff_t>(index + opponent_len));
                    apply_swap(std::span<float>(nums), std::span<const Swap>((*swap)[player ^ 1]));
                    num_ptr = nums.data();
                } else {
                    num_ptr = arena.data() + index;
                }

                for (size_t j = 0; j < opponent_len; ++j)
                    outer[i][j] += static_cast<double>(num_ptr[j]);
            }
        }

        const double num_possible_turn = static_cast<double>(45 - bunching_num_dead_cards_);
        for (std::vector<double>& inner : outer)
            for (double& c : inner) c /= num_possible_turn;

        std::vector<std::vector<std::vector<double>>> one;
        one.push_back(std::move(outer));
        std::vector<std::vector<size_t>> pushed = push_to_arena_f64(arena, std::move(one));
        bunching_coef_flop_[player] = std::move(pushed[0]);
    }

    bunching_arena_ = std::move(arena);
    assign_zero_weights();
    return Status::ok();
}

// --------------------------------------------------------------------------
// Terminal evaluation with bunching: an explicit O(#oop * #ip) dot product per
// hand instead of the inclusion-exclusion sweeps.
// --------------------------------------------------------------------------

void PostFlopGame::evaluate_internal_bunching(std::span<float> result, const PostFlopNode& node,
                                             size_t player, std::span<const float> cfreach) {
    const double pot = static_cast<double>(tree_config_.starting_pot + 2 * node.amount());
    const double half_pot = 0.5 * pot;
    const double rake = fmin_raw(pot * tree_config_.rake_rate, tree_config_.rake_cap);
    const float amount_win = static_cast<float>((half_pot - rake) / bunching_num_combinations_);
    const float amount_lose = static_cast<float>(-half_pot / bunching_num_combinations_);
    const float amount_tie = static_cast<float>(-0.5 * rake / bunching_num_combinations_);
    const size_t opponent_len = private_cards_[player ^ 1].size();

    if ((node.player() & PLAYER_FOLD_FLAG) == PLAYER_FOLD_FLAG) {
        const uint8_t folded_player = static_cast<uint8_t>(node.player() & PLAYER_MASK);
        const float payoff = (folded_player != player) ? amount_win : amount_lose;

        const std::vector<size_t>* indices = nullptr;
        if (node.river() != NOT_DEALT)
            indices = &bunching_num_river_[player][card_pair_to_index(node.turn(), node.river())];
        else if (node.turn() != NOT_DEALT)
            indices = &bunching_num_turn_[player][node.turn()];
        else
            indices = &bunching_num_flop_[player];

        for (size_t i = 0; i < result.size(); ++i) {
            const size_t index = (*indices)[i];
            result[i] = index != 0
                            ? payoff * inner_product(cfreach,
                                                     std::span<const float>(
                                                         bunching_arena_.data() + index,
                                                         opponent_len))
                            : 0.0f;
        }
        return;
    }

    const size_t pair_index = card_pair_to_index(node.turn(), node.river());
    const std::vector<size_t>& indices = bunching_num_river_[player][pair_index];
    const std::vector<uint16_t>& player_strength = bunching_strength_[pair_index][player];
    const std::vector<uint16_t>& opponent_strength = bunching_strength_[pair_index][player ^ 1];

    for (size_t i = 0; i < result.size(); ++i) {
        const size_t index = indices[i];
        result[i] = index != 0
                        ? inner_product_cond(
                              cfreach,
                              std::span<const float>(bunching_arena_.data() + index, opponent_len),
                              opponent_strength, player_strength[i], amount_win, amount_lose,
                              amount_tie)
                        : 0.0f;
    }
}

std::vector<float> PostFlopGame::equity_internal_bunching(size_t player) const {
    std::vector<float> weights_buf;
    const std::vector<float>* opponent_weights = &weights_[player ^ 1];
    if (turn_swap_ || river_swap_) {
        weights_buf = weights_[player ^ 1];
        apply_swap_to(weights_buf, player ^ 1, true);
        opponent_weights = &weights_buf;
    }

    const PostFlopNode& node = current_node();
    const size_t opponent_len = opponent_weights->size();

    if (node.river() == NOT_DEALT) {
        const std::vector<size_t>& indices = node.turn() != NOT_DEALT
                                                 ? bunching_coef_turn_[player][node.turn()]
                                                 : bunching_coef_flop_[player];
        std::vector<float> out(indices.size(), 0.0f);
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t index = indices[i];
            if (index == 0) continue;
            out[i] = 0.5f * inner_product(*opponent_weights,
                                          std::span<const float>(
                                              bunching_arena_.data() + index, opponent_len));
        }
        return out;
    }

    const size_t pair_index = card_pair_to_index(node.turn(), node.river());
    const std::vector<size_t>& indices = bunching_num_river_[player][pair_index];
    const std::vector<uint16_t>& player_strength = bunching_strength_[pair_index][player];
    const std::vector<uint16_t>& opponent_strength = bunching_strength_[pair_index][player ^ 1];

    std::vector<float> out(indices.size(), 0.0f);
    for (size_t i = 0; i < indices.size(); ++i) {
        const size_t index = indices[i];
        if (index == 0) continue;
        out[i] = inner_product_cond(
            *opponent_weights,
            std::span<const float>(bunching_arena_.data() + index, opponent_len),
            opponent_strength, player_strength[i], 0.5f, -0.5f, 0.0f);
    }
    return out;
}

}  // namespace pfs
