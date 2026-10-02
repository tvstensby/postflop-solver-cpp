#include "harness.hpp"

#include <pfs/action_tree.hpp>

#include <string>
#include <vector>

using namespace pfs;

namespace {

TreeConfig basic_config() {
    // The configuration from examples/basic.rs.
    TreeConfig c;
    c.initial_state = BoardState::Turn;
    c.starting_pot = 200;
    c.effective_stack = 900;
    c.rake_rate = 0.0;
    c.rake_cap = 0.0;
    const BetSizeOptions sizes = BetSizeOptions::parse("60%, e, a", "2.5x").value();
    c.flop_bet_sizes = {sizes, sizes};
    c.turn_bet_sizes = {sizes, sizes};
    c.river_bet_sizes = {sizes, sizes};
    c.turn_donk_sizes = std::nullopt;
    c.river_donk_sizes = DonkSizeOptions::parse("50%").value();
    c.add_allin_threshold = 1.5;
    c.force_allin_threshold = 0.15;
    c.merging_threshold = 0.1;
    return c;
}

// The configuration shared by most of src/game/tests.rs.
TreeConfig check_only_config() {
    TreeConfig c;
    c.starting_pot = 60;
    c.effective_stack = 970;
    return c;
}

ActionTree build(TreeConfig c) {
    Result<ActionTree> t = ActionTree::create(std::move(c));
    if (!t.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, "tree build failed: " + t.error());
        Result<ActionTree> fallback = ActionTree::create(check_only_config());
        return std::move(fallback.value());
    }
    return std::move(t.value());
}

// span cannot be built from a braced-init-list, so name the vector.
std::vector<Action> mkline(std::initializer_list<Action> as) { return std::vector<Action>(as); }

}  // namespace

PFS_TEST(action, ordering_is_discriminant_then_payload) {
    CHECK(Action::none() < Action::fold());
    CHECK(Action::fold() < Action::check());
    CHECK(Action::check() < Action::call());
    CHECK(Action::call() < Action::bet(1));
    CHECK(Action::bet(999999) < Action::raise(1));
    CHECK(Action::raise(999999) < Action::all_in(1));
    CHECK(Action::all_in(999999) < Action::chance(0));
    // Within a kind, by payload.
    CHECK(Action::bet(120) < Action::bet(216));
    CHECK(Action::chance(3) < Action::chance(51));
    // Payload-less kinds compare equal to themselves.
    CHECK(Action::fold() == Action::fold());
    CHECK(!(Action::fold() < Action::fold()));
}

PFS_TEST(action, binary_search_gives_insertion_index_on_miss) {
    const std::vector<Action> actions{Action::check(), Action::bet(120), Action::bet(216),
                                      Action::all_in(900)};
    CHECK(binary_search_action(actions, Action::check()).found);
    CHECK_EQ(binary_search_action(actions, Action::check()).index, 0u);
    CHECK(binary_search_action(actions, Action::bet(216)).found);
    CHECK_EQ(binary_search_action(actions, Action::bet(216)).index, 2u);

    // Misses report where the action belongs, which is how add_line inserts.
    const SearchResult miss = binary_search_action(actions, Action::bet(150));
    CHECK(!miss.found);
    CHECK_EQ(miss.index, 2u);
    const SearchResult front = binary_search_action(actions, Action::fold());
    CHECK(!front.found);
    CHECK_EQ(front.index, 0u);
    const SearchResult back = binary_search_action(actions, Action::chance(0));
    CHECK(!back.found);
    CHECK_EQ(back.index, 4u);
}

PFS_TEST(action_tree, config_validation) {
    TreeConfig c = check_only_config();
    CHECK(ActionTree::create(c).is_ok());

    auto bad = [](void (*mutate)(TreeConfig&)) {
        TreeConfig c2 = check_only_config();
        mutate(c2);
        return !ActionTree::create(c2).is_ok();
    };
    CHECK(bad([](TreeConfig& x) { x.starting_pot = 0; }));
    CHECK(bad([](TreeConfig& x) { x.effective_stack = 0; }));
    CHECK(bad([](TreeConfig& x) { x.rake_rate = -0.1; }));
    CHECK(bad([](TreeConfig& x) { x.rake_rate = 1.1; }));
    CHECK(bad([](TreeConfig& x) { x.rake_cap = -1.0; }));
    CHECK(bad([](TreeConfig& x) { x.add_allin_threshold = -1.0; }));
    CHECK(bad([](TreeConfig& x) { x.force_allin_threshold = -1.0; }));
    CHECK(bad([](TreeConfig& x) { x.merging_threshold = -1.0; }));
}

// These are the two action-list assertions from examples/basic.rs, and they
// exercise pot-relative sizing, geometric sizing, the all-in threshold, clamping,
// sorting and the PioSOLVER merging step all at once.
PFS_TEST(action_tree, basic_example_action_lists) {
    ActionTree tree = build(basic_config());

    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));

    // Play Bet(120).
    CHECK(tree.play(Action::bet(120)).is_ok());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Fold, Call, Raise(300)]"));

    // Call takes us to the river chance node.
    CHECK(tree.play(Action::call()).is_ok());
    CHECK(tree.is_chance_node());

    tree.back_to_root();
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));
}

PFS_TEST(action_tree, check_only_tree_is_well_formed) {
    ActionTree tree = build(check_only_config());

    // No bet sizes configured, so every node offers only Check / Call.
    CHECK_EQ(actions_to_string(tree.available_actions()), std::string("[Check]"));
    CHECK(tree.invalid_terminals().empty());

    // Flop check-check -> turn chance -> ... -> river check-check -> terminal.
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.is_chance_node());
    CHECK(tree.play(Action::check()).is_ok());  // chance is skipped automatically
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.is_chance_node());
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.is_terminal_node());

    CHECK(tree.undo().is_ok());
    CHECK(!tree.is_terminal_node());
    tree.back_to_root();
    CHECK(!tree.undo().is_ok());  // nothing left to undo
}

PFS_TEST(action_tree, node_counts_per_street) {
    // A flop-start check-only tree: 1 node per street plus the chance nodes.
    ActionTree tree = build(check_only_config());
    const ActionTree::Ejected e = std::move(tree).eject();
    const std::array<uint64_t, 3> counts = count_num_action_nodes(*e.root);
    CHECK(counts[0] > 0);
    CHECK(counts[1] > 0);
    CHECK(counts[2] > 0);

    // A river-start tree has all its nodes attributed to the river.
    TreeConfig rc = check_only_config();
    rc.initial_state = BoardState::River;
    ActionTree rt = build(rc);
    const ActionTree::Ejected re = std::move(rt).eject();
    const std::array<uint64_t, 3> rcounts = count_num_action_nodes(*re.root);
    CHECK_EQ(rcounts[0], 0u);
    CHECK_EQ(rcounts[1], 0u);
    CHECK(rcounts[2] > 0);
}

PFS_TEST(action_tree, total_bet_amount_tracks_both_players) {
    ActionTree tree = build(basic_config());
    CHECK_EQ(tree.total_bet_amount()[0], 0);
    CHECK_EQ(tree.total_bet_amount()[1], 0);

    CHECK(tree.play(Action::bet(120)).is_ok());
    CHECK_EQ(tree.total_bet_amount()[0], 120);
    CHECK_EQ(tree.total_bet_amount()[1], 0);

    CHECK(tree.play(Action::call()).is_ok());
    CHECK_EQ(tree.total_bet_amount()[0], 120);
    CHECK_EQ(tree.total_bet_amount()[1], 120);
}

PFS_TEST(action_tree, add_and_remove_line) {
    ActionTree tree = build(basic_config());

    // Bet(150) is a legal amount that the configured sizes do not produce.
    const std::vector<Action> line{Action::bet(150)};
    CHECK(tree.add_line(line).is_ok());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(150), Bet(216), AllIn(900)]"));
    CHECK_EQ(tree.added_lines().size(), 1u);

    // Adding it twice fails.
    CHECK(!tree.add_line(line).is_ok());

    // Removing it takes it back out and clears the added-line record.
    CHECK(tree.remove_line(line).is_ok());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));
    CHECK_EQ(tree.added_lines().size(), 0u);

    // Removing an existing configured line records it as removed.
    const std::vector<Action> existing{Action::bet(216)};
    CHECK(tree.remove_line(existing).is_ok());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), AllIn(900)]"));
    CHECK_EQ(tree.removed_lines().size(), 1u);

    // Adding it back drops the removed-line record rather than adding one.
    CHECK(tree.add_line(existing).is_ok());
    CHECK_EQ(tree.removed_lines().size(), 0u);
    CHECK_EQ(tree.added_lines().size(), 0u);

    // Out-of-range amounts are rejected.
    CHECK(!tree.add_line(mkline({Action::bet(0)})).is_ok());
    CHECK(!tree.add_line(mkline({Action::bet(100000)})).is_ok());
    CHECK(!tree.remove_line(mkline({Action::bet(12345)})).is_ok());
    CHECK(!tree.add_line(mkline({})).is_ok());
}

// Upstream quirk, reproduced deliberately: add_line looks the action up BEFORE
// rewriting a max-amount Bet/Raise into an AllIn, then inserts at the index it
// found. So adding Bet(900) to a node that already offers AllIn(900) succeeds and
// leaves a duplicate AllIn. Pinned here so a future "cleanup" cannot silently
// diverge from the Rust.
PFS_TEST(action_tree, max_amount_bet_is_promoted_to_allin_without_researching) {
    ActionTree tree = build(basic_config());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900)]"));

    CHECK(tree.add_line(mkline({Action::bet(900)})).is_ok());
    CHECK_EQ(actions_to_string(tree.available_actions()),
             std::string("[Check, Bet(120), Bet(216), AllIn(900), AllIn(900)]"));

    // The recorded added line reflects the promotion, not the requested Bet.
    CHECK_EQ(tree.added_lines().size(), 1u);
    CHECK_EQ(tree.added_lines()[0].size(), 1u);
    CHECK(tree.added_lines()[0][0] == Action::all_in(900));
}

PFS_TEST(action_tree, merging_threshold_drops_close_sizes) {
    TreeConfig c;
    c.starting_pot = 100;
    c.effective_stack = 1000;
    c.initial_state = BoardState::River;
    // 50% and 55% of the pot are within 10% of each other.
    const BetSizeOptions sizes = BetSizeOptions::parse("50%, 55%, 100%", "").value();
    c.river_bet_sizes = {sizes, sizes};

    c.merging_threshold = 0.0;
    ActionTree unmerged = build(c);
    CHECK_EQ(actions_to_string(unmerged.available_actions()),
             std::string("[Check, Bet(50), Bet(55), Bet(100)]"));

    c.merging_threshold = 0.1;
    ActionTree merged = build(c);
    // 55 survives (it is the larger of the close pair); 50 is dropped.
    CHECK_EQ(actions_to_string(merged.available_actions()),
             std::string("[Check, Bet(55), Bet(100)]"));
}

PFS_TEST(action_tree, force_allin_threshold_converts_large_bets) {
    TreeConfig c;
    c.starting_pot = 100;
    c.effective_stack = 120;
    c.initial_state = BoardState::River;
    const BetSizeOptions sizes = BetSizeOptions::parse("100%", "").value();
    c.river_bet_sizes = {sizes, sizes};
    c.force_allin_threshold = 0.15;

    ActionTree tree = build(c);
    // Betting 100 into 100 leaves an SPR of 20/300, below the threshold, so the
    // bet becomes an all-in.
    CHECK_EQ(actions_to_string(tree.available_actions()), std::string("[Check, AllIn(120)]"));
}

PFS_TEST(action_tree, add_allin_threshold_appends_allin) {
    TreeConfig c;
    c.starting_pot = 100;
    c.effective_stack = 100;
    c.initial_state = BoardState::River;
    const BetSizeOptions sizes = BetSizeOptions::parse("30%", "").value();
    c.river_bet_sizes = {sizes, sizes};

    c.add_allin_threshold = 0.0;
    CHECK_EQ(actions_to_string(build(c).available_actions()), std::string("[Check, Bet(30)]"));

    // max_amount (100) <= round(pot * 1.5) = 150, so an all-in is added.
    c.add_allin_threshold = 1.5;
    CHECK_EQ(actions_to_string(build(c).available_actions()),
             std::string("[Check, Bet(30), AllIn(100)]"));
}

PFS_TEST(action_tree, donk_sizes_apply_after_oop_call) {
    TreeConfig c;
    c.starting_pot = 100;
    c.effective_stack = 1000;
    c.initial_state = BoardState::Turn;
    const BetSizeOptions sizes = BetSizeOptions::parse("50%", "").value();
    c.turn_bet_sizes = {sizes, sizes};
    c.river_bet_sizes = {sizes, sizes};
    c.river_donk_sizes = DonkSizeOptions::parse("30%").value();

    ActionTree tree = build(c);
    // OOP checks, IP bets, OOP calls -> river, and OOP is now in a donk spot.
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.play(Action::bet(50)).is_ok());
    CHECK(tree.play(Action::call()).is_ok());
    CHECK(tree.is_chance_node());
    // 30% of the 200 pot.
    CHECK_EQ(actions_to_string(tree.available_actions()), std::string("[Check, Bet(60)]"));
}

PFS_TEST(action_tree, play_rejects_unavailable_action) {
    ActionTree tree = build(basic_config());
    CHECK(!tree.play(Action::fold()).is_ok());
    CHECK(!tree.play(Action::bet(1)).is_ok());
    CHECK(tree.play(Action::check()).is_ok());
    CHECK(tree.history().size() == 1u);
    CHECK(tree.apply_history(std::vector<Action>{Action::bet(120), Action::call()}).is_ok());
    CHECK(tree.history().size() == 2u);
    CHECK(!tree.apply_history(std::vector<Action>{Action::fold()}).is_ok());
}
