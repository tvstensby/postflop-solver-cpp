// The per-player ICM risk premium: it weights a player's losing outcomes by their bubble factor,
// (1 + 2r) / (1 - 2r), which makes the game non-zero-sum.
//
// As in test_game.cpp, the closed-form cases call allocate_memory + finalize WITHOUT solving and
// read a terminal node, where the value comes from the terminal evaluation alone and is exact.
#include "harness.hpp"

#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pfs;

namespace {

// OOP cannot make an ace, IP holds the last two, so on an ace-ace board IP always wins.
const char* const kLosingRange = "KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+";

CardConfig cards(const char* oop, const char* ip, const char* flop, const char* turn = nullptr,
                 const char* river = nullptr) {
    CardConfig c;
    c.range[0] = Range::parse(oop).value();
    c.range[1] = Range::parse(ip).value();
    c.flop = flop_from_str(flop).value();
    if (turn) c.turn = card_from_str(turn).value();
    if (river) c.river = card_from_str(river).value();
    return c;
}

std::unique_ptr<PostFlopGame> build(CardConfig cc, TreeConfig tc) {
    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    if (!tree.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, "tree: " + tree.error());
        return nullptr;
    }
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    if (!g.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, "game: " + g.error());
        return nullptr;
    }
    return std::move(g.value());
}

// The weight a lost chip gains, BF - 1. The library computes this itself; spelling it out again
// here keeps the expected values independent of the implementation.
double loss_extra(double premium) { return 4.0 * premium / (1.0 - 2.0 * premium); }

float average_ev(PostFlopGame& game, size_t player) {
    game.cache_normalized_weights();
    return compute_average(game.expected_values(player), game.normalized_weights(player));
}

// The frequency of one action over the whole range, as rangeFrequencies in the result file.
double action_frequency(PostFlopGame& game, size_t action) {
    game.cache_normalized_weights();
    const size_t player = game.current_player();
    const std::vector<float> strategy = game.strategy();
    const std::span<const float> weights = game.normalized_weights(player);
    const size_t num_hands = weights.size();
    double sum = 0.0;
    double total = 0.0;
    for (size_t h = 0; h < num_hands; ++h) {
        sum += static_cast<double>(weights[h]) * strategy[action * num_hands + h];
        total += weights[h];
    }
    return total > 0.0 ? sum / total : 0.0;
}

// A river game where OOP may bet half the pot and IP may only fold or call.
TreeConfig river_tree(double premium_oop, double premium_ip) {
    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    tc.risk_premium = {premium_oop, premium_ip};
    return tc;
}

}  // namespace

// OOP bets 30 and IP calls, so the pot is 120 and OOP loses the showdown. Without a premium OOP
// takes home nothing; with one, the 30 chips OOP put in are weighted by the bubble factor, so the
// risk-adjusted value is -30 * (BF - 1). IP wins, so IP's own payoff is untouched.
PFS_TEST(risk_premium, weights_the_losing_payoff) {
    for (const double premium : {0.0, 0.02, 0.1, 0.25}) {
        auto game = build(cards(kLosingRange, "AA", "AcAdKh", "Qc", "7s"), river_tree(premium, 0.0));
        if (!game) return;
        game->allocate_memory(false);
        finalize(*game);

        game->play(1);  // OOP bets 30
        game->play(1);  // IP calls
        CHECK(game->is_terminal_node());
        CHECK_NEAR(average_ev(*game, 0), -30.0 * loss_extra(premium), 1e-3);
        CHECK_NEAR(average_ev(*game, 1), 120.0, 1e-3);
    }
}

// The premium is per player: the same spot with the premium on the winning player instead leaves
// both values at their chip EV, because a premium only ever touches that player's losses.
PFS_TEST(risk_premium, only_the_premium_players_losses_change) {
    auto game = build(cards(kLosingRange, "AA", "AcAdKh", "Qc", "7s"), river_tree(0.0, 0.25));
    if (!game) return;
    game->allocate_memory(false);
    finalize(*game);

    game->play(1);
    game->play(1);
    CHECK(game->is_terminal_node());
    CHECK_NEAR(average_ev(*game, 0), 0.0, 1e-3);
    CHECK_NEAR(average_ev(*game, 1), 120.0, 1e-3);
}

// A fold forfeits the chips already committed, so with a premium its reported EV is no longer 0.
// Here OOP has 30 matched from the turn and folds the river, which is worth -30 * (BF - 1).
PFS_TEST(risk_premium, fold_costs_the_premium_on_committed_chips) {
    for (const double premium : {0.0, 0.1}) {
        TreeConfig tc;
        tc.initial_state = BoardState::Turn;
        tc.starting_pot = 60;
        tc.effective_stack = 970;
        tc.turn_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
        tc.river_bet_sizes = {BetSizeOptions{}, BetSizeOptions::parse("50%", "").value()};
        tc.risk_premium = {premium, 0.0};
        auto game = build(cards(kLosingRange, "AA", "AcAdKh", "Qc"), std::move(tc));
        if (!game) return;
        game->allocate_memory(false);
        finalize(*game);

        game->play(1);          // OOP bets 30 on the turn
        game->play(1);          // IP calls
        game->play(SIZE_MAX);   // the lowest river card
        game->play(0);          // OOP checks
        game->play(1);          // IP bets 60
        const std::vector<Action> actions = game->available_actions();
        CHECK_EQ(actions.size(), size_t{2});
        CHECK(actions[0].kind == ActionKind::Fold);

        game->cache_normalized_weights();
        const std::vector<float> detail = game->expected_values_detail(0);
        const std::span<const float> normalized = game->normalized_weights(0);
        const size_t num_hands = normalized.size();
        CHECK_EQ(detail.size(), 2 * num_hands);
        size_t checked = 0;
        for (size_t h = 0; h < num_hands; ++h) {
            if (normalized[h] <= 0.0f) continue;
            CHECK_NEAR(detail[h], -30.0 * loss_extra(premium), 1e-3);
            ++checked;
        }
        CHECK(checked > 0);
    }
}

// A premium makes the game non-zero-sum, so exploitability has to be measured against the current
// strategy's values rather than from the best responses alone.
PFS_TEST(risk_premium, breaks_zero_sum) {
    auto plain = build(cards("AA,KK,QQ", "AA,KK,JJ", "Td9d6h", "Qc", "7s"), river_tree(0.0, 0.0));
    auto averse = build(cards("AA,KK,QQ", "AA,KK,JJ", "Td9d6h", "Qc", "7s"), river_tree(0.1, 0.1));
    if (!plain || !averse) return;
    CHECK(plain->is_zero_sum());
    CHECK(!averse->is_zero_sum());
    CHECK(!averse->is_raked());  // the premium alone is enough to break it
    CHECK(averse->has_risk_premium());

    for (PostFlopGame* game : {plain.get(), averse.get()}) {
        game->allocate_memory(false);
        solve(*game, 200, 0.0f, false);
        CHECK(compute_exploitability(*game) >= 0.0f);
    }
    // Centered EVs cancel without a premium and are both negative with one: each player pays for
    // the risk they carry.
    const std::array<float, 2> plain_ev = compute_current_ev(*plain);
    const std::array<float, 2> averse_ev = compute_current_ev(*averse);
    CHECK_NEAR(plain_ev[0] + plain_ev[1], 0.0, 1e-3);
    CHECK(averse_ev[0] + averse_ev[1] < -1.0f);
}

// The point of the feature: a risk-averse player needs more than pot odds to call an all-in, so
// they call less often. IP is locked to always shove, which leaves OOP a pure pot-odds decision
// against a known range -- calling 60 to win 120 needs a third of the equity at chip EV, and
// BF / (2 + BF) with a premium, so the calling range can only tighten.
PFS_TEST(risk_premium, raises_the_bar_for_calling_an_all_in) {
    double previous = 2.0;
    double without_premium = 0.0;
    for (const double premium : {0.0, 0.05, 0.15, 0.3}) {
        TreeConfig tc;
        tc.initial_state = BoardState::River;
        tc.starting_pot = 60;
        tc.effective_stack = 60;
        tc.river_bet_sizes = {BetSizeOptions{}, BetSizeOptions::parse("a", "").value()};
        tc.risk_premium = {premium, 0.0};
        auto game = build(cards("QQ-22", "AA,KK,55-22", "Td9d6h", "Qc", "7s"), std::move(tc));
        if (!game) return;
        game->allocate_memory(false);

        game->play(0);  // OOP checks
        CHECK_EQ(game->current_player(), size_t{1});
        CHECK_EQ(game->available_actions().size(), size_t{2});
        const size_t ip_hands = game->num_private_hands(1);
        std::vector<float> always_shove(2 * ip_hands, 0.0f);
        for (size_t h = 0; h < ip_hands; ++h) always_shove[ip_hands + h] = 1.0f;
        game->lock_current_strategy(always_shove);
        game->back_to_root();
        solve(*game, 500, 0.0f, false);

        game->play(0);  // OOP checks
        game->play(1);  // IP shoves
        const std::vector<Action> actions = game->available_actions();
        CHECK_EQ(actions.size(), size_t{2});
        CHECK(actions[1].kind == ActionKind::Call);
        const double call = action_frequency(*game, 1);
        CHECK(call <= previous + 1e-4);
        CHECK(call > 0.0);  // the spot stays playable, so the trend is meaningful
        previous = call;
        if (premium == 0.0) without_premium = call;
    }
    // Not merely non-increasing: on this board the calling range goes from about 78% of the
    // range to about 50%. A premium that stopped reaching the call/fold decision would leave
    // these equal and pass the monotonicity checks above.
    CHECK(without_premium - previous > 0.1);
}

// expected_values is the strategy-weighted average of expected_values_detail. That has to keep
// holding once a premium makes the fold row non-zero, or the reported EV would no longer be the
// quantity the solver optimized.
PFS_TEST(risk_premium, reported_ev_matches_the_per_action_detail) {
    // Raises are allowed, so OOP can face one with its own bet already matched. A player facing
    // the first bet of a street has nothing committed, and folding is then genuinely free.
    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "2.5x").value(),
                          BetSizeOptions::parse("50%", "2.5x").value()};
    tc.risk_premium = {0.08, 0.08};
    auto game = build(cards("AA,KK,QQ,JJ", "AA,KK,QQ,TT", "Td9d6h", "Qc", "7s"), std::move(tc));
    if (!game) return;
    game->allocate_memory(false);
    solve(*game, 200, 0.0f, false);

    game->play(1);  // OOP bets 30
    CHECK_EQ(game->current_player(), size_t{1});
    const std::vector<Action> ip_actions = game->available_actions();
    CHECK_EQ(ip_actions.size(), size_t{3});
    CHECK(ip_actions[2].kind == ActionKind::Raise);
    game->play(2);  // IP raises, so OOP faces it with 30 already matched
    CHECK_EQ(game->current_player(), size_t{0});
    CHECK(game->available_actions()[0].kind == ActionKind::Fold);
    game->cache_normalized_weights();

    const std::vector<float> detail = game->expected_values_detail(0);
    const std::vector<float> ev = game->expected_values(0);
    const std::vector<float> strategy = game->strategy();
    const size_t num_hands = game->num_private_hands(0);
    const size_t num_actions = detail.size() / num_hands;
    CHECK_EQ(ev.size(), num_hands);

    bool any_fold_row_charged = false;
    for (size_t h = 0; h < num_hands; ++h) {
        double mixed = 0.0;
        for (size_t a = 0; a < num_actions; ++a)
            mixed += static_cast<double>(detail[a * num_hands + h]) * strategy[a * num_hands + h];
        CHECK_NEAR(ev[h], mixed, 1e-2);
        if (detail[h] < -1e-3) any_fold_row_charged = true;
    }
    // OOP's 30 is matched, so giving it up is charged the premium: -30 * (BF - 1).
    CHECK(any_fold_row_charged);
    CHECK_NEAR(detail[0], -30.0 * loss_extra(0.08), 1e-2);
}

// Compression stores values as 16-bit integers scaled by the slice maximum, which a premium
// inflates. The strategies must still agree with the uncompressed solve.
PFS_TEST(risk_premium, agrees_between_compressed_and_uncompressed) {
    std::array<std::vector<float>, 2> strategy;
    for (size_t pass = 0; pass < 2; ++pass) {
        auto game = build(cards("AA,KK,QQ,JJ", "AA,KK,QQ,TT", "Td9d6h", "Qc", "7s"),
                          river_tree(0.05, 0.05));
        if (!game) return;
        game->allocate_memory(pass == 1);
        solve(*game, 300, 0.0f, false);
        game->cache_normalized_weights();
        strategy[pass] = game->strategy();
    }
    CHECK_EQ(strategy[0].size(), strategy[1].size());
    for (size_t i = 0; i < strategy[0].size() && i < strategy[1].size(); ++i)
        CHECK_NEAR(strategy[0][i], strategy[1][i], 2e-2);
}
