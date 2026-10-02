// Port of the two #[ignore]d PioSOLVER-verified tests plus isomorphism_monotone
// from src/game/tests.rs. The PioSOLVER ones actually solve a full flop tree, so
// they are gated behind PFS_RUN_SLOW.
#include "harness.hpp"

#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <cstdlib>
#include <memory>
#include <vector>

using namespace pfs;

namespace {

const char* kOopRange =
    "88+,A8s+,A5s-A2s:0.5,AJo+,ATo:0.75,K9s+,KQo,KJo:0.75,KTo:0.25,Q9s+,QJo:0.5,J8s+,JTo:0.25,"
    "T8s+,T7s:0.45,97s+,96s:0.45,87s,86s:0.75,85s:0.45,75s+:0.75,74s:0.45,65s:0.75,64s:0.5,"
    "63s:0.45,54s:0.75,53s:0.5,52s:0.45,43s:0.5,42s:0.45,32s:0.45";
const char* kIpRange =
    "AA:0.25,99-22,AJs-A2s,AQo-A8o,K2s+,K9o+,Q2s+,Q9o+,J6s+,J9o+,T6s+,T9o,96s+,95s:0.5,98o,"
    "86s+,85s:0.5,75s+,74s:0.5,64s+,63s:0.5,54s,53s:0.5,43s";

std::unique_ptr<PostFlopGame> build_pio(const char* flop, double rake_rate, double rake_cap) {
    CardConfig cc;
    cc.range[0] = Range::parse(kOopRange).value();
    cc.range[1] = Range::parse(kIpRange).value();
    cc.flop = flop_from_str(flop).value();

    TreeConfig tc;
    tc.starting_pot = 180;
    tc.effective_stack = 910;
    tc.rake_rate = rake_rate;
    tc.rake_cap = rake_cap;
    const BetSizeOptions f = BetSizeOptions::parse("52%", "45%").value();
    const BetSizeOptions t = BetSizeOptions::parse("55%", "45%").value();
    const BetSizeOptions r = BetSizeOptions::parse("70%", "45%").value();
    tc.flop_bet_sizes = {f, f};
    tc.turn_bet_sizes = {t, t};
    tc.river_bet_sizes = {r, r};

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    if (!tree.is_ok()) return nullptr;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    if (!g.is_ok()) return nullptr;
    return std::move(g.value());
}

bool skip_slow() {
    if (std::getenv("PFS_RUN_SLOW") == nullptr) {
        std::printf("    skipped (set PFS_RUN_SLOW=1 to run)\n");
        return true;
    }
    return false;
}

}  // namespace

PFS_TEST(pio_slow, solve_preset_normal) {
    if (skip_slow()) return;

    auto game = build_pio("QsJh2h", 0.0, 0.0);
    CHECK(game != nullptr);
    if (!game) return;

    std::printf("    memory usage: %.2f GB\n",
                static_cast<double>(game->memory_usage().first) / (1024.0 * 1024.0 * 1024.0));
    game->allocate_memory(false);

    solve(*game, 1000, 180.0f * 0.001f, true);
    game->cache_normalized_weights();

    const std::span<const float> w0 = game->normalized_weights(0);
    const std::span<const float> w1 = game->normalized_weights(1);

    // Verified by PioSOLVER Free.
    CHECK_NEAR(compute_average(game->equity(0), w0), 0.55347f, 1e-5);
    CHECK_NEAR(compute_average(game->equity(1), w1), 0.44653f, 1e-5);
    CHECK_NEAR(compute_average(game->expected_values(0), w0), 105.11f, 0.2);
    CHECK_NEAR(compute_average(game->expected_values(1), w1), 74.89f, 0.2);
}

PFS_TEST(pio_slow, solve_preset_raked) {
    if (skip_slow()) return;

    auto game = build_pio("QsJh2h", 0.05, 30.0);
    CHECK(game != nullptr);
    if (!game) return;

    game->allocate_memory(false);
    solve(*game, 1000, 180.0f * 0.001f, true);
    game->cache_normalized_weights();

    const std::span<const float> w0 = game->normalized_weights(0);
    const std::span<const float> w1 = game->normalized_weights(1);

    // Verified by PioSOLVER Free, though not theoretically guaranteed to match.
    CHECK_NEAR(compute_average(game->expected_values(0), w0), 95.57f, 0.2);
    CHECK_NEAR(compute_average(game->expected_values(1), w1), 66.98f, 0.2);
}

// Port of game/tests.rs::isomorphism_monotone. On a monotone flop two of the four
// suits are isomorphic, so playing an eliminated card records a suit swap. This
// checks the exact swap state for 20 histories, and that no hand with positive
// weight ends up with the "untouched" EV of exactly 50.0.
PFS_TEST(game, isomorphism_monotone) {
    CardConfig cc;
    cc.range[0] = Range::parse(kOopRange).value();
    cc.range[1] = Range::parse(kIpRange).value();
    cc.flop = flop_from_str("QhJh2h").value();

    TreeConfig tc;
    tc.starting_pot = 100;
    tc.effective_stack = 100;

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    if (!tree.is_ok()) return;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    if (!g.is_ok()) return;
    PostFlopGame& game = *g.value();

    game.allocate_memory(false);
    finalize(game);

    auto check = [&](std::vector<size_t> history, std::optional<uint8_t> expected_turn_swap,
                     std::optional<std::pair<uint8_t, uint8_t>> expected_river_swap, int line) {
        game.apply_history(history);
        game.cache_normalized_weights();
        const std::span<const float> weights = game.normalized_weights(0);
        const std::vector<float> ev = game.expected_values(0);
        for (size_t i = 0; i < weights.size() && i < ev.size(); ++i)
            if (weights[i] > 0.0f && ev[i] == 50.0f)
                ::pfs::test::report_failure(__FILE__, line, "hand left at the unswapped EV 50.0");

        if (game.turn_swap() != expected_turn_swap)
            ::pfs::test::report_failure(__FILE__, line, "turn_swap mismatch");
        if (game.river_swap() != expected_river_swap)
            ::pfs::test::report_failure(__FILE__, line, "river_swap mismatch");
    };

    using RS = std::optional<std::pair<uint8_t, uint8_t>>;
    auto rs = [](int a, int b) {
        return RS(std::pair<uint8_t, uint8_t>{static_cast<uint8_t>(a), static_cast<uint8_t>(b)});
    };
    const std::optional<uint8_t> none_ts = std::nullopt;
    const RS none_rs = std::nullopt;

    check({0, 0, 4}, none_ts, none_rs, __LINE__);
    check({0, 0, 5}, uint8_t{1}, none_rs, __LINE__);
    check({0, 0, 6}, none_ts, none_rs, __LINE__);
    check({0, 0, 7}, uint8_t{3}, none_rs, __LINE__);

    check({0, 0, 4, 0, 0, 8}, none_ts, none_rs, __LINE__);
    check({0, 0, 4, 0, 0, 9}, none_ts, none_rs, __LINE__);
    check({0, 0, 4, 0, 0, 10}, none_ts, none_rs, __LINE__);
    check({0, 0, 4, 0, 0, 11}, none_ts, rs(0, 3), __LINE__);

    check({0, 0, 5, 0, 0, 8}, uint8_t{1}, none_rs, __LINE__);
    check({0, 0, 5, 0, 0, 9}, uint8_t{1}, none_rs, __LINE__);
    check({0, 0, 5, 0, 0, 10}, uint8_t{1}, none_rs, __LINE__);
    check({0, 0, 5, 0, 0, 11}, uint8_t{1}, rs(1, 3), __LINE__);

    check({0, 0, 6, 0, 0, 8}, none_ts, none_rs, __LINE__);
    check({0, 0, 6, 0, 0, 9}, none_ts, rs(2, 1), __LINE__);
    check({0, 0, 6, 0, 0, 10}, none_ts, none_rs, __LINE__);
    check({0, 0, 6, 0, 0, 11}, none_ts, rs(2, 3), __LINE__);

    check({0, 0, 7, 0, 0, 8}, uint8_t{3}, rs(3, 1), __LINE__);
    check({0, 0, 7, 0, 0, 9}, uint8_t{3}, none_rs, __LINE__);
    check({0, 0, 7, 0, 0, 10}, uint8_t{3}, none_rs, __LINE__);
    check({0, 0, 7, 0, 0, 11}, uint8_t{3}, none_rs, __LINE__);
}
