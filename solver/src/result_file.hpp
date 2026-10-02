// Builds the result json written by postflop-solver (format "postflop-solver/result",
// version 1):
//   - settings and solve: the solve parameters, the iterations used, the final
//     exploitability (also as a percentage of the starting pot) and the time.
//   - ev / equity: each player's average EV and equity at the root.
//   - nodes: the solved strategy of every action node on the first street (the
//     nodes that are reached without a card being dealt). Per hand: its weight at
//     the node (the range weight times the frequencies of the actions taken to
//     get there), the frequency and EV of each action, and its equity. Actions are
//     listed in the order of the game file. rangeFrequencies are the frequencies
//     over the whole range, taking card removal into account.
// Amounts and EVs are in the game file's units: GTO+ chips times chipScale. Hands
// are written with the higher card first, e.g. "AhKh".
#pragma once

#include "game_file.hpp"

#include <pfs/game.hpp>

#include <nlohmann/json.hpp>

namespace pfs_solver {

struct SolveSettings {
    uint32_t max_iterations = 0;
    double target_exploitability_percent = 0.0;  // of the starting pot
    bool compression = false;
};

struct SolveStats {
    uint32_t iterations = 0;
    float exploitability = 0.0f;
    double seconds = 0.0;
    unsigned threads = 1;
    uint64_t memory_bytes = 0;
};

// The game must be solved.
nlohmann::ordered_json build_result(pfs::PostFlopGame& game, const GameFile& file,
                                    const std::string& input_name, const SolveSettings& settings,
                                    const SolveStats& stats);

}  // namespace pfs_solver
