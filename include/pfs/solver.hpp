// Port of src/solver.rs -- Discounted CFR.
#pragma once

#include <pfs/interface.hpp>
#include <pfs/utility.hpp>

#include "core/arena.hpp"
#include "core/encoding.hpp"
#include "core/sliceop.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace pfs {

// Discounted CFR coefficients. alpha = 1.5, beta = 0.0 (so beta_t is the fixed
// 0.5), gamma = 3.0 -- the crate uses 3.0 rather than the paper's 2.0. The
// cumulative strategy is reset whenever the iteration count reaches a power of 4,
// which is what t_gamma restarting from nearest_lower_power_of_4 achieves.
struct DiscountParams {
    float alpha_t;
    float beta_t;
    float gamma_t;

    explicit DiscountParams(uint32_t current_iteration) {
        // 0, 1, 4, 16, 64, 256, ...
        uint32_t nearest_lower_power_of_4 = 0;
        if (current_iteration != 0) {
            // Rust: 1 << ((x.leading_zeros() ^ 31) & !1)
            const int msb = 31 - static_cast<int>(std::countl_zero(current_iteration));
            nearest_lower_power_of_4 = 1u << (static_cast<unsigned>(msb) & ~1u);
        }

        const double t_alpha =
            static_cast<double>(std::max(static_cast<int32_t>(current_iteration) - 1, 0));
        const double t_gamma = static_cast<double>(current_iteration - nearest_lower_power_of_4);

        const double pow_alpha = t_alpha * std::sqrt(t_alpha);       // t^1.5
        const double r = t_gamma / (t_gamma + 1.0);
        const double pow_gamma = r * r * r;                          // gamma = 3

        alpha_t = static_cast<float>(pow_alpha / (pow_alpha + 1.0));
        beta_t = 0.5f;
        gamma_t = static_cast<float>(pow_gamma);
    }
};

namespace detail {

template <GameLike G>
void solve_recursive(std::span<float> result, G& game, typename G::Node& node, size_t player,
                     std::span<const float> cfreach, const DiscountParams& params) {
    if (node.is_terminal()) {
        game.evaluate(result, node, player, cfreach);
        return;
    }

    const size_t num_actions = node.num_actions();
    const size_t num_hands = result.size();

    if (num_actions == 1 && !node.is_chance()) {
        solve_recursive(result, game, node.play(0), player, cfreach, params);
        return;
    }

    ScratchScope scratch;
    std::span<float> cfv_actions = scratch.floats(num_actions * num_hands);

    if (node.is_chance()) {
        std::span<float> cfreach_updated = scratch.floats(cfreach.size());
        mul_slice_scalar_into(cfreach_updated, cfreach,
                              1.0f / static_cast<float>(game.chance_factor(node)));

        for_each_child(node, [&](size_t action) {
            solve_recursive(row_mut(cfv_actions, action, num_hands), game, node.play(action),
                            player, std::span<const float>(cfreach_updated), params);
        });

        reduce_chance_rows(game, node, player, result, cfv_actions, num_hands, scratch);
    } else if (node.player() == player) {
        for_each_child(node, [&](size_t action) {
            solve_recursive(row_mut(cfv_actions, action, num_hands), game, node.play(action),
                            player, cfreach, params);
        });

        // Strategy by regret matching -- from the REGRETS, unlike the two walks
        // in utility.hpp which use the cumulative strategy.
        std::span<float> strategy =
            game.is_compression_enabled()
                ? regret_matching<int16_t>(
                      scratch, std::span<const int16_t>(node.regrets_compressed()), num_actions)
                : regret_matching<float>(scratch, std::span<const float>(node.regrets()),
                                         num_actions);

        const std::span<const float> locking = game.locking_strategy(node);
        apply_locking_strategy(strategy, locking);

        fma_slices_into(result, std::span<const float>(strategy),
                        std::span<const float>(cfv_actions));

        if (game.is_compression_enabled()) {
            // --- cumulative strategy ---
            {
                const float scale = node.strategy_scale();
                const float decoder = params.gamma_t * scale / 65535.0f;
                const std::span<uint16_t> cum_strategy = node.strategy_compressed();
                for (size_t i = 0; i < strategy.size(); ++i)
                    strategy[i] += static_cast<float>(cum_strategy[i]) * decoder;

                if (!locking.empty())
                    for (size_t i = 0; i < strategy.size() && i < locking.size(); ++i)
                        if (is_sign_positive(locking[i])) strategy[i] = 0.0f;

                node.strategy_scale() =
                    encode_unsigned_slice(cum_strategy, std::span<const float>(strategy));
            }
            // --- cumulative regret ---
            {
                const float scale = node.regret_scale();
                const float alpha_decoder = params.alpha_t * scale / 32767.0f;
                const float beta_decoder = params.beta_t * scale / 32767.0f;
                const std::span<int16_t> cum_regret = node.regrets_compressed();

                for (size_t i = 0; i < cfv_actions.size(); ++i)
                    cfv_actions[i] += static_cast<float>(cum_regret[i]) *
                                      (cum_regret[i] >= 0 ? alpha_decoder : beta_decoder);

                for (size_t a = 0; a < num_actions; ++a)
                    sub_slice(row_mut(cfv_actions, a, num_hands),
                              std::span<const float>(result));

                if (!locking.empty())
                    for (size_t i = 0; i < cfv_actions.size() && i < locking.size(); ++i)
                        if (is_sign_positive(locking[i])) cfv_actions[i] = 0.0f;

                node.regret_scale() =
                    encode_signed_slice(cum_regret, std::span<const float>(cfv_actions));
            }
        } else {
            // --- cumulative strategy ---
            const float gamma = params.gamma_t;
            const std::span<float> cum_strategy = node.strategy();
            for (size_t i = 0; i < cum_strategy.size(); ++i)
                cum_strategy[i] = cum_strategy[i] * gamma + strategy[i];

            // --- cumulative regret ---
            const float alpha = params.alpha_t;
            const float beta = params.beta_t;
            const std::span<float> cum_regret = node.regrets();
            for (size_t i = 0; i < cum_regret.size(); ++i) {
                const float coef = is_sign_positive(cum_regret[i]) ? alpha : beta;
                cum_regret[i] = cum_regret[i] * coef + cfv_actions[i];
            }
            for (size_t a = 0; a < num_actions; ++a)
                sub_slice(row_mut(cum_regret, a, num_hands), std::span<const float>(result));
        }
    } else {
        std::span<float> cfreach_actions =
            game.is_compression_enabled()
                ? regret_matching<int16_t>(
                      scratch, std::span<const int16_t>(node.regrets_compressed()), num_actions)
                : regret_matching<float>(scratch, std::span<const float>(node.regrets()),
                                         num_actions);

        apply_locking_strategy(cfreach_actions, game.locking_strategy(node));

        const size_t row_size = cfreach.size();
        for (size_t a = 0; a < num_actions; ++a)
            mul_slice(row_mut(cfreach_actions, a, row_size), cfreach);

        for_each_child(node, [&](size_t action) {
            solve_recursive(row_mut(cfv_actions, action, num_hands), game, node.play(action),
                            player, row(std::span<const float>(cfreach_actions), action, row_size),
                            params);
        });

        sum_slices_into(result, std::span<const float>(cfv_actions));
    }
}

}  // namespace detail

// Runs one Discounted CFR iteration: alternating updates for both players.
template <GameLike G>
void solve_step(G& game, uint32_t current_iteration) {
    if (game.is_solved()) PFS_PANIC("Game is already solved");
    if (!game.is_ready()) PFS_PANIC("Game is not ready");

    const DiscountParams params(current_iteration);
    for (size_t player = 0; player < 2; ++player) {
        std::vector<float> result(game.num_private_hands(player));
        detail::solve_recursive(result, game, game.root(), player,
                                game.initial_weights(player ^ 1), params);
    }
}

// Runs Discounted CFR until the iteration limit or the target exploitability,
// then finalizes. Returns the exploitability of the resulting strategy.
template <GameLike G>
float solve(G& game, uint32_t max_num_iterations, float target_exploitability,
            bool print_progress) {
    if (game.is_solved()) PFS_PANIC("Game is already solved");
    if (!game.is_ready()) PFS_PANIC("Game is not ready");

    float exploitability = compute_exploitability(game);

    if (print_progress) {
        std::printf("iteration: 0 / %u (exploitability = %.4e)", max_num_iterations,
                    static_cast<double>(exploitability));
        std::fflush(stdout);
    }

    for (uint32_t t = 0; t < max_num_iterations; ++t) {
        if (exploitability <= target_exploitability) break;

        const DiscountParams params(t);
        for (size_t player = 0; player < 2; ++player) {
            std::vector<float> result(game.num_private_hands(player));
            detail::solve_recursive(result, game, game.root(), player,
                                    game.initial_weights(player ^ 1), params);
        }

        if ((t + 1) % 10 == 0 || t + 1 == max_num_iterations)
            exploitability = compute_exploitability(game);

        if (print_progress) {
            std::printf("\riteration: %u / %u (exploitability = %.4e)", t + 1, max_num_iterations,
                        static_cast<double>(exploitability));
            std::fflush(stdout);
        }
    }

    if (print_progress) {
        std::printf("\n");
        std::fflush(stdout);
    }

    finalize(game);
    return exploitability;
}

}  // namespace pfs
