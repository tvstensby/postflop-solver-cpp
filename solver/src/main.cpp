// postflop-solver: solves a game exported by poker-tools exporters and writes the solution as
// json, and optionally the complete solved tree as a solution file. See game_file.hpp for the
// input, result_file.hpp for the json and solution_file.hpp for the solution file.
#include "game_file.hpp"
#include "result_file.hpp"
#include "solution_file.hpp"

#include <core/thread_pool.hpp>
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pfs_solver;

namespace {

constexpr uint32_t kDefaultIterations = 1000;
constexpr double kDefaultExploitabilityPercent = 0.25;
// Exploitability is computed every this many iterations, as in pfs::solve.
constexpr uint32_t kCheckInterval = 10;

void print_usage() {
    std::fprintf(stderr,
                 "Usage: postflop-solver <game.json> [--output <result.json>] [--solution <file.pfsol>]\n"
                 "                       [options]\n"
                 "       postflop-solver --tree <tree.json> --ranges <ranges.json> --board <cards>\n"
                 "                       [--output <result.json>] [--solution <file.pfsol>] [options]\n"
                 "\n"
                 "Solves a game and writes the solution as json, and the complete solved tree as a\n"
                 "solution file. At least one output is required. The game is either a game file\n"
                 "exported by poker-tools (poker-tools/game), or put together from a game tree file\n"
                 "(poker-tools/tree), a range file (poker-tools/range) and a board.\n"
                 "  --tree <file>               The game tree file, instead of a game file.\n"
                 "  --board <cards>             The board with the game tree: the flop, and the turn\n"
                 "                              and river if the tree starts on them, for example\n"
                 "                              KcQdJs, KcQdJsTh or KcQdJsTh2c.\n"
                 "  --output <file>             The result file: the first street as json.\n"
                 "  --solution <file>           The solution file: every street and card\n"
                 "                              (documentation/SolverFileFormat.md).\n"
                 "  --ranges <file>             The ranges of a range file (poker-tools/range). With\n"
                 "                              a game file they replace the file's ranges, with a\n"
                 "                              game tree file they are required.\n"
                 "  --iterations <n>            Maximum number of iterations (default: %u).\n"
                 "  --exploitability <percent>  Stop when the exploitability is at most this\n"
                 "                              percentage of the starting pot (default: %g).\n"
                 "  --compress                  Store the solver data as 16-bit integers, which\n"
                 "                              halves the memory use.\n"
                 "  --risk-premium <oop> <ip>   The players' ICM risk premiums, in percent: the\n"
                 "                              extra equity each needs before stacking off. 2\n"
                 "                              means 52%% instead of 50%% for an all-in for stacks.\n"
                 "                              Below 50, 0 for chip EV (the default). Overrides\n"
                 "                              the riskPremiumPercent of the input files.\n"
                 "  --quiet                     Do not print progress.\n"
                 "\n"
                 "Solution file options:\n"
                 "  --solution-streets <street> The last street stored: flop, turn or river\n"
                 "                              (default: river).\n"
                 "  --solution-precision <p>    u16 (default) or f32, the exact values.\n"
                 "  --solution-codec <codec>    zlib (default) or none.\n"
                 "  --solution-prune <percent>  Leave out what follows an action the range takes\n"
                 "                              less often than this (default: 0, off).\n"
                 "  --solution-prune-line <percent>\n"
                 "                              Leave out lines played less often than this, the\n"
                 "                              product of the frequencies of their actions\n"
                 "                              (default: 0, off).\n",
                 kDefaultIterations, kDefaultExploitabilityPercent);
}

bool parse_street(const std::string& text, pfs::BoardState& street) {
    if (text == "flop") street = pfs::BoardState::Flop;
    else if (text == "turn") street = pfs::BoardState::Turn;
    else if (text == "river") street = pfs::BoardState::River;
    else return false;
    return true;
}

bool parse_unsigned(const std::string& text, uint32_t& value) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || parsed == 0 || parsed > 100000000UL) return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_percent(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return !text.empty() && *end == '\0' && value >= 0.0 && value <= 100.0;
}

// A risk premium is an equity share, so at 50% the bubble factor would be infinite.
bool parse_premium(const std::string& text, double& value) {
    return parse_percent(text, value) && value < 50.0;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "Error: %s\n", message.c_str());
    return EXIT_FAILURE;
}

int run(int argc, char* argv[]) {
    std::string input_path;  // the game file, or the game tree file with --tree
    std::string tree_path;
    std::string board;
    std::string output_path;
    std::string ranges_path;
    std::string solution_path;
    SolveSettings settings;
    settings.max_iterations = kDefaultIterations;
    settings.target_exploitability_percent = kDefaultExploitabilityPercent;
    SolutionSettings solution_settings;
    std::array<double, 2> risk_premium{};  // percentages, [OOP, IP]
    bool risk_premium_given = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--tree" && i + 1 < argc) {
            tree_path = argv[++i];
        } else if (arg == "--board" && i + 1 < argc) {
            board = argv[++i];
        } else if (arg == "--solution" && i + 1 < argc) {
            solution_path = argv[++i];
        } else if (arg == "--solution-streets" && i + 1 < argc) {
            if (!parse_street(argv[++i], solution_settings.max_street))
                return fail(std::string("invalid street '") + argv[i] + "' (flop, turn or river)");
        } else if (arg == "--solution-precision" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value != "u16" && value != "f32")
                return fail("invalid precision '" + value + "' (u16 or f32)");
            solution_settings.full_precision = value == "f32";
        } else if (arg == "--solution-codec" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value != "zlib" && value != "none") return fail("invalid codec '" + value + "' (zlib or none)");
            solution_settings.codec = value == "zlib" ? solution::Codec::Zlib : solution::Codec::None;
        } else if (arg == "--solution-prune" && i + 1 < argc) {
            if (!parse_percent(argv[++i], solution_settings.prune_action_percent))
                return fail(std::string("invalid frequency '") + argv[i] + "' (a percentage is needed)");
        } else if (arg == "--solution-prune-line" && i + 1 < argc) {
            if (!parse_percent(argv[++i], solution_settings.prune_line_percent))
                return fail(std::string("invalid frequency '") + argv[i] + "' (a percentage is needed)");
        } else if (arg == "--ranges" && i + 1 < argc) {
            ranges_path = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            if (!parse_unsigned(argv[++i], settings.max_iterations))
                return fail(std::string("invalid number of iterations '") + argv[i] + "'");
        } else if (arg == "--exploitability" && i + 1 < argc) {
            if (!parse_percent(argv[++i], settings.target_exploitability_percent))
                return fail(std::string("invalid exploitability '") + argv[i] + "' (a percentage is needed)");
        } else if (arg == "--risk-premium" && i + 2 < argc) {
            for (size_t player = 0; player < 2; ++player) {
                const std::string value = argv[++i];
                if (!parse_premium(value, risk_premium[player]))
                    return fail("invalid " + std::string(player == 0 ? "OOP" : "IP") +
                                " risk premium '" + value +
                                "' (a percentage of at least 0 and below 50 is needed)");
            }
            risk_premium_given = true;
        } else if (arg == "--compress") {
            settings.compression = true;
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return EXIT_SUCCESS;
        } else if (input_path.empty() && arg.rfind("-", 0) != 0) {
            input_path = arg;
        } else {
            print_usage();
            return EXIT_FAILURE;
        }
    }
    if (output_path.empty() && solution_path.empty()) {
        print_usage();
        return EXIT_FAILURE;
    }
    const bool from_tree = !tree_path.empty();
    if (from_tree) {
        if (!input_path.empty()) return fail("a game file cannot be combined with --tree");
        if (ranges_path.empty()) return fail("--tree needs the ranges of a range file (--ranges)");
        if (board.empty()) return fail("--tree needs the board (--board), for example KcQdJs");
        input_path = tree_path;
    } else {
        if (input_path.empty()) {
            print_usage();
            return EXIT_FAILURE;
        }
        if (!board.empty()) return fail("--board is only used with --tree, a game file has its board");
    }

    pfs::Result<GameFile> file = from_tree ? read_tree_file(input_path) : read_game_file(input_path);
    if (!file) return fail(file.error());
    if (from_tree) {
        const pfs::Status status = set_board(file.value(), board);
        if (!status) return fail(status.error());
    }
    if (!ranges_path.empty()) {
        std::string warning;
        pfs::Status status = read_range_file(file.value(), ranges_path, warning);
        if (!status) return fail(status.error());
        if (!warning.empty()) std::fprintf(stderr, "Warning: %s\n", warning.c_str());
    }
    // The command line wins over the premiums the input files carry.
    if (risk_premium_given) file.value().risk_premium_percent = risk_premium;
    apply_risk_premium(file.value());

    pfs::Result<pfs::ActionTree> tree = build_action_tree(file.value());
    if (!tree) return fail(tree.error());
    pfs::Result<pfs::CardConfig> cards = build_card_config(file.value());
    if (!cards) return fail(cards.error());
    pfs::Result<std::unique_ptr<pfs::PostFlopGame>> built =
        pfs::PostFlopGame::with_config(std::move(cards.value()), std::move(tree.value()));
    if (!built) return fail(built.error());
    pfs::PostFlopGame& game = *built.value();

    SolveStats stats;
    stats.threads = pfs::ThreadPool::global().num_threads();
    const std::pair<uint64_t, uint64_t> memory = game.memory_usage();
    stats.memory_bytes = settings.compression ? memory.second : memory.first;
    const double pot = file.value().tree_config.starting_pot;
    if (!quiet) {
        const std::string board_text = file.value().flop + file.value().turn + file.value().river;
        std::printf("Solving %s on %s%s%s: %zu OOP and %zu IP hands, %.1f MB, %u threads\n",
                    input_path.c_str(), board_text.c_str(), from_tree ? "" : " from ",
                    from_tree ? "" : file.value().start_id.c_str(), game.num_private_hands(0),
                    game.num_private_hands(1), static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0),
                    stats.threads);
        if (game.has_risk_premium()) {
            const std::array<double, 2>& premium = file.value().risk_premium_percent;
            std::printf("Risk premium: OOP %g%% (bubble factor %.4f), IP %g%% (%.4f). EVs are "
                        "risk-adjusted, not chips.\n",
                        premium[0], bubble_factor(premium[0]), premium[1],
                        bubble_factor(premium[1]));
        }
    }

    const auto start = std::chrono::steady_clock::now();
    game.allocate_memory(settings.compression);

    // The loop of pfs::solve, with progress as a percentage of the pot and the
    // number of iterations kept.
    const float target = static_cast<float>(pot * settings.target_exploitability_percent / 100.0);
    float exploitability = pfs::compute_exploitability(game);
    uint32_t iteration = 0;
    while (iteration < settings.max_iterations && exploitability > target) {
        pfs::solve_step(game, iteration);
        ++iteration;
        if (iteration % kCheckInterval == 0 || iteration == settings.max_iterations) {
            exploitability = pfs::compute_exploitability(game);
            if (!quiet) {
                std::printf("\riteration %u / %u: exploitability %.3f%% of pot", iteration,
                            settings.max_iterations, 100.0 * exploitability / pot);
                std::fflush(stdout);
            }
        }
    }
    pfs::finalize(game);
    stats.iterations = iteration;
    stats.exploitability = exploitability;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!quiet)
        std::printf("%sSolved in %u iterations, %.1f seconds: exploitability %.3f%% of pot\n",
                    iteration > 0 ? "\n" : "", iteration, stats.seconds, 100.0 * exploitability / pot);

    const std::string input_name = std::filesystem::path(input_path).filename().string();
    if (!output_path.empty()) {
        const nlohmann::ordered_json result = build_result(game, file.value(), input_name, settings, stats);
        std::ofstream stream(output_path, std::ios::binary);
        if (!stream || !(stream << result.dump(2) << "\n")) return fail("could not write " + output_path);
    }

    if (!solution_path.empty()) {
        if (!quiet) {
            solution_settings.progress = [](uint64_t segments) {
                if (segments % 100 == 0) {
                    std::printf("\rWriting the solution: %llu segments", static_cast<unsigned long long>(segments));
                    std::fflush(stdout);
                }
            };
        }
        const auto solution_start = std::chrono::steady_clock::now();
        const pfs::Result<SolutionStats> written = write_solution(
            game, file.value(), input_name, settings, stats, solution_settings, solution_path);
        if (!written) return fail(written.error());
        if (!quiet) {
            const SolutionStats& s = written.value();
            std::printf("\rWrote %s in %.1f seconds: %.1f MB, %llu segments, %llu nodes, %llu pruned\n",
                        solution_path.c_str(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - solution_start).count(),
                        static_cast<double>(s.file_bytes) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(s.segments),
                        static_cast<unsigned long long>(s.stored_nodes),
                        static_cast<unsigned long long>(s.pruned_nodes + s.pruned_exits));
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
