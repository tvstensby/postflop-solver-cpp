// Port of game/tests.rs::set_bunching_effect and set_bunching_effect_always_win.
#include "harness.hpp"

#include <pfs/bunching.hpp>
#include <pfs/game.hpp>
#include <pfs/utility.hpp>

#include <memory>
#include <vector>

using namespace pfs;

namespace {

// The two 6-max folded-player ranges used by both Rust tests.
const char* kCoRange =
    "33:0.59,22:0.635,A8o:0.265,A7o-A6o,A5o:0.445,A4o-A2o,K2s,K9o:0.905,K8o-K2o,Q4s-Q2s,"
    "Q9o-Q2o,J6s-J2s,J9o:0.88,J8o-J2o,T7s:0.405,T6s-T2s,T9o:0.96,T8o-T2o,96s-92s,92o+,"
    "86s:0.57,85s-82s,82o+,76s:0.37,75s-72s,72o+,65s:0.475,64s-62s,62o+,54s:0.68,53s-52s,"
    "52o+,42+,32";
const char* kSbRange =
    "66:0.46,55:0.821,44:0.92,33:0.93,22:0.925,A6s:0.73,A3s:0.47,A2s,ATo:0.105,A9o-A2o,"
    "K8s:0.795,K7s,K6s:0.85,K5s:0.965,K4s-K2s,KJo:0.085,KTo:0.645,K9o-K2o,Q8s-Q2s,QJo:0.765,"
    "QTo-Q2o,J8s-J2s,J2o+,T8s:0.69,T7s-T2s,T2o+,98s:0.905,97s-92s,92o+,87s:0.78,86s-82s,82o+,"
    "76s:0.77,75s-72s,72o+,65s:0.845,64s-62s,62o+,54s:0.735,53s-52s,52o+,42+,32";

struct Averages {
    float eq_oop, eq_ip, ev_oop, ev_ip;
};

Averages measure(PostFlopGame& game) {
    game.cache_normalized_weights();
    return {compute_average(game.equity(0), game.normalized_weights(0)),
            compute_average(game.equity(1), game.normalized_weights(1)),
            compute_average(game.expected_values(0), game.normalized_weights(0)),
            compute_average(game.expected_values(1), game.normalized_weights(1))};
}

}  // namespace

PFS_TEST(bunching_game, set_bunching_effect_turn) {
    const std::array<Card, 3> flop = flop_from_str("Td9d6h").value();

    CardConfig cc;
    cc.range[0] = Range::ones();
    cc.range[1] = Range::ones();
    cc.flop = flop;
    cc.turn = card_from_str("Qc").value();

    TreeConfig tc;
    tc.initial_state = BoardState::Turn;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    tc.river_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    if (!tree.is_ok()) return;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    if (!g.is_ok()) return;
    PostFlopGame& game = *g.value();

    Result<BunchingData> bd = BunchingData::create(
        {Range::parse(kCoRange).value(), Range::parse(kSbRange).value()}, flop);
    CHECK(bd.is_ok());
    if (!bd.is_ok()) return;
    bd.value().process();
    CHECK(bd.value().is_ready());

    const Status st = game.set_bunching_effect(bd.value());
    CHECK(st.is_ok());
    if (!st.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, st.error());
        return;
    }

    game.allocate_memory(false);
    finalize(game);

    // OOP can bet the river, worth 7.5 of the 60 pot in expectation.
    const std::array<float, 2> ev = compute_current_ev(game);
    CHECK_NEAR(ev[0], 7.5f, 1e-4);
    CHECK_NEAR(ev[1], -7.5f, 1e-4);

    const Averages a = measure(game);
    CHECK_NEAR(a.eq_oop, 0.5f, 1e-5);
    CHECK_NEAR(a.eq_ip, 0.5f, 1e-5);
    CHECK_NEAR(a.ev_oop, 37.5f, 1e-4);
    CHECK_NEAR(a.ev_ip, 22.5f, 1e-4);
}

PFS_TEST(bunching_game, set_bunching_effect_always_win) {
    const std::array<Card, 3> flop = flop_from_str("AcAdKh").value();

    CardConfig cc;
    cc.range[0] = Range::parse("AA").value();
    cc.range[1] =
        Range::parse("KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+").value();
    cc.flop = flop;

    TreeConfig tc;
    tc.starting_pot = 60;
    tc.effective_stack = 970;

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    if (!tree.is_ok()) return;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    if (!g.is_ok()) return;
    PostFlopGame& game = *g.value();

    Result<BunchingData> bd = BunchingData::create(
        {Range::parse(kCoRange).value(), Range::parse(kSbRange).value()}, flop);
    CHECK(bd.is_ok());
    if (!bd.is_ok()) return;
    bd.value().process();

    const Status st = game.set_bunching_effect(bd.value());
    CHECK(st.is_ok());
    if (!st.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, st.error());
        return;
    }
    CHECK(game.memory_usage_bunching() > 0u);

    game.allocate_memory(false);
    finalize(game);

    const std::array<float, 2> ev = compute_current_ev(game);
    CHECK_NEAR(ev[0], 30.0f, 1e-4);
    CHECK_NEAR(ev[1], -30.0f, 1e-4);

    {
        const Averages a = measure(game);
        CHECK_NEAR(a.eq_oop, 1.0f, 1e-5);
        CHECK_NEAR(a.eq_ip, 0.0f, 1e-5);
        CHECK_NEAR(a.ev_oop, 60.0f, 1e-4);
        CHECK_NEAR(a.ev_ip, 0.0f, 1e-4);
    }

    // Same result at the river terminal, which exercises the bunching branches of
    // play(), assign_zero_weights() and cache_normalized_weights().
    game.play(0);
    game.play(0);
    CHECK(game.is_chance_node());
    game.play(SIZE_MAX);
    game.play(0);
    game.play(0);
    CHECK(game.is_chance_node());
    game.play(SIZE_MAX);
    game.play(0);
    game.play(0);
    CHECK(game.is_terminal_node());

    {
        const Averages a = measure(game);
        CHECK_NEAR(a.eq_oop, 1.0f, 1e-5);
        CHECK_NEAR(a.eq_ip, 0.0f, 1e-5);
        CHECK_NEAR(a.ev_oop, 60.0f, 1e-4);
        CHECK_NEAR(a.ev_ip, 0.0f, 1e-4);
    }
}

PFS_TEST(bunching_game, set_bunching_effect_validation) {
    const std::array<Card, 3> flop = flop_from_str("Td9d6h").value();

    CardConfig cc;
    cc.range[0] = Range::ones();
    cc.range[1] = Range::ones();
    cc.flop = flop;

    TreeConfig tc;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    if (!tree.is_ok()) return;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    if (!g.is_ok()) return;
    PostFlopGame& game = *g.value();

    // Unprocessed data is rejected.
    Result<BunchingData> unready = BunchingData::create({Range::parse(kCoRange).value()}, flop);
    CHECK(unready.is_ok());
    if (unready.is_ok()) CHECK(!game.set_bunching_effect(unready.value()).is_ok());

    // A mismatched flop is rejected.
    const std::array<Card, 3> other_flop = flop_from_str("2c3d4h").value();
    Result<BunchingData> wrong_flop =
        BunchingData::create({Range::parse(kCoRange).value()}, other_flop);
    CHECK(wrong_flop.is_ok());
    if (wrong_flop.is_ok()) {
        wrong_flop.value().process();
        CHECK(!game.set_bunching_effect(wrong_flop.value()).is_ok());
    }
}
