// Port of examples/basic.rs.
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace pfs;

int main() {
    // Ranges in string format; see include/pfs/range.hpp for the grammar.
    const char* oop_range = "66+,A8s+,A5s-A4s,AJo+,K9s+,KQo,QTs+,JTs,96s+,85s+,75s+,65s,54s";
    const char* ip_range = "QQ-22,AQs-A2s,ATo+,K5s+,KJo+,Q8s+,J8s+,T7s+,96s+,86s+,75s+,64s+,53s+";

    CardConfig card_config;
    card_config.range[0] = Range::parse(oop_range).value();
    card_config.range[1] = Range::parse(ip_range).value();
    card_config.flop = flop_from_str("Td9d6h").value();
    card_config.turn = card_from_str("Qc").value();
    card_config.river = NOT_DEALT;

    // Bets: 60% of the pot, a geometric size, and all-in. Raises: 2.5x the previous bet.
    const BetSizeOptions bet_sizes = BetSizeOptions::parse("60%, e, a", "2.5x").value();

    TreeConfig tree_config;
    tree_config.initial_state = BoardState::Turn;  // must match card_config
    tree_config.starting_pot = 200;
    tree_config.effective_stack = 900;
    tree_config.rake_rate = 0.0;
    tree_config.rake_cap = 0.0;
    tree_config.flop_bet_sizes = {bet_sizes, bet_sizes};  // [OOP, IP]
    tree_config.turn_bet_sizes = {bet_sizes, bet_sizes};
    tree_config.river_bet_sizes = {bet_sizes, bet_sizes};
    tree_config.turn_donk_sizes = std::nullopt;  // use the default bet sizes
    tree_config.river_donk_sizes = DonkSizeOptions::parse("50%").value();
    tree_config.add_allin_threshold = 1.5;    // add all-in if max bet <= 1.5x pot
    tree_config.force_allin_threshold = 0.15; // force all-in if SPR after a call <= 0.15
    tree_config.merging_threshold = 0.1;

    // Build the tree. An ActionTree can be edited after construction.
    Result<ActionTree> action_tree = ActionTree::create(std::move(tree_config));
    if (!action_tree.is_ok()) {
        std::printf("failed to build action tree: %s\n", action_tree.error().c_str());
        return 1;
    }
    Result<std::unique_ptr<PostFlopGame>> built =
        PostFlopGame::with_config(std::move(card_config), std::move(action_tree.value()));
    if (!built.is_ok()) {
        std::printf("failed to build game: %s\n", built.error().c_str());
        return 1;
    }
    PostFlopGame& game = *built.value();

    // The private hands, in (low, high) order and lexicographically sorted.
    const std::span<const Hole> oop_cards = game.private_cards(0);
    const std::vector<std::string> oop_cards_str =
        holes_to_strings(std::vector<Hole>(oop_cards.begin(), oop_cards.end())).value();
    const char* expected_first_ten[10] = {"5c4c", "Ac4c", "5d4d", "Ad4d", "5h4h",
                                          "Ah4h", "5s4s", "As4s", "6c5c", "7c5c"};
    for (size_t i = 0; i < 10; ++i) assert(oop_cards_str[i] == expected_first_ten[i]);

    const auto [mem, mem_compressed] = game.memory_usage();
    std::printf("Memory usage without compression (32-bit float): %.2fGB\n",
                static_cast<double>(mem) / (1024.0 * 1024.0 * 1024.0));
    std::printf("Memory usage with compression (16-bit integer): %.2fGB\n",
                static_cast<double>(mem_compressed) / (1024.0 * 1024.0 * 1024.0));

    game.allocate_memory(false);  // pass true for the 16-bit mode

    const uint32_t max_num_iterations = 1000;
    const float target_exploitability =
        static_cast<float>(game.tree_config().starting_pot) * 0.005f;  // 0.5% of the pot
    const float exploitability = solve(game, max_num_iterations, target_exploitability, true);
    std::printf("Exploitability: %.2f\n", static_cast<double>(exploitability));

    // Solving manually is also possible:
    //   for (uint32_t i = 0; i < max_num_iterations; ++i) {
    //       solve_step(game, i);
    //       if ((i + 1) % 10 == 0 && compute_exploitability(game) <= target_exploitability) break;
    //   }
    //   finalize(game);

    game.cache_normalized_weights();
    const std::vector<float> equity = game.equity(0);  // 0 means OOP
    const std::vector<float> ev = game.expected_values(0);
    std::printf("Equity of oop_hands[0]: %.2f%%\n", 100.0 * static_cast<double>(equity[0]));
    std::printf("EV of oop_hands[0]: %.2f\n", static_cast<double>(ev[0]));

    const std::span<const float> weights = game.normalized_weights(0);
    std::printf("Average equity: %.2f%%\n",
                100.0 * static_cast<double>(compute_average(equity, weights)));
    std::printf("Average EV: %.2f\n", static_cast<double>(compute_average(ev, weights)));

    assert(actions_to_string(game.available_actions()) ==
           "[Check, Bet(120), Bet(216), AllIn(900)]");

    game.play(1);  // Bet(120)
    assert(actions_to_string(game.available_actions()) == "[Fold, Call, Raise(300)]");

    // Confirm that IP does not fold the nut straight.
    const std::span<const Hole> ip_cards = game.private_cards(1);
    const std::vector<float> strategy = game.strategy();
    assert(ip_cards.size() == 250);
    assert(strategy.size() == 750);

    const std::vector<std::string> ip_strs =
        holes_to_strings(std::vector<Hole>(ip_cards.begin(), ip_cards.end())).value();
    size_t ksjs = SIZE_MAX;
    for (size_t i = 0; i < ip_strs.size(); ++i)
        if (ip_strs[i] == "KsJs") {
            ksjs = i;
            break;
        }
    assert(ksjs != SIZE_MAX);

    // strategy is action-major: [Fold, Call, Raise(300)] each of length 250.
    assert(strategy[ksjs] == 0.0f);
    assert(std::abs(strategy[ksjs] + strategy[ksjs + 250] + strategy[ksjs + 500] - 1.0f) < 1e-6f);

    game.play(1);  // Call
    assert(game.is_chance_node());

    const Card card_7s = card_from_str("7s").value();
    assert((game.possible_cards() & (uint64_t{1} << card_7s)) != 0);
    game.play(card_7s);  // at a chance node, the action IS the card id

    game.back_to_root();
    std::printf("basic: all assertions passed\n");
    return 0;
}
