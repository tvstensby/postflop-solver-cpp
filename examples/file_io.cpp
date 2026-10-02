// Port of examples/file_io.rs. See basic.cpp for the setup explanation.
#include <pfs/file.hpp>
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pfs;

int main() {
    const char* oop_range = "66+,A8s+,A5s-A4s,AJo+,K9s+,KQo,QTs+,JTs,96s+,85s+,75s+,65s,54s";
    const char* ip_range = "QQ-22,AQs-A2s,ATo+,K5s+,KJo+,Q8s+,J8s+,T7s+,96s+,86s+,75s+,64s+,53s+";

    CardConfig card_config;
    card_config.range[0] = Range::parse(oop_range).value();
    card_config.range[1] = Range::parse(ip_range).value();
    card_config.flop = flop_from_str("Td9d6h").value();
    card_config.turn = card_from_str("Qc").value();
    card_config.river = NOT_DEALT;

    const BetSizeOptions bet_sizes = BetSizeOptions::parse("60%, e, a", "2.5x").value();

    TreeConfig tree_config;
    tree_config.initial_state = BoardState::Turn;
    tree_config.starting_pot = 200;
    tree_config.effective_stack = 900;
    tree_config.flop_bet_sizes = {bet_sizes, bet_sizes};
    tree_config.turn_bet_sizes = {bet_sizes, bet_sizes};
    tree_config.river_bet_sizes = {bet_sizes, bet_sizes};
    tree_config.river_donk_sizes = DonkSizeOptions::parse("50%").value();
    tree_config.add_allin_threshold = 1.5;
    tree_config.force_allin_threshold = 0.15;
    tree_config.merging_threshold = 0.1;

    Result<ActionTree> action_tree = ActionTree::create(std::move(tree_config));
    if (!action_tree.is_ok()) {
        std::printf("failed to build action tree: %s\n", action_tree.error().c_str());
        return 1;
    }
    Result<std::unique_ptr<PostFlopGame>> built =
        PostFlopGame::with_config(std::move(card_config), std::move(action_tree.value()));
    if (!built.is_ok()) {
        std::printf("failed to build game: %s\n", built.error().c_str());
        return 1;
    }
    PostFlopGame& game = *built.value();

    game.allocate_memory(false);
    const float target = static_cast<float>(game.tree_config().starting_pot) * 0.005f;
    solve(game, 1000, target, true);

    const char* path = "filename.bin";
    // The last argument is the zstd compression level; it needs PFS_ENABLE_ZSTD.
    Status saved = save_game_to_file(game, "memo string", path);
    if (!saved.is_ok()) {
        std::printf("failed to save: %s\n", saved.error().c_str());
        return 1;
    }

    // The second argument is a maximum memory usage in bytes.
    Result<LoadedGame> loaded = load_game_from_file(path);
    if (!loaded.is_ok()) {
        std::printf("failed to load: %s\n", loaded.error().c_str());
        return 1;
    }
    PostFlopGame& game2 = *loaded.value().game;

    // The reloaded tree must agree exactly: the counterfactual values were
    // recomputed from the saved strategy.
    game.cache_normalized_weights();
    game2.cache_normalized_weights();
    assert(game.equity(0) == game2.equity(0));

    // Discard everything after the river deal when serializing. This loses no
    // information about the tree itself, but a game loaded from the truncated file
    // cannot browse past the river deal.
    const Status narrowed = game2.set_target_storage_mode(BoardState::Turn);
    if (!narrowed.is_ok()) {
        std::printf("failed to narrow: %s\n", narrowed.error().c_str());
        return 1;
    }

    std::printf("Memory usage of the original game tree: %.2fMB\n",
                static_cast<double>(game.target_memory_usage()) / (1024.0 * 1024.0));
    std::printf("Memory usage of the truncated game tree: %.2fMB\n",
                static_cast<double>(game2.target_memory_usage()) / (1024.0 * 1024.0));

    if (!save_game_to_file(game2, "memo string", path).is_ok()) return 1;
    std::remove(path);

    std::printf("file_io: all assertions passed\n");
    return 0;
}
