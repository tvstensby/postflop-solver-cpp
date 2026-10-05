// Writes solution files of small solved games, reads them back with SolutionReader and compares
// every node with the interpreter, which is navigated with the actual cards. Isomorphic cards are
// translated by the interpreter's own swaps, so this also checks the suit permutations of the file.
#include "harness.hpp"

#include "solution_file.hpp"
#include "solution_reader.hpp"

#include <pfs/game.hpp>
#include <pfs/solver.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pfs;
using namespace pfs_solver;

namespace {

const char* const kOop = "AA,KK,QQ,AK,T9s,98s,66,54s";
const char* const kIp = "AA,KK,JJ,AQs,KQ,T8s,87s,77";

Card card(const char* text) { return card_from_str(text).value(); }

using Deal = std::vector<Card>;  // the cards dealt on the way to a node, in order

struct Spot {
    const char* flop;
    const char* turn;  // nullptr if the game starts on the flop
    std::vector<Deal> deals;
};

// A monotone flop: clubs, hearts and spades are interchangeable, so the turn cards of those suits
// are isomorphic, and after a club turn the river cards of hearts and spades are too. No flop bets
// keep the tree small.
const Spot kMonotone{"Td9d6d", nullptr,
                     {{card("2c"), card("3h")}, {card("2h"), card("3c")}, {card("2h"), card("3s")},
                      {card("2s"), card("3h")}, {card("Ad"), card("Kh")}, {card("Ad"), card("Ks")},
                      {card("Qh"), card("Qs")}, {card("Qs"), card("Qc")}}};

// A game that starts on the turn: clubs and spades are interchangeable on the river.
const Spot kTurn{"Td9d6h", "2h", {{card("3c")}, {card("3s")}, {card("Kc")}, {card("Ks")}, {card("Ah")}}};

TreeConfig tree_config(const Spot& spot) {
    TreeConfig tc;
    tc.initial_state = spot.turn ? BoardState::Turn : BoardState::Flop;
    tc.starting_pot = 100;
    tc.effective_stack = 250;
    const BetSizeOptions raise = BetSizeOptions::parse("60%", "2.5x").value();
    const BetSizeOptions bet = BetSizeOptions::parse("75%", "").value();
    tc.turn_bet_sizes = spot.turn ? std::array{raise, raise} : std::array{bet, bet};
    tc.river_bet_sizes = {bet, bet};
    return tc;
}

// Solved once per spot and shared by the tests, which leave the interpreter at the root.
PostFlopGame& solved_game(const Spot& spot) {
    static std::map<const Spot*, std::unique_ptr<PostFlopGame>> games;
    std::unique_ptr<PostFlopGame>& game = games[&spot];
    if (!game) {
        CardConfig cc;
        cc.range[0] = Range::parse(kOop).value();
        cc.range[1] = Range::parse(kIp).value();
        cc.flop = flop_from_str(spot.flop).value();
        if (spot.turn) cc.turn = card(spot.turn);
        Result<ActionTree> tree = ActionTree::create(tree_config(spot));
        Result<std::unique_ptr<PostFlopGame>> built =
            PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
        game = std::move(built.value());
        game->allocate_memory(false);
        solve(*game, 20, 0.0f, false);
    }
    return *game;
}

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("pfs_solution_test_" + name + ".pfsol");
}

SolutionStats write(const Spot& spot, const SolutionSettings& settings, const std::filesystem::path& path) {
    GameFile file;
    file.version = 2;
    file.source = "test";
    file.start_id = "OOP:0";
    file.flop = spot.flop;
    file.turn = spot.turn ? spot.turn : "";
    file.ranges = {kOop, kIp};
    file.tree_config = tree_config(spot);
    SolveSettings solve_settings;
    solve_settings.max_iterations = 20;
    Result<SolutionStats> stats = write_solution(solved_game(spot), file, "test.json", solve_settings,
                                                 SolveStats{}, settings, path);
    if (!stats) ::pfs::test::report_failure(__FILE__, __LINE__, "write_solution: " + stats.error());
    return stats ? stats.value() : SolutionStats{};
}

std::unique_ptr<SolutionReader> open(const std::filesystem::path& path) {
    Result<std::unique_ptr<SolutionReader>> reader = SolutionReader::open(path);
    if (!reader) {
        ::pfs::test::report_failure(__FILE__, __LINE__, "open: " + reader.error());
        return nullptr;
    }
    return std::move(reader.value());
}

// The template nodes from the root to id.
std::vector<size_t> path_of(const nlohmann::json& tree, size_t id) {
    std::vector<size_t> path{id};
    while (!tree[path.back()]["parent"].is_null()) path.push_back(tree[path.back()]["parent"].get<size_t>());
    std::reverse(path.begin(), path.end());
    return path;
}

// The interpreter history that reaches a template node with the given cards.
std::vector<size_t> history_of(const nlohmann::json& tree, size_t id, const Deal& cards) {
    const std::vector<size_t> path = path_of(tree, id);
    std::vector<size_t> history;
    size_t dealt = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        if (tree[path[i - 1]]["kind"] == "chance")
            history.push_back(cards[dealt++]);
        else
            history.push_back(tree[path[i]]["parentAction"].get<size_t>());
    }
    return history;
}

// Calls fn(template id, cards) for every action and chance node, once for each distinct deal of
// as many cards as the node needs.
template <class Fn>
void for_each_node(const nlohmann::json& tree, const std::vector<Deal>& deals, const Fn& fn) {
    for (size_t id = 0; id < tree.size(); ++id) {
        if (tree[id]["kind"] == "terminal") continue;
        const std::vector<size_t> path = path_of(tree, id);
        const size_t needed = static_cast<size_t>(std::count_if(
            path.begin(), path.end() - 1, [&](size_t n) { return tree[n]["kind"] == "chance"; }));
        std::vector<Deal> seen;
        for (const Deal& deal : deals) {
            const Deal cards(deal.begin(), deal.begin() + static_cast<std::ptrdiff_t>(needed));
            if (std::find(seen.begin(), seen.end(), cards) != seen.end()) continue;
            seen.push_back(cards);
            fn(id, cards);
        }
    }
}

// The largest difference between two arrays, or infinity if their lengths differ.
double max_diff(std::span<const float> a, std::span<const float> b) {
    if (a.size() != b.size()) return INFINITY;
    double diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        diff = std::max(diff, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return diff;
}

double max_abs(std::span<const float> a) {
    double m = 0.0;
    for (float v : a) m = std::max(m, std::fabs(static_cast<double>(v)));
    return m;
}

std::vector<double> range_frequencies(PostFlopGame& game) {
    const size_t player = game.current_player();
    const std::vector<float> strategy = game.strategy();
    const std::span<const float> weights = game.normalized_weights(player);
    const size_t num_hands = weights.size();
    std::vector<double> out(strategy.size() / num_hands, 0.0);
    double total = 0.0;
    for (size_t h = 0; h < num_hands; ++h) {
        for (size_t a = 0; a < out.size(); ++a)
            out[a] += static_cast<double>(weights[h]) * strategy[a * num_hands + h];
        total += weights[h];
    }
    // A node no hand reaches has frequencies 0, as in the writer.
    for (double& f : out) f = total > 0.0 ? f / total : 0.0;
    return out;
}

// Compares every node of the file with the interpreter. Strategies and weights are only permuted
// between isomorphic boards, so they must be exact in f32 files; EVs and equities are summed in a
// different order there.
void check_round_trip(const Spot& spot, SolutionReader& reader, bool quantized) {
    PostFlopGame& game = solved_game(spot);
    const nlohmann::json& tree = reader.metadata()["tree"];
    size_t checked = 0;
    for_each_node(tree, spot.deals, [&](size_t id, const Deal& cards) {
        Result<SolutionReader::Node> loaded = reader.load(id, cards);
        if (!loaded) {
            ::pfs::test::report_failure(__FILE__, __LINE__, "load: " + loaded.error());
            return;
        }
        const SolutionReader::Node& node = loaded.value();
        CHECK(node.state == SolutionReader::State::Stored);
        if (node.state != SolutionReader::State::Stored) return;
        ++checked;

        game.apply_history(history_of(tree, id, cards));
        game.cache_normalized_weights();
        if (tree[id]["kind"] == "action") {
            const size_t player = game.current_player();
            CHECK_EQ(static_cast<size_t>(node.player), player);
            const std::vector<float> strategy = game.strategy();
            const std::vector<float> action_ev = game.expected_values_detail(player);
            CHECK_NEAR(max_diff(node.strategy, strategy), 0.0, quantized ? 2e-5 : 0.0);
            CHECK_NEAR(max_diff(node.action_ev, action_ev), 0.0,
                       quantized ? max_abs(action_ev) / 32767.0 + 1e-6 : 1e-3);
            const std::vector<double> frequencies = range_frequencies(game);
            CHECK_EQ(node.summary.size(), 6 + frequencies.size());
            for (size_t a = 0; a < frequencies.size() && 6 + a < node.summary.size(); ++a)
                CHECK_NEAR(node.summary[6 + a], frequencies[a], 1e-6);
            if (quantized) {
                const size_t num_hands = game.num_private_hands(player);
                for (size_t h = 0; h < num_hands; ++h) {
                    double sum = 0.0;
                    for (size_t a = 0; a < node.num_actions; ++a) sum += node.strategy[a * num_hands + h];
                    CHECK_NEAR(sum, 1.0, 1e-6);
                }
            }
        }
        for (size_t q = 0; q < 2; ++q) {
            const std::span<const float> weights = game.weights(q);
            const std::vector<float> ev = game.expected_values(q);
            const std::vector<float> equity = game.equity(q);
            CHECK_NEAR(max_diff(node.weight[q], weights), 0.0, quantized ? max_abs(weights) / 65535.0 + 1e-9 : 0.0);
            CHECK_NEAR(max_diff(node.ev[q], ev), 0.0, quantized ? max_abs(ev) / 32767.0 + 1e-6 : 1e-3);
            CHECK_NEAR(max_diff(node.equity[q], equity), 0.0, quantized ? 1.0 / 65535.0 : 1e-5);
        }
    });
    CHECK(checked > 50);
    game.back_to_root();
}

std::vector<char> file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

PFS_TEST(solution, round_trip_full_precision) {
    for (const Spot* spot : {&kMonotone, &kTurn}) {
        SolutionSettings settings;
        settings.full_precision = true;
        const std::filesystem::path path = temp_path("full");
        const SolutionStats stats = write(*spot, settings, path);
        CHECK(stats.segments > 1);
        CHECK_EQ(stats.pruned_nodes, uint64_t{0});
        CHECK_EQ(stats.file_bytes, static_cast<uint64_t>(std::filesystem::file_size(path)));
        if (std::unique_ptr<SolutionReader> reader = open(path)) check_round_trip(*spot, *reader, false);
        std::filesystem::remove(path);
    }
}

PFS_TEST(solution, round_trip_quantized) {
    for (const Spot* spot : {&kMonotone, &kTurn}) {
        const std::filesystem::path path = temp_path("u16");
        write(*spot, SolutionSettings{}, path);
        if (std::unique_ptr<SolutionReader> reader = open(path)) check_round_trip(*spot, *reader, true);
        std::filesystem::remove(path);
    }
}

PFS_TEST(solution, uncompressed_and_deterministic) {
    SolutionSettings settings;
    const std::filesystem::path a = temp_path("a");
    const std::filesystem::path b = temp_path("b");
    const std::filesystem::path raw = temp_path("raw");
    write(kTurn, settings, a);
    write(kTurn, settings, b);
    settings.codec = solution::Codec::None;
    write(kTurn, settings, raw);

    CHECK(file_bytes(a) == file_bytes(b));
    CHECK(std::filesystem::file_size(raw) > std::filesystem::file_size(a));
    if (std::unique_ptr<SolutionReader> reader = open(raw)) check_round_trip(kTurn, *reader, true);
    for (const auto& p : {a, b, raw}) std::filesystem::remove(p);
}

PFS_TEST(solution, stored_streets) {
    SolutionSettings settings;
    settings.max_street = BoardState::Turn;
    const std::filesystem::path path = temp_path("turn");
    write(kTurn, settings, path);
    if (std::unique_ptr<SolutionReader> reader = open(path)) {
        const nlohmann::json& tree = reader->metadata()["tree"];
        CHECK_EQ(reader->metadata()["storedStreets"].size(), size_t{1});
        size_t river_node = 0;
        for_each_node(tree, kTurn.deals, [&](size_t id, const Deal& cards) {
            const Result<SolutionReader::Node> node = reader->load(id, cards);
            CHECK(node.is_ok());
            if (!node) return;
            const bool river = tree[id]["street"] == "river";
            if (river) river_node = id;
            CHECK(node.value().state == (river ? SolutionReader::State::NotStored : SolutionReader::State::Stored));
        });
        // A card on the board cannot be dealt.
        const Deal on_board{card("Td")};
        const Result<SolutionReader::Node> impossible = reader->load(river_node, on_board);
        CHECK(impossible.is_ok() && impossible.value().state == SolutionReader::State::Impossible);
        CHECK(!reader->load(river_node).is_ok());  // the river card is missing
    }
    std::filesystem::remove(path);
}

// Pruned exactly where an action on the path is below the action threshold, or the product of
// the frequencies on the path is below the line threshold. Only represented cards are dealt, so
// the interpreter's frequencies are the ones the writer saw.
PFS_TEST(solution, pruning) {
    PostFlopGame& game = solved_game(kTurn);
    const std::vector<Deal> deals = {{card("3c")}, {card("Ah")}};
    const std::filesystem::path full_path = temp_path("unpruned");
    const uint64_t full_size = write(kTurn, SolutionSettings{}, full_path).file_bytes;
    std::filesystem::remove(full_path);

    for (const auto& [action_percent, line_percent] : {std::pair{20.0, 0.0}, std::pair{0.0, 10.0}}) {
        SolutionSettings settings;
        settings.prune_action_percent = action_percent;
        settings.prune_line_percent = line_percent;
        const std::filesystem::path path = temp_path("pruned");
        const SolutionStats stats = write(kTurn, settings, path);
        CHECK(stats.pruned_nodes > 0);
        CHECK(stats.file_bytes < full_size);

        std::unique_ptr<SolutionReader> reader = open(path);
        if (!reader) continue;
        const nlohmann::json& tree = reader->metadata()["tree"];
        CHECK_NEAR(reader->metadata()["pruning"]["minActionFrequencyPercent"].get<double>(), action_percent, 0.0);
        size_t pruned = 0;
        size_t stored = 0;
        for_each_node(tree, deals, [&](size_t id, const Deal& cards) {
            // Walk the path in the interpreter and decide whether the node should be pruned.
            bool expect_pruned = false;
            double line = 1.0;
            game.back_to_root();
            for (size_t action : history_of(tree, id, cards)) {
                if (!game.is_chance_node()) {
                    game.cache_normalized_weights();
                    const double frequency = range_frequencies(game)[action];
                    line *= frequency;
                    if (frequency < action_percent / 100.0 || line < line_percent / 100.0) expect_pruned = true;
                }
                game.play(action);
            }
            const Result<SolutionReader::Node> node = reader->load(id, cards);
            CHECK(node.is_ok());
            if (!node) return;
            CHECK(node.value().state ==
                  (expect_pruned ? SolutionReader::State::Pruned : SolutionReader::State::Stored));
            ++(expect_pruned ? pruned : stored);
        });
        CHECK(pruned > 0);
        CHECK(stored > 0);
        game.back_to_root();
        reader.reset();
        std::filesystem::remove(path);
    }
}
