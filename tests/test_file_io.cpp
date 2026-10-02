// Port of src/file.rs::save_and_load_file.
#include "harness.hpp"

#include <pfs/file.hpp>
#include <pfs/game.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <cstdio>
#include <memory>
#include <vector>

using namespace pfs;

namespace {

std::unique_ptr<PostFlopGame> make_game() {
    CardConfig cc;
    cc.range[0] = Range::ones();
    cc.range[1] = Range::ones();
    cc.flop = flop_from_str("Td9d6h").value();

    TreeConfig tc;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    tc.flop_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};
    tc.turn_bet_sizes = {BetSizeOptions::parse("50%", "").value(), BetSizeOptions{}};

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    if (!tree.is_ok()) return nullptr;
    Result<std::unique_ptr<PostFlopGame>> g =
        PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    if (!g.is_ok()) return nullptr;
    return std::move(g.value());
}

struct Averages {
    float eq_oop, eq_ip, ev_oop, ev_ip;
};

Averages measure(PostFlopGame& game) {
    game.cache_normalized_weights();
    return {compute_average(game.equity(0), game.normalized_weights(0)),
            compute_average(game.equity(1), game.normalized_weights(1)),
            compute_average(game.expected_values(0), game.normalized_weights(0)),
            compute_average(game.expected_values(1), game.normalized_weights(1))};
}

void check_root(PostFlopGame& game) {
    const Averages a = measure(game);
    CHECK_NEAR(a.eq_oop, 0.5f, 1e-5);
    CHECK_NEAR(a.eq_ip, 0.5f, 1e-5);
    CHECK_NEAR(a.ev_oop, 45.0f, 1e-4);
    CHECK_NEAR(a.ev_ip, 15.0f, 1e-4);
}

}  // namespace

// Round-trips through River, then Turn, then Flop target storage modes, checking
// the same four numbers each time. Narrowing the target discards the deeper
// streets' cfvalues, which finalize() regenerates on load.
PFS_TEST(file_io, save_and_load_round_trip) {
    auto game = make_game();
    if (!game) {
        CHECK(false);
        return;
    }

    game->allocate_memory(false);
    finalize(*game);
    check_root(*game);

    // --- River (the full tree) ---
    Result<std::vector<std::byte>> bytes = save_game_to_bytes(*game, "memo string");
    CHECK(bytes.is_ok());
    if (!bytes.is_ok()) return;

    Result<LoadedGame> loaded = load_game_from_bytes(bytes.value().data(), bytes.value().size());
    CHECK(loaded.is_ok());
    if (!loaded.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, loaded.error());
        return;
    }
    CHECK_EQ(loaded.value().memo, std::string("memo string"));
    PostFlopGame& g2 = *loaded.value().game;
    CHECK(g2.is_solved());
    check_root(g2);

    // The reloaded game must agree bit-for-bit, which is a strong statement: the
    // cfvalues were recomputed from the saved strategy.
    game->cache_normalized_weights();
    g2.cache_normalized_weights();
    const std::vector<float> e1 = game->equity(0);
    const std::vector<float> e2 = g2.equity(0);
    CHECK_EQ(e1.size(), e2.size());
    for (size_t i = 0; i < e1.size() && i < e2.size(); ++i) CHECK_EQ(e1[i], e2[i]);

    // --- Turn ---
    CHECK(g2.set_target_storage_mode(BoardState::Turn).is_ok());
    CHECK(g2.target_memory_usage() < game->target_memory_usage());

    Result<std::vector<std::byte>> b3 = save_game_to_bytes(g2, "turn");
    CHECK(b3.is_ok());
    if (!b3.is_ok()) return;
    Result<LoadedGame> l3 = load_game_from_bytes(b3.value().data(), b3.value().size());
    CHECK(l3.is_ok());
    if (!l3.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, l3.error());
        return;
    }
    PostFlopGame& g3 = *l3.value().game;
    CHECK(g3.storage_mode() == BoardState::Turn);
    check_root(g3);

    // --- Flop ---
    CHECK(g3.set_target_storage_mode(BoardState::Flop).is_ok());
    Result<std::vector<std::byte>> b4 = save_game_to_bytes(g3, "flop");
    CHECK(b4.is_ok());
    if (!b4.is_ok()) return;
    Result<LoadedGame> l4 = load_game_from_bytes(b4.value().data(), b4.value().size());
    CHECK(l4.is_ok());
    if (!l4.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, l4.error());
        return;
    }
    PostFlopGame& g4 = *l4.value().game;
    CHECK(g4.storage_mode() == BoardState::Flop);
    check_root(g4);
}

PFS_TEST(file_io, target_storage_mode_validation) {
    auto game = make_game();
    if (!game) return;
    game->allocate_memory(false);
    finalize(*game);

    CHECK(game->storage_mode() == BoardState::River);
    CHECK(game->target_storage_mode() == BoardState::River);

    CHECK(game->set_target_storage_mode(BoardState::Turn).is_ok());
    CHECK(game->set_target_storage_mode(BoardState::Flop).is_ok());
    // Cannot exceed what is actually stored.
    CHECK(game->set_target_storage_mode(BoardState::River).is_ok());

    // A turn-initial game cannot target the flop.
    CardConfig cc;
    cc.range[0] = Range::ones();
    cc.range[1] = Range::ones();
    cc.flop = flop_from_str("Td9d6h").value();
    cc.turn = card_from_str("Qc").value();
    TreeConfig tc;
    tc.initial_state = BoardState::Turn;
    tc.starting_pot = 60;
    tc.effective_stack = 970;
    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    auto g = PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    g.value()->allocate_memory(false);
    finalize(*g.value());
    CHECK(!g.value()->set_target_storage_mode(BoardState::Flop).is_ok());
}

PFS_TEST(file_io, rejects_bad_input) {
    auto game = make_game();
    if (!game) return;

    // An unsolved game cannot be saved.
    game->allocate_memory(false);
    CHECK(!save_game_to_bytes(*game, "x").is_ok());

    finalize(*game);
    Result<std::vector<std::byte>> bytes = save_game_to_bytes(*game, "x");
    CHECK(bytes.is_ok());
    if (!bytes.is_ok()) return;

    // Wrong magic: a file written by the Rust solver must be rejected, not
    // mis-parsed. 0x09f15790 is the Rust magic.
    std::vector<std::byte> wrong = bytes.value();
    const uint32_t rust_magic = 0x09f15790;
    std::memcpy(wrong.data(), &rust_magic, sizeof rust_magic);
    CHECK(!load_game_from_bytes(wrong.data(), wrong.size()).is_ok());

    // Wrong version.
    std::vector<std::byte> bad_ver = bytes.value();
    const uint16_t v = 999;
    std::memcpy(bad_ver.data() + 4, &v, sizeof v);
    CHECK(!load_game_from_bytes(bad_ver.data(), bad_ver.size()).is_ok());

    // The memory guard is checked before the payload is read.
    CHECK(!load_game_from_bytes(bytes.value().data(), bytes.value().size(), uint64_t{1}).is_ok());
    CHECK(load_game_from_bytes(bytes.value().data(), bytes.value().size(),
                               uint64_t{1} << 40)
              .is_ok());

    // Truncated input.
    CHECK(!load_game_from_bytes(bytes.value().data(), 8).is_ok());
    // Compression is unavailable in the default build.
#if !PFS_ENABLE_ZSTD
    CHECK(!save_game_to_bytes(*game, "x", 3).is_ok());
#endif
}

PFS_TEST(file_io, save_and_load_actual_file) {
    auto game = make_game();
    if (!game) return;
    game->allocate_memory(false);
    finalize(*game);

    const std::string path = "pfs_test_roundtrip.bin";
    CHECK(save_game_to_file(*game, "on disk", path).is_ok());

    Result<LoadedGame> loaded = load_game_from_file(path);
    CHECK(loaded.is_ok());
    if (loaded.is_ok()) {
        CHECK_EQ(loaded.value().memo, std::string("on disk"));
        check_root(*loaded.value().game);
    } else {
        ::pfs::test::report_failure(__FILE__, __LINE__, loaded.error());
    }

    CHECK(!load_game_from_file("definitely_missing_file.bin").is_ok());
    std::remove(path.c_str());
}

PFS_TEST(file_io, round_trip_preserves_node_locking) {
    CardConfig cc;
    cc.range[0] = Range::parse("AsAh,QsQh").value();
    cc.range[1] = Range::parse("KsKh").value();
    cc.flop = flop_from_str("2s3h4d").value();
    cc.turn = card_from_str("6c").value();
    cc.river = card_from_str("7c").value();

    TreeConfig tc;
    tc.initial_state = BoardState::River;
    tc.starting_pot = 20;
    tc.effective_stack = 10;
    tc.river_bet_sizes = {BetSizeOptions::parse("a", "").value(),
                          BetSizeOptions::parse("a", "").value()};

    Result<ActionTree> tree = ActionTree::create(std::move(tc));
    CHECK(tree.is_ok());
    auto g = PostFlopGame::with_config(std::move(cc), std::move(tree.value()));
    CHECK(g.is_ok());
    if (!g.is_ok()) return;
    PostFlopGame& game = *g.value();

    game.allocate_memory(false);
    game.play(1);
    game.lock_current_strategy(std::vector<float>{0.25f, 0.75f});
    game.back_to_root();
    solve(game, 1000, 0.0f, false);

    Result<std::vector<std::byte>> bytes = save_game_to_bytes(game, "locked");
    CHECK(bytes.is_ok());
    if (!bytes.is_ok()) return;
    Result<LoadedGame> loaded = load_game_from_bytes(bytes.value().data(), bytes.value().size());
    CHECK(loaded.is_ok());
    if (!loaded.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, loaded.error());
        return;
    }

    // The lock must survive, so the reloaded strategy matches.
    PostFlopGame& g2 = *loaded.value().game;
    g2.cache_normalized_weights();
    const std::vector<float> s = g2.strategy();
    CHECK_NEAR(s[0], 1.0f, 1e-3);
    CHECK_NEAR(s[3], 1.0f, 1e-3);

    g2.play(1);
    const auto lock = g2.current_locking_strategy();
    CHECK(lock.has_value());
    if (lock) {
        CHECK_NEAR((*lock)[0], 0.25f, 1e-6);
        CHECK_NEAR((*lock)[1], 0.75f, 1e-6);
    }
}
