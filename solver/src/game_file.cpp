#include "game_file.hpp"

#include "gto_range.hpp"

#include <nlohmann/json.hpp>

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

Result<GameFile> read_game_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Result<GameFile>::err("Could not open " + path.string());

    try {
        const json data = json::parse(stream);
        if (data.value("format", "") != "poker-tools/game")
            return Result<GameFile>::err(path.string() + " is not a poker-tools exporter game file");
        const int version = data.value("version", 0);
        if (version != 1 && version != 2)
            return Result<GameFile>::err("Unsupported game file version " +
                                         std::to_string(data.value("version", 0)));

        GameFile file;
        file.version = version;
        file.source = data.value("source", "");
        file.start_id = data.at("start").at("id").get<std::string>();
        file.chip_scale = data.at("chipScale").get<double>();
        file.flop = data.at("board").at("flop").get<std::string>();
        file.turn = optional_card(data.at("board").at("turn"));
        file.river = optional_card(data.at("board").at("river"));
        file.ranges[0] = data.at("ranges").at("OOP").get<std::string>();
        file.ranges[1] = data.at("ranges").at("IP").get<std::string>();

        const json& config = data.at("treeConfig");
        Result<pfs::BoardState> state = parse_board_state(config.at("initialState").get<std::string>());
        if (!state) return Result<GameFile>::err(state.error());
        file.tree_config.initial_state = state.value();
        file.tree_config.starting_pot = config.at("startingPot").get<int32_t>();
        file.tree_config.effective_stack = config.at("effectiveStack").get<int32_t>();
        file.tree_config.rake_rate = config.at("rakeRate").get<double>();
        file.tree_config.rake_cap = config.at("rakeCap").get<double>();

        for (const json& line : data.at("addLines")) {
            Result<std::vector<Action>> actions = parse_line(line);
            if (!actions) return Result<GameFile>::err(actions.error());
            file.add_lines.push_back(actions.value());
        }
        for (const json& line : data.at("removeLines")) {
            Result<std::vector<Action>> actions = parse_line(line);
            if (!actions) return Result<GameFile>::err(actions.error());
            file.remove_lines.push_back(actions.value());
        }

        for (const json& item : data.at("nodes")) {
            GameFileNode node;
            node.id = item.at("id").get<std::string>();
            node.street = item.at("street").get<std::string>();
            node.player = item.at("player").get<std::string>() == "OOP" ? 0 : 1;
            node.pot = item.at("pot").get<int64_t>();
            Result<std::vector<Action>> history = parse_line(item.at("history"));
            if (!history) return Result<GameFile>::err(history.error());
            node.history = history.value();
            node.actions = item.at("actions").get<std::vector<std::string>>();
            file.nodes.push_back(std::move(node));
        }
        return file;
    } catch (const json::exception& e) {
        return Result<GameFile>::err("Invalid game file " + path.string() + ": " + e.what());
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
        // Version 1 has ranges in Range::parse syntax, version 2 in GTO+ syntax.
        Result<pfs::Range> range = file.version == 1 ? pfs::Range::parse(file.ranges[player])
                                                     : parse_gto_range(file.ranges[player]);
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
