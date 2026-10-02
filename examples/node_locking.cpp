// Port of examples/node_locking.rs.
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pfs;

namespace {

std::unique_ptr<PostFlopGame> build(const char* oop, const char* ip, int32_t pot) {
    CardConfig cc;
    cc.range[0] = Range::parse(oop).value();
    cc.range[1] = Range::parse(ip).value();
    cc.flop = flop_from_str("2s3h4d").value();
    cc.turn = card_from_str("6c").value();
    cc.river = card_from_str("7c").value();

    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = pot;
    tc.effective_stack = 10;
    tc.river_bet_sizes = {BetSizeOptions::parse("a", "").value(),
                          BetSizeOptions::parse("a", "").value()};

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    if (!tree.is_ok()) return nullptr;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    if (!g.is_ok()) return nullptr;
    return std::move(g.value());
}

void normal_node_locking() {
    auto game = build("AsAh,QsQh", "KsKh", 20);
    assert(game != nullptr);

    game->allocate_memory(false);

    // Locking must happen after allocating memory and before solving.
    game->play(1);  // OOP all-in
    game->lock_current_strategy(std::vector<float>{0.25f, 0.75f});  // IP: 25% fold, 75% call
    game->back_to_root();

    solve(*game, 1000, 0.001f, false);
    game->cache_normalized_weights();

    {
        const std::vector<float> s = game->strategy();
        assert(std::abs(s[0] - 1.0f) < 1e-3f);  // QQ always checks
        assert(std::abs(s[1] - 0.0f) < 1e-3f);  // AA never checks
        assert(std::abs(s[2] - 0.0f) < 1e-3f);  // QQ never jams
        assert(std::abs(s[3] - 1.0f) < 1e-3f);  // AA always jams
    }

    // Re-allocating resets Solved -> MemoryAllocated so the node can be re-locked.
    game->allocate_memory(false);
    game->play(1);
    game->lock_current_strategy(std::vector<float>{0.5f, 0.5f});  // IP: 50% fold, 50% call
    game->back_to_root();

    solve(*game, 1000, 0.001f, false);
    game->cache_normalized_weights();

    {
        const std::vector<float> s = game->strategy();
        assert(std::abs(s[0] - 0.0f) < 1e-3f);  // QQ never checks
        assert(std::abs(s[1] - 0.0f) < 1e-3f);  // AA never checks
        assert(std::abs(s[2] - 1.0f) < 1e-3f);  // QQ always bets
        assert(std::abs(s[3] - 1.0f) < 1e-3f);  // AA always bets
    }

    std::printf("normal_node_locking: passed\n");
}

void partial_node_locking() {
    auto game = build("AsAh,QsQh,JsJh", "KsKh", 10);
    assert(game != nullptr);

    game->allocate_memory(false);
    // Only JJ is locked (80% check, 20% all-in); QQ and AA still solve.
    game->lock_current_strategy(std::vector<float>{0.8f, 0.0f, 0.0f, 0.2f, 0.0f, 0.0f});

    solve(*game, 1000, 0.001f, false);
    game->cache_normalized_weights();

    const std::vector<float> s = game->strategy();
    assert(std::abs(s[0] - 0.8f) < 1e-3f);  // JJ checks 80% (locked)
    assert(std::abs(s[1] - 0.7f) < 1e-3f);  // QQ checks 70% (solved)
    assert(std::abs(s[2] - 0.0f) < 1e-3f);  // AA never checks
    assert(std::abs(s[3] - 0.2f) < 1e-3f);  // JJ bets 20% (locked)
    assert(std::abs(s[4] - 0.3f) < 1e-3f);  // QQ bets 30% (solved)
    assert(std::abs(s[5] - 1.0f) < 1e-3f);  // AA always bets

    std::printf("partial_node_locking: passed\n");
}

}  // namespace

int main() {
    normal_node_locking();
    partial_node_locking();
    return 0;
}
