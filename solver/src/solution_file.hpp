// Writes the complete solved game as a solution file (format "postflop-solver/solution",
// version 1.0), specified in documentation/SolverFileFormat.md. The file holds every street and
// every dealt card, and can be read one node at a time with file seeks or HTTP range requests.
// solution_reader.hpp reads it back.
#pragma once

#include "game_file.hpp"
#include "result_file.hpp"
#include "solution_format.hpp"

#include <pfs/game.hpp>
#include <pfs/result.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace pfs_solver {

struct SolutionSettings {
    // The deepest street that is stored. Chance entries for later streets are NotStored.
    pfs::BoardState max_street = pfs::BoardState::River;
    // Store every value as a 32-bit float instead of as a 16-bit integer.
    bool full_precision = false;
    // Zlib or None. Zstd is defined by the format but not written.
    solution::Codec codec = solution::Codec::Zlib;
    // Leave out the subtree after an action the range takes less often than this.
    double prune_action_percent = 0.0;
    // Leave out nodes whose line (the product of the range frequencies of the actions leading
    // there) is played less often than this.
    double prune_line_percent = 0.0;
    // Called after each written segment with the number of segments written so far.
    std::function<void(uint64_t)> progress;
};

struct SolutionStats {
    uint64_t file_bytes = 0;
    uint64_t segments = 0;
    uint64_t stored_nodes = 0;
    uint64_t pruned_nodes = 0;  // slots marked Pruned in the segments that were written
    uint64_t pruned_exits = 0;  // chance nodes whose following segments were left out
};

// The game must be solved. The interpreter is used to walk the tree and is back at the root
// afterwards.
pfs::Result<SolutionStats> write_solution(pfs::PostFlopGame& game, const GameFile& file,
                                          const std::string& input_name,
                                          const SolveSettings& solve_settings,
                                          const SolveStats& solve_stats,
                                          const SolutionSettings& settings,
                                          const std::filesystem::path& path);

}  // namespace pfs_solver
