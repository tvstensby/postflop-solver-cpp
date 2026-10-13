#include "game_file.hpp"

#include "gto_range.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>

using nlohmann::json;
using pfs::Action;
using pfs::Result;

namespace pfs_solver {

namespace {

Result<std::vector<Action>> parse_line(const json& line) {
    std::vector<Action> actions;
    for (const json& item : line) {
        Result<Action> action = parse_action(item.get<std::string>());
        if (!action) return Result<std::vector<Action>>::err(action.error());
        actions.push_back(action.value());
    }
    return actions;
}

Result<pfs::BoardState> parse_board_state(const std::string& text) {
    if (text == "flop") return pfs::BoardState::Flop;
    if (text == "turn") return pfs::BoardState::Turn;
    if (text == "river") return pfs::BoardState::River;
    return Result<pfs::BoardState>::err("Unknown initial state: " + text);
}

std::string optional_card(const json& value) {
    return value.is_null() ? std::string() : value.get<std::string>();
}

std::string line_to_string(const std::vector<Action>& line) {
    return line.empty() ? "(root)" : pfs::actions_to_string(line);
}

// Reads the optional riskPremiumPercent of a file that carries ranges, as percentages. Absent
// leaves the premiums as they are, so a range file without them keeps the game file's.
pfs::Status read_risk_premium(const json& data, std::array<double, 2>& premium) {
    if (!data.contains("riskPremiumPercent")) return pfs::Status::ok();
    const json& item = data.at("riskPremiumPercent");
    const char* const names[2] = {"OOP", "IP"};
    for (size_t player = 0; player < 2; ++player) {
        const double value = item.value(names[player], 0.0);
        if (!(value >= 0.0 && value < 50.0))
            return pfs::Status::err(std::string("Risk premium of ") + names[player] +
                                    " must be at least 0 and below 50 percent: " +
                                    std::to_string(value));
        premium[player] = value;
    }
    return pfs::Status::ok();
}

}  // namespace

Result<Action> parse_action(const std::string& text) {
    if (text == "Fold") return Action::fold();
    if (text == "Check") return Action::check();
    if (text == "Call") return Action::call();

    const size_t open = text.find('(');
    if (open == std::string::npos || text.back() != ')' || open + 2 > text.size() - 1)
        return Result<Action>::err("Invalid action: " + text);
    const std::string kind = text.substr(0, open);
    const std::string number = text.substr(open + 1, text.size() - open - 2);
    if (number.find_first_not_of("0123456789") != std::string::npos || number.size() > 9)
        return Result<Action>::err("Invalid action amount: " + text);
    const int32_t amount = std::stoi(number);

    if (kind == "Bet") return Action::bet(amount);
    if (kind == "Raise") return Action::raise(amount);
    if (kind == "AllIn") return Action::all_in(amount);
    return Result<Action>::err("Invalid action: " + text);
}

namespace {

// The parts that game files and tree files have in common: chipScale, treeConfig, addLines,
// removeLines and nodes.
pfs::Status read_tree_parts(const json& data, GameFile& file) {
    file.chip_scale = data.at("chipScale").get<double>();

    const json& config = data.at("treeConfig");
    Result<pfs::BoardState> state = parse_board_state(config.at("initialState").get<std::string>());
    if (!state) return pfs::Status::err(state.error());
    file.tree_config.initial_state = state.value();
    file.tree_config.starting_pot = config.at("startingPot").get<int32_t>();
    file.tree_config.effective_stack = config.at("effectiveStack").get<int32_t>();
    file.tree_config.rake_rate = config.at("rakeRate").get<double>();
    file.tree_config.rake_cap = config.at("rakeCap").get<double>();

    for (const json& line : data.at("addLines")) {
        Result<std::vector<Action>> actions = parse_line(line);
        if (!actions) return pfs::Status::err(actions.error());
        file.add_lines.push_back(actions.value());
    }
    for (const json& line : data.at("removeLines")) {
        Result<std::vector<Action>> actions = parse_line(line);
        if (!actions) return pfs::Status::err(actions.error());
        file.remove_lines.push_back(actions.value());
    }

    for (const json& item : data.at("nodes")) {
        GameFileNode node;
        node.id = item.at("id").get<std::string>();
        node.street = item.at("street").get<std::string>();
        node.player = item.at("player").get<std::string>() == "OOP" ? 0 : 1;
        node.pot = item.at("pot").get<int64_t>();
        Result<std::vector<Action>> history = parse_line(item.at("history"));
        if (!history) return pfs::Status::err(history.error());
        node.history = history.value();
        node.actions = item.at("actions").get<std::vector<std::string>>();
        file.nodes.push_back(std::move(node));
    }
    return pfs::Status::ok();
}

}  // namespace

Result<GameFile> read_game_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Result<GameFile>::err("Could not open " + path.string());

    try {
        const json data = json::parse(stream);
        if (data.value("format", "") != "poker-tools/game")
            return Result<GameFile>::err(path.string() + " is not a poker-tools exporter game file");
        const int version = data.value("version", 0);
        if (version < 1 || version > 3)
            return Result<GameFile>::err("Unsupported game file version " +
                                         std::to_string(data.value("version", 0)));

        GameFile file;
        file.version = version;
        // Version 1 has ranges in Range::parse syntax, version 2 in GTO+ syntax.
        file.gto_ranges = version >= 2;
        file.source = data.value("source", "");
        file.start_id = data.at("start").at("id").get<std::string>();
        file.flop = data.at("board").at("flop").get<std::string>();
        file.turn = optional_card(data.at("board").at("turn"));
        file.river = optional_card(data.at("board").at("river"));
        file.ranges[0] = data.at("ranges").at("OOP").get<std::string>();
        file.ranges[1] = data.at("ranges").at("IP").get<std::string>();
        pfs::Status premium = read_risk_premium(data, file.risk_premium_percent);
        if (!premium) return Result<GameFile>::err(premium.error());

        pfs::Status tree = read_tree_parts(data, file);
        if (!tree) return Result<GameFile>::err(tree.error());
        return file;
    } catch (const json::exception& e) {
        return Result<GameFile>::err("Invalid game file " + path.string() + ": " + e.what());
    }
}

Result<GameFile> read_tree_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Result<GameFile>::err("Could not open " + path.string());

    try {
        const json data = json::parse(stream);
        if (data.value("format", "") != "poker-tools/tree")
            return Result<GameFile>::err(path.string() + " is not a poker-tools game tree file");
        if (data.value("version", 0) != 1)
            return Result<GameFile>::err("Unsupported game tree file version " +
                                         std::to_string(data.value("version", 0)));

        GameFile file;
        file.version = 2;
        file.gto_ranges = true;
        pfs::Status tree = read_tree_parts(data, file);
        if (!tree) return Result<GameFile>::err(tree.error());
        return file;
    } catch (const json::exception& e) {
        return Result<GameFile>::err("Invalid game tree file " + path.string() + ": " + e.what());
    }
}

pfs::Status set_board(GameFile& file, const std::string& board) {
    // Three, four or five cards, e.g. "KcQdJs" or "KcQdJsTh2c".
    if (board.size() != 6 && board.size() != 8 && board.size() != 10)
        return pfs::Status::err("Invalid board '" + board +
                                "': the flop, optionally followed by the turn and the river, is needed "
                                "(for example KcQdJs, KcQdJsTh or KcQdJsTh2c)");
    const size_t cards = board.size() / 2;
    const pfs::BoardState state = file.tree_config.initial_state;
    const size_t expected = state == pfs::BoardState::Flop ? 3 : state == pfs::BoardState::Turn ? 4 : 5;
    if (cards != expected)
        return pfs::Status::err("The tree starts on the " +
                                std::string(state == pfs::BoardState::Flop   ? "flop"
                                            : state == pfs::BoardState::Turn ? "turn"
                                                                             : "river") +
                                ", so the board needs " + std::to_string(expected) + " cards, not " +
                                std::to_string(cards));
    file.flop = board.substr(0, 6);
    file.turn = cards >= 4 ? board.substr(6, 2) : std::string();
    file.river = cards >= 5 ? board.substr(8, 2) : std::string();
    return pfs::Status::ok();
}

void apply_risk_premium(GameFile& file) {
    for (size_t player = 0; player < 2; ++player)
        file.tree_config.risk_premium[player] = file.risk_premium_percent[player] / 100.0;
}

double bubble_factor(double premium_percent) {
    const double premium = premium_percent / 100.0;
    return (1.0 + 2.0 * premium) / (1.0 - 2.0 * premium);
}

pfs::Status read_range_file(GameFile& file, const std::filesystem::path& path, std::string& warning) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return pfs::Status::err("Could not open " + path.string());

    try {
        const json data = json::parse(stream);
        if (data.value("format", "") != "poker-tools/range")
            return pfs::Status::err(path.string() + " is not a poker-tools range file");
        // Version 2 has the stacks and pot in chips, version 1 in hundredths of a chip. Only
        // their ratio is used, so both work.
        const int version = data.value("version", 0);
        if (version < 1 || version > 3)
            return pfs::Status::err("Unsupported range file version " +
                                    std::to_string(data.value("version", 0)));

        file.ranges[0] = data.at("ranges").at("OOP").get<std::string>();
        file.ranges[1] = data.at("ranges").at("IP").get<std::string>();
        file.gto_ranges = true;
        file.ranges_source = path.filename().string();
        // The ranges and the premiums they were solved under belong together, so a range file's
        // premiums replace the game file's just as its ranges do.
        pfs::Status premium = read_risk_premium(data, file.risk_premium_percent);
        if (!premium) return premium;

        // The pot and stacks are in the range file's own units, so compare the
        // stack-to-pot ratios. Range files written by hand may not have them.
        warning.clear();
        if (!data.contains("pot") || !data.contains("stacks")) return pfs::Status::ok();
        const double pot = data.at("pot").get<double>();
        const double stack = std::min(data.at("stacks").at("OOP").get<double>(),
                                      data.at("stacks").at("IP").get<double>());
        const double game_spr = static_cast<double>(file.tree_config.effective_stack) /
                                static_cast<double>(file.tree_config.starting_pot);
        const double spr = pot > 0.0 ? stack / pot : 0.0;
        if (std::abs(spr - game_spr) > 0.01 * game_spr) {
            char text[160];
            std::snprintf(text, sizeof(text),
                          "the stack-to-pot ratio of the range file (%.3f) differs from the game's (%.3f)",
                          spr, game_spr);
            warning = text;
        }
        return pfs::Status::ok();
    } catch (const json::exception& e) {
        return pfs::Status::err("Invalid range file " + path.string() + ": " + e.what());
    }
}

Result<pfs::ActionTree> build_action_tree(const GameFile& file) {
    Result<pfs::ActionTree> created = pfs::ActionTree::create(file.tree_config);
    if (!created) return created;
    pfs::ActionTree tree = std::move(created.value());

    for (const std::vector<Action>& line : file.add_lines) {
        pfs::Status status = tree.add_line(line);
        if (!status)
            return Result<pfs::ActionTree>::err("Could not add line " + line_to_string(line) + ": " +
                                                status.error());
    }
    for (const std::vector<Action>& line : file.remove_lines) {
        pfs::Status status = tree.remove_line(line);
        if (!status)
            return Result<pfs::ActionTree>::err("Could not remove line " + line_to_string(line) +
                                                ": " + status.error());
    }

    // Every node of the file must exist with the same actions and pot.
    for (const GameFileNode& node : file.nodes) {
        pfs::Status status = tree.apply_history(node.history);
        if (!status)
            return Result<pfs::ActionTree>::err("Node " + node.id + " (" +
                                                line_to_string(node.history) +
                                                ") is not in the tree: " + status.error());

        std::set<std::string> expected(node.actions.begin(), node.actions.end());
        std::set<std::string> actual;
        for (const Action& action : tree.available_actions()) actual.insert(action.to_string());
        if (expected != actual)
            return Result<pfs::ActionTree>::err("Node " + node.id + " has the actions " +
                                                pfs::actions_to_string(tree.available_actions()) +
                                                " in the tree");

        const std::array<int32_t, 2> bets = tree.total_bet_amount();
        const int64_t pot = int64_t{file.tree_config.starting_pot} + bets[0] + bets[1];
        if (pot != node.pot)
            return Result<pfs::ActionTree>::err("Node " + node.id + " has pot " + std::to_string(pot) +
                                                " in the tree, " + std::to_string(node.pot) +
                                                " in the file");
    }
    tree.back_to_root();

    if (!tree.invalid_terminals().empty())
        return Result<pfs::ActionTree>::err("The tree has lines that end without a terminal node");
    return tree;
}

Result<pfs::CardConfig> build_card_config(const GameFile& file) {
    pfs::CardConfig config;
    for (size_t player = 0; player < 2; ++player) {
        Result<pfs::Range> range = file.gto_ranges ? parse_gto_range(file.ranges[player])
                                                   : pfs::Range::parse(file.ranges[player]);
        if (!range)
            return Result<pfs::CardConfig>::err(std::string(player == 0 ? "OOP" : "IP") +
                                                " range: " + range.error());
        config.range[player] = range.value();
    }

    Result<std::array<pfs::Card, 3>> flop = pfs::flop_from_str(file.flop);
    if (!flop) return Result<pfs::CardConfig>::err("Flop: " + flop.error());
    config.flop = flop.value();
    if (!file.turn.empty()) {
        Result<pfs::Card> turn = pfs::card_from_str(file.turn);
        if (!turn) return Result<pfs::CardConfig>::err("Turn: " + turn.error());
        config.turn = turn.value();
    }
    if (!file.river.empty()) {
        Result<pfs::Card> river = pfs::card_from_str(file.river);
        if (!river) return Result<pfs::CardConfig>::err("River: " + river.error());
        config.river = river.value();
    }
    return config;
}

}  // namespace pfs_solver
