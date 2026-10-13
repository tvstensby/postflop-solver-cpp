// Putting a game together from a game tree file, a range file and a board, instead of reading a
// game file.
#include "harness.hpp"

#include "game_file.hpp"

#include <pfs/game.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pfs;
using namespace pfs_solver;

namespace {

// A tree that starts on the given street, with a bet after a check on the first street. The node
// list has the root and the node after the check.
std::string tree_json(const char* street) {
    std::string text = R"json({
  "format": "poker-tools/tree", "version": 1, "rules": "test.json", "chipScale": 1000.0,
  "treeConfig": { "initialState": "STREET", "startingPot": 6500, "effectiveStack": 42000,
                  "rakeRate": 0.0, "rakeCap": 0.0 },
  "addLines": [ ["Bet(3028)"], ["Check", "Bet(3028)"] ],
  "removeLines": [],
  "nodes": [
    { "id": "N0", "street": "STREET", "player": "OOP", "pot": 6500, "history": [],
      "actions": ["Check", "Bet(3028)"], "sizes": [] },
    { "id": "N1", "street": "STREET", "player": "IP", "pot": 6500, "history": ["Check"],
      "actions": ["Check", "Bet(3028)"], "sizes": [] }
  ]
})json";
    for (size_t at; (at = text.find("STREET")) != std::string::npos;) text.replace(at, 6, street);
    return text;
}

std::string range_json(bool with_pot, const char* risk_premium = nullptr) {
    return std::string(R"json({ "format": "poker-tools/range", "version": 2,
  "positions": { "OOP": "MP", "IP": "LP" },
  "ranges": { "OOP": "AA,KK,[50.0]QQ[/50.0]", "IP": "AA-JJ,AKs" })json") +
           (with_pot ? R"json(, "stacks": { "OOP": 84.0, "IP": 84.0 }, "pot": 13.0)json" : "") +
           (risk_premium ? std::string(", \"riskPremiumPercent\": ") + risk_premium : "") + " }";
}

// A game file, which carries its own board and ranges.
std::string game_json(const char* risk_premium = nullptr) {
    return std::string(R"json({
  "format": "poker-tools/game", "version": 2, "source": "test.gto", "chipScale": 1000.0,
  "start": { "id": "OOP:0", "street": "flop", "line": [] },
  "board": { "flop": "KcQdJs", "turn": null, "river": null },
  "ranges": { "OOP": "AA,KK", "IP": "QQ,JJ" },
  "treeConfig": { "initialState": "flop", "startingPot": 6500, "effectiveStack": 42000,
                  "rakeRate": 0.0, "rakeCap": 0.0 },
  "addLines": [], "removeLines": [], "nodes": [])json") +
           (risk_premium ? std::string(", \"riskPremiumPercent\": ") + risk_premium : "") + " }";
}

std::filesystem::path write_file(const std::string& name, const std::string& text) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("pfs_game_file_test_" + name);
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

}  // namespace

PFS_TEST(game_file, tree_ranges_and_board_make_a_game) {
    const std::filesystem::path tree_path = write_file("tree.json", tree_json("flop"));
    const std::filesystem::path range_path = write_file("range.json", range_json(false));

    Result<GameFile> file = read_tree_file(tree_path);
    CHECK(file.is_ok());
    if (!file) return;
    CHECK(file.value().flop.empty());
    CHECK(file.value().tree_config.starting_pot == 6500);
    CHECK(file.value().chip_scale == 1000.0);
    CHECK(file.value().nodes.size() == 2);

    CHECK(set_board(file.value(), "KcQdJs").is_ok());
    CHECK(file.value().flop == "KcQdJs");
    CHECK(file.value().turn.empty() && file.value().river.empty());

    // A range file without the stacks and pot is fine, and gives no warning.
    std::string warning = "unchanged";
    CHECK(read_range_file(file.value(), range_path, warning).is_ok());
    CHECK(warning.empty());
    CHECK(file.value().ranges[0] == "AA,KK,[50.0]QQ[/50.0]");
    CHECK(file.value().ranges_source == "pfs_game_file_test_range.json");

    Result<ActionTree> tree = build_action_tree(file.value());
    CHECK(tree.is_ok());
    Result<CardConfig> cards = build_card_config(file.value());
    CHECK(cards.is_ok());
    if (!tree || !cards) return;
    Result<std::unique_ptr<PostFlopGame>> game =
        PostFlopGame::with_config(std::move(cards.value()), std::move(tree.value()));
    CHECK(game.is_ok());
    if (game) CHECK(game.value()->num_private_hands(0) > 0 && game.value()->num_private_hands(1) > 0);

    std::filesystem::remove(tree_path);
    std::filesystem::remove(range_path);
}

PFS_TEST(game_file, range_file_with_pot_checks_the_stack_to_pot_ratio) {
    const std::filesystem::path tree_path = write_file("tree2.json", tree_json("flop"));
    const std::filesystem::path range_path = write_file("range2.json", range_json(true));
    Result<GameFile> file = read_tree_file(tree_path);
    CHECK(file.is_ok());
    if (file) {
        // 84 / 13 = 6.46 against 42000 / 6500 = 6.46.
        std::string warning;
        CHECK(read_range_file(file.value(), range_path, warning).is_ok());
        CHECK(warning.empty());
        file.value().tree_config.effective_stack = 20000;
        CHECK(read_range_file(file.value(), range_path, warning).is_ok());
        CHECK(!warning.empty());
    }
    std::filesystem::remove(tree_path);
    std::filesystem::remove(range_path);
}

PFS_TEST(game_file, board_must_match_the_street_the_tree_starts_on) {
    const char* const streets[] = {"flop", "turn", "river"};
    const char* const boards[] = {"KcQdJs", "KcQdJsTh", "KcQdJsTh2c"};
    for (int street = 0; street < 3; ++street) {
        const std::filesystem::path path = write_file("street.json", tree_json(streets[street]));
        Result<GameFile> file = read_tree_file(path);
        CHECK(file.is_ok());
        if (file) {
            for (int board = 0; board < 3; ++board) {
                GameFile copy = file.value();
                CHECK_EQ(set_board(copy, boards[board]).is_ok(), street == board);
            }
            GameFile copy = file.value();
            CHECK(!set_board(copy, "").is_ok());
            CHECK(!set_board(copy, "KcQd").is_ok());
            CHECK(!set_board(copy, "KcQdJsT").is_ok());
        }
        std::filesystem::remove(path);
    }

    // A turn tree: the cards are split into the flop and the turn.
    const std::filesystem::path path = write_file("turn.json", tree_json("turn"));
    Result<GameFile> file = read_tree_file(path);
    if (file) {
        CHECK(set_board(file.value(), "KcQdJsTh").is_ok());
        CHECK(file.value().flop == "KcQdJs" && file.value().turn == "Th" && file.value().river.empty());
    }
    std::filesystem::remove(path);
}

// The premium travels with the ranges, because the preflop ranges are normally solved under it:
// a game file can carry it, and a range file's premium replaces it just as its ranges do.
PFS_TEST(game_file, risk_premium_comes_from_the_files_that_carry_ranges) {
    const std::filesystem::path plain_game = write_file("g1.json", game_json());
    const std::filesystem::path premium_game = write_file("g2.json", game_json(R"({"OOP": 2.0, "IP": 7.5})"));
    const std::filesystem::path plain_range = write_file("r1.json", range_json(false));
    const std::filesystem::path premium_range = write_file("r2.json", range_json(false, R"({"OOP": 1.0, "IP": 3.0})"));

    // Absent means chip EV, and the stored fractions stay 0.
    Result<GameFile> none = read_game_file(plain_game);
    CHECK(none.is_ok());
    if (none) {
        CHECK_NEAR(none.value().risk_premium_percent[0], 0.0, 0.0);
        apply_risk_premium(none.value());
        CHECK_NEAR(none.value().tree_config.risk_premium[1], 0.0, 0.0);
    }

    // Percent in the file, fraction in the tree configuration, [OOP, IP] throughout.
    Result<GameFile> from_game = read_game_file(premium_game);
    CHECK(from_game.is_ok());
    if (from_game) {
        CHECK_NEAR(from_game.value().risk_premium_percent[0], 2.0, 0.0);
        CHECK_NEAR(from_game.value().risk_premium_percent[1], 7.5, 0.0);
        apply_risk_premium(from_game.value());
        CHECK_NEAR(from_game.value().tree_config.risk_premium[0], 0.02, 1e-12);
        CHECK_NEAR(from_game.value().tree_config.risk_premium[1], 0.075, 1e-12);

        // A range file without premiums leaves the game file's in place.
        std::string warning;
        CHECK(read_range_file(from_game.value(), plain_range, warning).is_ok());
        CHECK_NEAR(from_game.value().risk_premium_percent[0], 2.0, 0.0);
        // One with premiums replaces them.
        CHECK(read_range_file(from_game.value(), premium_range, warning).is_ok());
        CHECK_NEAR(from_game.value().risk_premium_percent[0], 1.0, 0.0);
        CHECK_NEAR(from_game.value().risk_premium_percent[1], 3.0, 0.0);
    }

    // Out of range is rejected rather than clamped: at 50 the bubble factor is infinite.
    const std::filesystem::path bad = write_file("g3.json", game_json(R"({"OOP": 50.0, "IP": 0.0})"));
    CHECK(!read_game_file(bad).is_ok());
    const std::filesystem::path negative = write_file("g4.json", game_json(R"({"OOP": 0.0, "IP": -1.0})"));
    CHECK(!read_game_file(negative).is_ok());

    CHECK_NEAR(bubble_factor(0.0), 1.0, 0.0);
    CHECK_NEAR(bubble_factor(2.0), 1.0 + 4.0 * 0.02 / (1.0 - 2.0 * 0.02), 1e-12);

    for (const auto& p : {plain_game, premium_game, plain_range, premium_range, bad, negative})
        std::filesystem::remove(p);
}

PFS_TEST(game_file, wrong_kind_of_file_is_refused) {
    const std::filesystem::path tree_path = write_file("tree3.json", tree_json("flop"));
    const std::filesystem::path range_path = write_file("range3.json", range_json(false));
    CHECK(!read_tree_file(range_path).is_ok());
    CHECK(!read_game_file(tree_path).is_ok());
    GameFile file;
    std::string warning;
    CHECK(!read_range_file(file, tree_path, warning).is_ok());
    CHECK(!read_tree_file("does-not-exist.json").is_ok());
    std::filesystem::remove(tree_path);
    std::filesystem::remove(range_path);
}
