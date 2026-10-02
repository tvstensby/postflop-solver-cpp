// Port of src/utility.rs -- finalize, exploitability, and the two tree walks
// that accompany the solver's own.
//
// There are three near-identical recursions in the Rust (solve_recursive here in
// solver.hpp, plus the two below) and they have ALREADY drifted from each other
// in three observable ways. The drift is preserved deliberately:
//
//                              | solve      | cfvalue          | best_cfv
//   top-level 1-action shortcut| yes        | NO (opponent only)| yes
//   num_hands                  | result.size| result.size      | num_private_hands(player)
//   strategy source            | regrets    | strategy         | strategy (max at own node)
//
// Unifying them behind a policy template would hide exactly the parts most likely
// to go subtly wrong, so they stay separate.
#pragma once

#include <pfs/interface.hpp>

#include "core/arena.hpp"
#include "core/encoding.hpp"
#include "core/sliceop.hpp"
#include "core/thread_pool.hpp"

#include <array>
#include <span>
#include <vector>

namespace pfs {

// Weighted average, accumulated in double. Public API.
inline float compute_average(std::span<const float> slice, std::span<const float> weights) {
    double value_sum = 0.0;
    double weight_sum = 0.0;
    const size_t n = slice.size();
    for (size_t i = 0; i < n; ++i) {
        value_sum += static_cast<double>(slice[i]) * static_cast<double>(weights[i]);
        weight_sum += static_cast<double>(weights[i]);
    }
    return static_cast<float>(value_sum / weight_sum);
}

namespace detail {

// Shared by both walks below and by the solver: fold the child rows in double,
// then add each isomorphic chance's row through its swap list.
template <GameLike G>
void reduce_chance_rows(G& game, typename G::Node& node, size_t player, std::span<float> result,
                        std::span<float> cfv_actions, size_t num_hands, ScratchScope& scratch) {
    std::span<double> result_f64 = scratch.doubles(num_hands);
    sum_slices_f64_into(result_f64, std::span<const float>(cfv_actions));

    const std::span<const uint8_t> iso = game.isomorphic_chances(node);
    for (size_t i = 0; i < iso.size(); ++i) {
        const std::span<const Swap> swap_list = game.isomorphic_swap(node, i)[player];
        std::span<float> tmp = row_mut(cfv_actions, iso[i], num_hands);

        apply_swap(tmp, swap_list);
        for (size_t j = 0; j < num_hands; ++j) result_f64[j] += static_cast<double>(tmp[j]);
        apply_swap(tmp, swap_list);  // involution: restores the buffer
    }

    for (size_t j = 0; j < num_hands; ++j) result[j] = static_cast<float>(result_f64[j]);
}

// Builds the opponent's reach-weighted per-action rows: normalized average
// strategy, node locking applied, then each row multiplied by cfreach.
template <GameLike G>
std::span<float> opponent_reach_rows(G& game, typename G::Node& node, size_t num_actions,
                                     std::span<const float> cfreach, ScratchScope& scratch) {
    std::span<float> rows =
        game.is_compression_enabled()
            ? normalized_strategy<uint16_t>(
                  scratch, std::span<const uint16_t>(node.strategy_compressed()), num_actions)
            : normalized_strategy<float>(scratch, std::span<const float>(node.strategy()),
                                         num_actions);

    apply_locking_strategy(rows, game.locking_strategy(node));

    const size_t row_size = cfreach.size();
    for (size_t a = 0; a < num_actions; ++a) mul_slice(row_mut(rows, a, row_size), cfreach);
    return rows;
}

// --------------------------------------------------------------------------
// compute_cfvalue_recursive -- the average strategy's counterfactual values,
// optionally written back into the node storage.
// --------------------------------------------------------------------------
template <GameLike G>
void compute_cfvalue_recursive(std::span<float> result, G& game, typename G::Node& node,
                               size_t player, std::span<const float> cfreach,
                               bool save_cfvalues) {
    if (node.is_terminal()) {
        game.evaluate(result, node, player, cfreach);
        return;
    }

    const size_t num_actions = node.num_actions();
    const size_t num_hands = result.size();

    // NOTE: unlike the other two walks there is deliberately NO top-level
    // single-action shortcut here; it lives in the opponent branch only, so that
    // the IP-cfvalue save at the bottom still runs on a pass-through node.
    ScratchScope scratch;
    std::span<float> cfv_actions = scratch.floats(num_actions * num_hands);

    if (node.is_chance()) {
        std::span<float> cfreach_updated = scratch.floats(cfreach.size());
        mul_slice_scalar_into(cfreach_updated, cfreach,
                              1.0f / static_cast<float>(game.chance_factor(node)));

        for_each_child(node, [&](size_t action) {
            compute_cfvalue_recursive(row_mut(cfv_actions, action, num_hands), game,
                                      node.play(action), player,
                                      std::span<const float>(cfreach_updated), save_cfvalues);
        });

        reduce_chance_rows(game, node, player, result, cfv_actions, num_hands, scratch);

        if (save_cfvalues && node.cfvalue_storage_player() == std::optional<size_t>(player)) {
            if (game.is_compression_enabled()) {
                node.cfvalue_chance_scale() = encode_signed_slice(
                    node.cfvalues_chance_compressed(), std::span<const float>(result));
            } else {
                const std::span<float> dst = node.cfvalues_chance();
                for (size_t i = 0; i < dst.size(); ++i) dst[i] = result[i];
            }
        }
    } else if (node.player() == player) {
        for_each_child(node, [&](size_t action) {
            compute_cfvalue_recursive(row_mut(cfv_actions, action, num_hands), game,
                                      node.play(action), player, cfreach, save_cfvalues);
        });

        std::span<float> strategy =
            game.is_compression_enabled()
                ? normalized_strategy<uint16_t>(
                      scratch, std::span<const uint16_t>(node.strategy_compressed()), num_actions)
                : normalized_strategy<float>(scratch, std::span<const float>(node.strategy()),
                                             num_actions);
        apply_locking_strategy(strategy, game.locking_strategy(node));

        fma_slices_into(result, std::span<const float>(strategy),
                        std::span<const float>(cfv_actions));

        if (save_cfvalues) {
            if (game.is_compression_enabled()) {
                node.cfvalue_scale() = encode_signed_slice(node.cfvalues_compressed(),
                                                           std::span<const float>(cfv_actions));
            } else {
                const std::span<float> dst = node.cfvalues();
                for (size_t i = 0; i < dst.size(); ++i) dst[i] = cfv_actions[i];
            }
        }
    } else if (num_actions == 1) {
        compute_cfvalue_recursive(result, game, node.play(0), player, cfreach, save_cfvalues);
    } else {
        std::span<float> cfreach_actions =
            opponent_reach_rows(game, node, num_actions, cfreach, scratch);
        const size_t row_size = cfreach.size();

        for_each_child(node, [&](size_t action) {
            compute_cfvalue_recursive(row_mut(cfv_actions, action, num_hands), game,
                                      node.play(action), player,
                                      row(std::span<const float>(cfreach_actions), action, row_size),
                                      save_cfvalues);
        });

        sum_slices_into(result, std::span<const float>(cfv_actions));
    }

    // IP's counterfactual values are stored only at the first decision node of a
    // street, and only for player 1.
    if (save_cfvalues && node.has_cfvalues_ip() && player == 1) {
        if (game.is_compression_enabled()) {
            node.cfvalue_ip_scale() = encode_signed_slice(node.cfvalues_ip_compressed(),
                                                          std::span<const float>(result));
        } else {
            const std::span<float> dst = node.cfvalues_ip();
            for (size_t i = 0; i < dst.size(); ++i) dst[i] = result[i];
        }
    }
}

// --------------------------------------------------------------------------
// compute_best_cfv_recursive -- the best response walk.
// --------------------------------------------------------------------------
template <GameLike G>
void compute_best_cfv_recursive(std::span<float> result, G& game, typename G::Node& node,
                                size_t player, std::span<const float> cfreach) {
    if (node.is_terminal()) {
        game.evaluate(result, node, player, cfreach);
        return;
    }

    const size_t num_actions = node.num_actions();
    // Note: from the game, not from result.size().
    const size_t num_hands = game.num_private_hands(player);

    if (num_actions == 1 && !node.is_chance()) {
        compute_best_cfv_recursive(result, game, node.play(0), player, cfreach);
        return;
    }

    ScratchScope scratch;
    std::span<float> cfv_actions = scratch.floats(num_actions * num_hands);

    if (node.is_chance()) {
        std::span<float> cfreach_updated = scratch.floats(cfreach.size());
        mul_slice_scalar_into(cfreach_updated, cfreach,
                              1.0f / static_cast<float>(game.chance_factor(node)));

        for_each_child(node, [&](size_t action) {
            compute_best_cfv_recursive(row_mut(cfv_actions, action, num_hands), game,
                                       node.play(action), player,
                                       std::span<const float>(cfreach_updated));
        });

        reduce_chance_rows(game, node, player, result, cfv_actions, num_hands, scratch);
    } else if (node.player() == player) {
        for_each_child(node, [&](size_t action) {
            compute_best_cfv_recursive(row_mut(cfv_actions, action, num_hands), game,
                                       node.play(action), player, cfreach);
        });

        const std::span<const float> locking = game.locking_strategy(node);
        if (locking.empty()) {
            max_slices_into(result, std::span<const float>(cfv_actions));
        } else {
            max_fma_slices_into(result, std::span<const float>(cfv_actions), locking);
        }
    } else {
        std::span<float> cfreach_actions =
            opponent_reach_rows(game, node, num_actions, cfreach, scratch);
        const size_t row_size = cfreach.size();

        for_each_child(node, [&](size_t action) {
            compute_best_cfv_recursive(
                row_mut(cfv_actions, action, num_hands), game, node.play(action), player,
                row(std::span<const float>(cfreach_actions), action, row_size));
        });

        sum_slices_into(result, std::span<const float>(cfv_actions));
    }
}

}  // namespace detail

// The bias, (starting pot) / 2, is already subtracted from the returned EVs,
// which makes them zero-sum when the game is not raked.
template <GameLike G>
std::array<float, 2> compute_current_ev(G& game) {
    if (!game.is_ready() && !game.is_solved()) PFS_PANIC("Game is not ready");

    std::array<std::vector<float>, 2> cfvalues{
        std::vector<float>(game.num_private_hands(0)),
        std::vector<float>(game.num_private_hands(1))};
    const std::array<std::span<const float>, 2> reach{game.initial_weights(0),
                                                      game.initial_weights(1)};

    for (size_t player = 0; player < 2; ++player)
        detail::compute_cfvalue_recursive(cfvalues[player], game, game.root(), player,
                                          reach[player ^ 1], false);

    return {weighted_sum(cfvalues[0], reach[0]), weighted_sum(cfvalues[1], reach[1])};
}

// Expected values of the maximally exploitative strategy.
template <GameLike G>
std::array<float, 2> compute_mes_ev(G& game) {
    if (!game.is_ready() && !game.is_solved()) PFS_PANIC("Game is not ready");

    std::array<std::vector<float>, 2> cfvalues{
        std::vector<float>(game.num_private_hands(0)),
        std::vector<float>(game.num_private_hands(1))};
    const std::array<std::span<const float>, 2> reach{game.initial_weights(0),
                                                      game.initial_weights(1)};

    for (size_t player = 0; player < 2; ++player)
        detail::compute_best_cfv_recursive(cfvalues[player], game, game.root(), player,
                                           reach[player ^ 1]);

    return {weighted_sum(cfvalues[0], reach[0]), weighted_sum(cfvalues[1], reach[1])};
}

template <GameLike G>
float compute_exploitability(G& game) {
    if (!game.is_ready() && !game.is_solved()) PFS_PANIC("Game is not ready");

    const std::array<float, 2> mes_ev = compute_mes_ev(game);
    if (!game.is_raked()) return (mes_ev[0] + mes_ev[1]) * 0.5f;

    const std::array<float, 2> current_ev = compute_current_ev(game);
    return ((mes_ev[0] - current_ev[0]) + (mes_ev[1] - current_ev[1])) * 0.5f;
}

// Computes and stores the counterfactual values of the average strategy, then
// marks the game solved and releases the per-thread arenas.
template <GameLike G>
void finalize(G& game) {
    if (game.is_solved()) PFS_PANIC("Game is already solved");
    if (!game.is_ready()) PFS_PANIC("Game is not ready");

    for (size_t player = 0; player < 2; ++player) {
        std::vector<float> cfvalues(game.num_private_hands(player));
        detail::compute_cfvalue_recursive(cfvalues, game, game.root(), player,
                                          game.initial_weights(player ^ 1), true);
    }

    game.set_solved();

#if PFS_CUSTOM_ALLOC
    ThreadPool::global().broadcast([] { Arena::local().release(); });
#endif
}

}  // namespace pfs
