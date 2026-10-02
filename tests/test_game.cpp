// Port of src/game/tests.rs.
//
// Most of these call allocate_memory + finalize WITHOUT solving, so the strategy
// is the uniform fallback that normalized_strategy substitutes when every
// cumulative value is zero. That makes the expected EVs exact closed-form numbers
// rather than convergence targets, which is what makes them such good oracles.
#include "harness.hpp"

#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pfs;

namespace {

struct Built {
    std::unique_ptr<PostFlopGame> game;
};

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

CardConfig all_range(const char* flop, const char* turn = nullptr, const char* river = nullptr) {
    CardConfig c;
    c.range[0] = Range::ones();
    c.range[1] = Range::ones();
    c.flop = flop_from_str(flop).value();
    if (turn) c.turn = card_from_str(turn).value();
    if (river) c.river = card_from_str(river).value();
    return c;
}

TreeConfig base_tree() {
    TreeConfig t;
    t.starting_pot = 60;
    t.effective_stack = 970;
    return t;
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

// The four averages every test in game/tests.rs checks.
struct Averages {
    float equity_oop, equity_ip, ev_oop, ev_ip;
};

Averages measure(PostFlopGame& game) {
    game.cache_normalized_weights();
    const std::span<const float> w0 = game.normalized_weights(0);
    const std::span<const float> w1 = game.normalized_weights(1);
    return {compute_average(game.equity(0), w0), compute_average(game.equity(1), w1),
            compute_average(game.expected_values(0), w0),
            compute_average(game.expected_values(1), w1)};
}

void expect(PostFlopGame& game, float eq_oop, float eq_ip, float ev_oop, float ev_ip,
            double eq_tol = 1e-5, double ev_tol = 1e-4) {
    const Averages a = measure(game);
    CHECK_NEAR(a.equity_oop, eq_oop, eq_tol);
    CHECK_NEAR(a.equity_ip, eq_ip, eq_tol);
    CHECK_NEAR(a.ev_oop, ev_oop, ev_tol);
    CHECK_NEAR(a.ev_ip, ev_ip, ev_tol);
}

}  // namespace

PFS_TEST(game, all_check_all_range) {
    auto game = build(all_range("Td9d6h"), base_tree());
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);

    // Root, then every node down a line of checks: the pot is split evenly and
    // each player's EV is half the starting pot.
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->play(0);
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->play(0);
    CHECK(game->is_chance_node());
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->play(SIZE_MAX);  // lowest available card
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->play(0);
    game->play(0);
    CHECK(game->is_chance_node());
    game->play(SIZE_MAX);
    game->play(0);
    game->play(0);
    CHECK(game->is_terminal_node());
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);
}

PFS_TEST(game, one_raise_all_range) {
    TreeConfig tc = base_tree();
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    auto game = build(all_range("Td9d6h"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);

    // OOP can bet the river, so it captures more of the pot in expectation.
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);

    game->play(0);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);

    game->play(0);
    CHECK(game->is_chance_node());
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);

    game->play(SIZE_MAX);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);

    // Reach the river and have OOP bet.
    game->play(0);
    game->play(0);
    CHECK(game->is_chance_node());
    game->play(SIZE_MAX);
    game->play(1);
    expect(*game, 0.5f, 0.5f, 75.0f, 15.0f);

    game->play(1);  // IP calls
    CHECK(game->is_terminal_node());
    expect(*game, 0.5f, 0.5f, 60.0f, 60.0f);
}

PFS_TEST(game, one_raise_all_range_compressed) {
    TreeConfig tc = base_tree();
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    auto game = build(all_range("Td9d6h"), std::move(tc));
    if (!game) return;

    // 16-bit storage: same numbers, looser tolerances.
    game->allocate_memory(true);
    finalize(*game);

    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f, 1e-4, 1e-2);

    game->play(0);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f, 1e-4, 1e-2);

    game->play(0);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f, 1e-4, 1e-2);

    game->play(SIZE_MAX);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f, 1e-4, 1e-2);

    game->play(0);
    game->play(0);
    game->play(SIZE_MAX);
    game->play(1);
    expect(*game, 0.5f, 0.5f, 75.0f, 15.0f, 1e-4, 1e-2);

    game->play(1);
    expect(*game, 0.5f, 0.5f, 60.0f, 60.0f, 1e-4, 1e-2);
}

PFS_TEST(game, one_raise_all_range_with_turn) {
    TreeConfig tc = base_tree();
    tc.initial_state = BoardState::Turn;
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    auto game = build(all_range("Td9d6h", "Qc"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);
}

PFS_TEST(game, one_raise_all_range_with_river) {
    TreeConfig tc = base_tree();
    tc.initial_state = BoardState::River;
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    auto game = build(all_range("Td9d6h", "Qc", "7s"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    expect(*game, 0.5f, 0.5f, 37.5f, 22.5f);

    game->play(0);  // OOP checks
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->play(0);  // IP checks -> showdown
    CHECK(game->is_terminal_node());
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);

    game->back_to_root();
    game->play(1);  // OOP bets
    expect(*game, 0.5f, 0.5f, 75.0f, 15.0f);

    game->play(0);  // IP folds
    CHECK(game->is_terminal_node());
    expect(*game, 0.5f, 0.5f, 90.0f, 0.0f);
}

PFS_TEST(game, always_win) {
    auto game = build(cards("AA", "KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+", "AcAdKh"),
                      base_tree());
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    // OOP holds the only remaining aces on an ace-ace board: it cannot lose.
    expect(*game, 1.0f, 0.0f, 60.0f, 0.0f);

    // Walk to the river terminal; the result must hold there too.
    game->play(0);
    game->play(0);
    game->play(SIZE_MAX);
    game->play(0);
    game->play(0);
    game->play(SIZE_MAX);
    game->play(0);
    game->play(0);
    CHECK(game->is_terminal_node());
    expect(*game, 1.0f, 0.0f, 60.0f, 0.0f);
}

PFS_TEST(game, always_win_raked) {
    TreeConfig tc = base_tree();
    tc.rake_rate = 0.05;
    tc.rake_cap = 10.0;
    auto game = build(cards("AA", "KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+", "AcAdKh"),
                      std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    // 5% of the 60 pot is 3, which is under the 10 cap.
    expect(*game, 1.0f, 0.0f, 57.0f, 0.0f);
}

PFS_TEST(game, always_lose) {
    auto game = build(cards("KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+", "AA", "AcAdKh"),
                      base_tree());
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    expect(*game, 0.0f, 1.0f, 0.0f, 60.0f);
}

PFS_TEST(game, always_lose_raked) {
    TreeConfig tc = base_tree();
    tc.rake_rate = 0.05;
    tc.rake_cap = 10.0;
    auto game = build(cards("KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+", "AA", "AcAdKh"),
                      std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    expect(*game, 0.0f, 1.0f, 0.0f, 57.0f);
}

PFS_TEST(game, always_tie) {
    auto game = build(cards("AA", "AA", "2c6dTh"), base_tree());
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    expect(*game, 0.5f, 0.5f, 30.0f, 30.0f);
}

PFS_TEST(game, always_tie_raked) {
    TreeConfig tc = base_tree();
    tc.rake_rate = 0.05;
    tc.rake_cap = 10.0;
    auto game = build(cards("AA", "AA", "2c6dTh"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    finalize(*game);
    // The 3.0 rake is split: 1.5 off each side. This is the tie bucket of the
    // 3-sweep raked showdown path.
    expect(*game, 0.5f, 0.5f, 28.5f, 28.5f);
}

PFS_TEST(game, no_assignment) {
    // Both players need the same two tens, so no legal assignment exists.
    Result<ActionTree> tree = ActionTree::create(base_tree());
    CHECK(tree.is_ok());
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(cards("TT", "TT", "Td9d6h"), std::move(tree.value()));
    CHECK(!g.is_ok());
}

PFS_TEST(game, config_errors_are_reported) {
    auto make = [](CardConfig cc, TreeConfig tc) {
        Result<ActionTree> t = ActionTree::create(std::move(tc));
        if (!t.is_ok()) return false;
        return PostFlopGame::with_config(std::move(cc), std::move(t.value())).is_ok();
    };

    // initial_state must agree with the board.
    TreeConfig tc = base_tree();
    CHECK(!make(all_range("Td9d6h", "Qc"), tc));  // turn dealt but state is Flop
    tc.initial_state = BoardState::Turn;
    CHECK(make(all_range("Td9d6h", "Qc"), tc));
    CHECK(!make(all_range("Td9d6h"), tc));  // state Turn but no turn card

    // An empty range is rejected.
    CardConfig empty = all_range("Td9d6h");
    empty.range[0] = Range();
    CHECK(!make(std::move(empty), base_tree()));
}

PFS_TEST(game, memory_usage_and_state) {
    auto game = build(all_range("Td9d6h"), base_tree());
    if (!game) return;

    CHECK(game->state() == GameState::TreeBuilt);
    CHECK(!game->is_memory_allocated().has_value());

    const auto [uncompressed, compressed] = game->memory_usage();
    CHECK(uncompressed > compressed);
    CHECK(compressed > 0u);

    game->allocate_memory(false);
    CHECK(game->state() == GameState::MemoryAllocated);
    CHECK(game->is_memory_allocated().has_value());
    CHECK_EQ(*game->is_memory_allocated(), false);
    CHECK(game->is_ready());

    game->allocate_memory(true);
    CHECK_EQ(*game->is_memory_allocated(), true);

    finalize(*game);
    CHECK(game->state() == GameState::Solved);
    CHECK(game->is_solved());

    // Re-allocating downgrades Solved back to MemoryAllocated: the sanctioned
    // re-solve path used by the node-locking flow.
    game->allocate_memory(true);
    CHECK(game->state() == GameState::MemoryAllocated);
    CHECK(!game->is_solved());
}

PFS_TEST(game, private_cards_ordering) {
    // The exact ordering asserted by examples/basic.rs.
    TreeConfig tc;
    tc.initial_state = BoardState::Turn;
    tc.starting_pot = 200;
    tc.effective_stack = 900;
    const BetSizeOptions sizes = BetSizeOptions::parse("60%, e, a", "2.5x").value();
    tc.flop_bet_sizes = {sizes, sizes};
    tc.turn_bet_sizes = {sizes, sizes};
    tc.river_bet_sizes = {sizes, sizes};
    tc.river_donk_sizes = DonkSizeOptions::parse("50%").value();
    tc.add_allin_threshold = 1.5;
    tc.force_allin_threshold = 0.15;
    tc.merging_threshold = 0.1;

    CardConfig cc =
        cards("66+,A8s+,A5s-A4s,AJo+,K9s+,KQo,QTs+,JTs,96s+,85s+,75s+,65s,54s",
              "QQ-22,AQs-A2s,ATo+,K5s+,KJo+,Q8s+,J8s+,T7s+,96s+,86s+,75s+,64s+,53s+",
              "Td9d6h", "Qc");

    auto game = build(std::move(cc), std::move(tc));
    if (!game) return;

    const std::vector<Hole> oop(game->private_cards(0).begin(), game->private_cards(0).end());
    const Result<std::vector<std::string>> strs = holes_to_strings(oop);
    CHECK(strs.is_ok());
    const char* expected[10] = {"5c4c", "Ac4c", "5d4d", "Ad4d", "5h4h",
                                "Ah4h", "5s4s", "As4s", "6c5c", "7c5c"};
    for (size_t i = 0; i < 10; ++i) CHECK_EQ(strs.value()[i], std::string(expected[i]));

    CHECK_EQ(game->private_cards(1).size(), 250u);
}

PFS_TEST(game, available_actions_and_board) {
    TreeConfig tc;
    tc.initial_state = BoardState::Turn;
    tc.starting_pot = 200;
    tc.effective_stack = 900;
    const BetSizeOptions sizes = BetSizeOptions::parse("60%, e, a", "2.5x").value();
    tc.turn_bet_sizes = {sizes, sizes};
    tc.river_bet_sizes = {sizes, sizes};
    tc.river_donk_sizes = DonkSizeOptions::parse("50%").value();
    tc.add_allin_threshold = 1.5;
    tc.force_allin_threshold = 0.15;
    tc.merging_threshold = 0.1;

    auto game = build(all_range("Td9d6h", "Qc"), std::move(tc));
    if (!game) return;
    game->allocate_memory(false);

    CHECK_EQ(actions_to_string(game->available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));
    CHECK_EQ(game->current_board().size(), 4u);

    game->play(1);
    CHECK_EQ(actions_to_string(game->available_actions()),
             std::string("[Fold, Call, Raise(300)]"));

    game->play(1);
    CHECK(game->is_chance_node());

    const Card c7s = card_from_str("7s").value();
    CHECK((game->possible_cards() & (uint64_t{1} << c7s)) != 0);
    game->play(c7s);
    CHECK_EQ(game->current_board().size(), 5u);
    CHECK_EQ(game->current_board()[4], c7s);

    game->back_to_root();
    CHECK_EQ(actions_to_string(game->available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));
}

// Port of game/tests.rs::node_locking. Note the target exploitability of 0.0:
// the Rust runs a fixed 1000 iterations and checks the resulting strategy rather
// than asserting convergence.
PFS_TEST(game, node_locking) {
    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 20;
    tc.effective_stack = 10;
    tc.river_bet_sizes = {BetSizeOptions::parse("a", "").value(),
                          BetSizeOptions::parse("a", "").value()};

    auto game = build(cards("AsAh,QsQh", "KsKh", "2s3h4d", "6c", "7c"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    game->play(1);  // OOP all-in
    // Lock IP to 25% fold / 75% call. Locking must happen after allocating memory
    // and before solving.
    game->lock_current_strategy(std::vector<float>{0.25f, 0.75f});
    game->back_to_root();

    solve(*game, 1000, 0.0f, false);
    game->cache_normalized_weights();

    {
        const std::vector<float> ev_oop = game->expected_values(0);
        const std::vector<float> ev_ip = game->expected_values(1);
        CHECK_NEAR(ev_oop[0], 0.0f, 1e-2);
        CHECK_NEAR(ev_oop[1], 27.5f, 5e-2);
        CHECK_NEAR(ev_ip[0], 6.25f, 1e-2);

        // Action-major: [QQ check, AA check, QQ jam, AA jam].
        const std::vector<float> s = game->strategy();
        CHECK_NEAR(s[0], 1.0f, 1e-3);  // QQ always checks
        CHECK_NEAR(s[1], 0.0f, 1e-3);  // AA never checks
        CHECK_NEAR(s[2], 0.0f, 1e-3);  // QQ never jams
        CHECK_NEAR(s[3], 1.0f, 1e-3);  // AA always jams
    }

    // Re-allocating resets Solved -> MemoryAllocated so the node can be re-locked.
    game->allocate_memory(false);
    game->play(1);
    game->lock_current_strategy(std::vector<float>{0.5f, 0.5f});
    game->back_to_root();

    solve(*game, 1000, 0.0f, false);
    game->cache_normalized_weights();

    {
        const std::vector<float> ev_oop = game->expected_values(0);
        const std::vector<float> ev_ip = game->expected_values(1);
        CHECK_NEAR(ev_oop[0], 5.0f, 1e-2);
        CHECK_NEAR(ev_oop[1], 25.0f, 5e-2);
        CHECK_NEAR(ev_ip[0], 5.0f, 1e-2);

        // Against a looser caller, QQ starts jamming too.
        const std::vector<float> s = game->strategy();
        CHECK_NEAR(s[0], 0.0f, 1e-3);
        CHECK_NEAR(s[1], 0.0f, 1e-3);
        CHECK_NEAR(s[2], 1.0f, 1e-3);
        CHECK_NEAR(s[3], 1.0f, 1e-3);
    }
}

// Port of game/tests.rs::node_locking_partial -- only JJ is locked; QQ and AA
// still solve, which exercises the -1.0 "unlocked" sentinel.
PFS_TEST(game, node_locking_partial) {
    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 10;
    tc.effective_stack = 10;
    tc.river_bet_sizes = {BetSizeOptions::parse("a", "").value(),
                          BetSizeOptions::parse("a", "").value()};

    auto game = build(cards("AsAh,QsQh,JsJh", "KsKh", "2s3h4d", "6c", "7c"), std::move(tc));
    if (!game) return;

    game->allocate_memory(false);
    // JJ -> 80% check, 20% all-in; QQ and AA left free.
    game->lock_current_strategy(
        std::vector<float>{0.8f, 0.0f, 0.0f, 0.2f, 0.0f, 0.0f});

    solve(*game, 1000, 0.0f, false);
    game->cache_normalized_weights();

    const std::vector<float> ev_oop = game->expected_values(0);
    const std::vector<float> ev_ip = game->expected_values(1);
    CHECK_NEAR(ev_oop[0], 0.0f, 1e-2);
    CHECK_NEAR(ev_oop[1], 0.0f, 1e-2);
    CHECK_NEAR(ev_oop[2], 15.0f, 5e-2);
    CHECK_NEAR(ev_ip[0], 5.0f, 1e-2);

    const std::vector<float> s = game->strategy();
    CHECK_NEAR(s[0], 0.8f, 1e-3);  // JJ check (locked)
    CHECK_NEAR(s[1], 0.7f, 1e-3);  // QQ check (solved)
    CHECK_NEAR(s[2], 0.0f, 1e-3);  // AA never checks
    CHECK_NEAR(s[3], 0.2f, 1e-3);  // JJ bet (locked)
    CHECK_NEAR(s[4], 0.3f, 1e-3);  // QQ bet (solved)
    CHECK_NEAR(s[5], 1.0f, 1e-3);  // AA always bets
}

PFS_TEST(game, lock_and_unlock_round_trip) {
    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 20;
    tc.effective_stack = 10;
    tc.river_bet_sizes = {BetSizeOptions::parse("a", "").value(),
                          BetSizeOptions::parse("a", "").value()};

    auto game = build(cards("AsAh,QsQh", "KsKh", "2s3h4d", "6c", "7c"), std::move(tc));
    if (!game) return;
    game->allocate_memory(false);

    CHECK(!game->current_locking_strategy().has_value());

    game->lock_current_strategy(std::vector<float>{0.25f, 0.0f, 0.75f, 0.0f});
    const auto locked = game->current_locking_strategy();
    CHECK(locked.has_value());
    CHECK_EQ(locked->size(), 4u);
    // The first hand is locked and normalized; the second is untouched, marked -1.
    CHECK_NEAR((*locked)[0], 0.25f, 1e-6);
    CHECK_NEAR((*locked)[2], 0.75f, 1e-6);
    CHECK_EQ((*locked)[1], -1.0f);
    CHECK_EQ((*locked)[3], -1.0f);

    game->unlock_current_strategy();
    CHECK(!game->current_locking_strategy().has_value());
    // Unlocking twice is a no-op, not an error.
    game->unlock_current_strategy();
}

// Port of game/tests.rs::remove_lines -- removing chance-specific lines, which is
// only possible on the built game tree and not on the abstract action tree.
PFS_TEST(game, remove_lines) {
    TreeConfig tc = base_tree();
    tc.turn_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};

    auto game = build(cards("TT+,AKo,AQs+", "AA", "2c6dTh"), std::move(tc));
    if (!game) return;

    const std::vector<std::vector<Action>> lines{
        {Action::check(), Action::check(), Action::chance(2), Action::check()},
        {Action::check(), Action::check(), Action::chance(2), Action::bet(30), Action::call(),
         Action::chance(3), Action::bet(60)},
    };

    CHECK(game->remove_lines(lines).is_ok());
    game->allocate_memory(false);

    auto actions_at = [&](std::vector<size_t> history) {
        game->apply_history(history);
        return actions_to_string(game->available_actions());
    };

    // The turn check was removed, leaving only the bet.
    CHECK_EQ(actions_at({0, 0, 2}), std::string("[Bet(30)]"));
    // A different turn card is untouched.
    CHECK_EQ(actions_at({0, 0, 3}), std::string("[Check, Bet(30)]"));
    // The river bet was removed on that line.
    CHECK_EQ(actions_at({0, 0, 2, 0, 1, 3}), std::string("[Check]"));
    // Other river lines are intact.
    CHECK_EQ(actions_at({0, 0, 2, 0, 1, 4}), std::string("[Check, Bet(60)]"));
    CHECK_EQ(actions_at({0, 0, 3, 1, 1, 4}), std::string("[Check, Bet(60)]"));

    // Solving the pruned tree must not crash.
    game->back_to_root();
    solve(*game, 10, 0.01f, false);
}
