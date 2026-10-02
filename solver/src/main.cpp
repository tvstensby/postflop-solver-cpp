// postflop-solver: solves a game exported by poker-tools exporters
// and writes the solution as json. See game_file.hpp for the input and
// result_file.hpp for the output.
#include "game_file.hpp"
#include "result_file.hpp"

#include <core/thread_pool.hpp>
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

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
                 "Usage: postflop-solver <game.json> --output <result.json> [options]\n"
                 "\n"
                 "Solves a game exported by poker-tools and writes the solution as json.\n"
                 "  --output <file>             The result file (required).\n"
                 "  --iterations <n>            Maximum number of iterations (default: %u).\n"
                 "  --exploitability <percent>  Stop when the exploitability is at most this\n"
                 "                              percentage of the starting pot (default: %g).\n"
                 "  --compress                  Store the solver data as 16-bit integers, which\n"
                 "                              halves the memory use.\n"
                 "  --quiet                     Do not print progress.\n",
                 kDefaultIterations, kDefaultExploitabilityPercent);
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

int fail(const std::string& message) {
    std::fprintf(stderr, "Error: %s\n", message.c_str());
    return EXIT_FAILURE;
}

int run(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    SolveSettings settings;
    settings.max_iterations = kDefaultIterations;
    settings.target_exploitability_percent = kDefaultExploitabilityPercent;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            if (!parse_unsigned(argv[++i], settings.max_iterations))
                return fail(std::string("invalid number of iterations '") + argv[i] + "'");
        } else if (arg == "--exploitability" && i + 1 < argc) {
            if (!parse_percent(argv[++i], settings.target_exploitability_percent))
                return fail(std::string("invalid exploitability '") + argv[i] + "' (a percentage is needed)");
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
    if (input_path.empty() || output_path.empty()) {
        print_usage();
        return EXIT_FAILURE;
    }

    pfs::Result<GameFile> file = read_game_file(input_path);
    if (!file) return fail(file.error());
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
    if (!quiet)
        std::printf("Solving %s from %s: %zu OOP and %zu IP hands, %.1f MB, %u threads\n",
                    input_path.c_str(), file.value().start_id.c_str(), game.num_private_hands(0),
                    game.num_private_hands(1), static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0),
                    stats.threads);

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

    const nlohmann::ordered_json result =
        build_result(game, file.value(), std::filesystem::path(input_path).filename().string(), settings, stats);
    std::ofstream stream(output_path, std::ios::binary);
    if (!stream || !(stream << result.dump(2) << "\n")) return fail("could not write " + output_path);
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
