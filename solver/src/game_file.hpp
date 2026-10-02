// Reads the game files written by poker-tools exporters and builds the corresponding game.
// Format: "poker-tools/game" version 1
//
// The file describes the game in this library's own terms:
//   - treeConfig: initial state, starting pot, effective stack and rake. The tree
//     is created without bet sizes, so every node starts with check, or fold and
//     call, and the bets are added from addLines.
//   - addLines / removeLines: lines in Action::to_string notation ("Bet(4500)",
//     "Raise(10500)", "AllIn(40000)", "Call", ...), applied in order.
//   - ranges: range strings in Range::parse syntax.
//   - nodes: every action node with its history, actions and pot, used to check
//     that the tree was rebuilt as intended.
// All amounts are integers: GTO+ chips times chipScale.
#pragma once

#include <pfs/action_tree.hpp>
#include <pfs/card_config.hpp>
#include <pfs/result.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace pfs_solver {

struct GameFileNode {
    std::string id;      // GTO+ node id, e.g. "OOP:6"
    std::string street;  // "flop", "turn" or "river"
    size_t player = 0;
    int64_t pot = 0;
    std::vector<pfs::Action> history;
    std::vector<std::string> actions;  // in the order of the file
};

struct GameFile {
    std::string source;  // the GTO+ file the game was exported from
    std::string start_id;
    double chip_scale = 1.0;
    std::string flop;
    std::string turn;   // empty if not dealt
    std::string river;  // empty if not dealt
    std::array<std::string, 2> ranges;  // [OOP, IP]
    pfs::TreeConfig tree_config;
    std::vector<std::vector<pfs::Action>> add_lines;
    std::vector<std::vector<pfs::Action>> remove_lines;
    std::vector<GameFileNode> nodes;
};

pfs::Result<GameFile> read_game_file(const std::filesystem::path& path);

// Builds the action tree and checks every node of the file against it.
pfs::Result<pfs::ActionTree> build_action_tree(const GameFile& file);

pfs::Result<pfs::CardConfig> build_card_config(const GameFile& file);

// Parses Action::to_string notation.
pfs::Result<pfs::Action> parse_action(const std::string& text);

}  // namespace pfs_solver
