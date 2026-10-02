// Port of tests/leduc.rs.
//
// The richer of the two external Game implementations: it has chance nodes, a
// hard-coded suit isomorphism (so the solver's swap machinery is exercised), and
// it implements the 16-bit compressed accessors by reinterpreting the same float
// buffers. Between this and test_kuhn.cpp, essentially all of solver.hpp,
// utility.hpp, sliceop.hpp and the quantization codec is covered without any
// postflop-specific code.
#include "harness.hpp"

#include <pfs/interface.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

enum class LAct { None, Fold, Check, Call, Bet, Raise, Chance };

struct LAction {
    LAct kind = LAct::None;
    int32_t amount = 0;  // Bet/Raise amount, or the chance index
    friend bool operator==(const LAction& a, const LAction& b) {
        return a.kind == b.kind && a.amount == b.amount;
    }
};

constexpr size_t kNumPrivateHands = 6;
constexpr size_t kPlayerOop = 0;
constexpr size_t kPlayerChance = 0xff;
constexpr size_t kPlayerMask = 0xff;
constexpr size_t kPlayerTerminalFlag = 0x100;
constexpr size_t kPlayerFoldFlag = 0x300;
constexpr size_t kNotDealt = 0xff;

class LeducNode : public pfs::GameNodeBase<LeducNode> {
public:
    size_t player_ = kPlayerOop;
    size_t board = kNotDealt;
    int32_t amount = 1;
    std::vector<std::pair<LAction, std::unique_ptr<LeducNode>>> children;
    std::vector<float> strategy_;
    std::vector<float> storage_;
    float strategy_scale_ = 0.0f;
    float storage_scale_ = 0.0f;

    bool is_terminal() { return (player_ & kPlayerTerminalFlag) != 0; }
    // Note: an equality test, not a flag test.
    bool is_chance() { return player_ == kPlayerChance; }
    size_t player() { return player_; }
    size_t num_actions() { return children.size(); }
    LeducNode& play(size_t action) { return *children[action].second; }

    std::span<float> strategy() { return strategy_; }
    std::span<float> regrets() { return storage_; }
    std::span<float> cfvalues() { return storage_; }

    // The compressed views deliberately reinterpret the same buffers, using only
    // half their bytes -- exactly what the Rust does.
    std::span<uint16_t> strategy_compressed() {
        return {reinterpret_cast<uint16_t*>(strategy_.data()), strategy_.size()};
    }
    std::span<int16_t> regrets_compressed() {
        return {reinterpret_cast<int16_t*>(storage_.data()), storage_.size()};
    }
    std::span<int16_t> cfvalues_compressed() {
        return {reinterpret_cast<int16_t*>(storage_.data()), storage_.size()};
    }

    float& strategy_scale() { return strategy_scale_; }
    float& regret_scale() { return storage_scale_; }
    float& cfvalue_scale() { return storage_scale_; }
};

class LeducGame : public pfs::GameBase<LeducGame, LeducNode> {
public:
    explicit LeducGame(bool compression)
        : root_(build_tree()),
          initial_weight_(kNumPrivateHands, 1.0f),
          isomorphism_{0, 1, 2},
          is_compression_enabled_(compression) {
        const std::vector<pfs::Swap> swaps{pfs::Swap{uint16_t{0}, uint16_t{1}},
                                           pfs::Swap{uint16_t{2}, uint16_t{3}},
                                           pfs::Swap{uint16_t{4}, uint16_t{5}}};
        isomorphism_swap_[0] = swaps;
        isomorphism_swap_[1] = swaps;
    }

    LeducNode& root() { return *root_; }
    size_t num_private_hands(size_t) { return kNumPrivateHands; }
    std::span<const float> initial_weights(size_t) { return initial_weight_; }
    size_t chance_factor(LeducNode&) { return 4; }
    bool is_solved() { return is_solved_; }
    void set_solved() { is_solved_ = true; }
    bool is_compression_enabled() { return is_compression_enabled_; }

    std::span<const uint8_t> isomorphic_chances(const LeducNode&) { return isomorphism_; }
    const pfs::SwapList& isomorphic_swap(const LeducNode&, size_t) { return isomorphism_swap_; }

    void evaluate(std::span<float> result, LeducNode& node, size_t player,
                  std::span<const float> cfreach) {
        for (float& r : result) r = 0.0f;

        const size_t num_hands = kNumPrivateHands * (kNumPrivateHands - 1);  // 30
        const float num_hands_inv = 1.0f / static_cast<float>(num_hands);
        const float amount_normalized = static_cast<float>(node.amount) * num_hands_inv;

        if ((node.player_ & kPlayerFoldFlag) == kPlayerFoldFlag) {
            const size_t folded_player = node.player_ & kPlayerMask;
            const float sign = (player == folded_player) ? -1.0f : 1.0f;
            const float payoff = amount_normalized * sign;
            for (size_t my = 0; my < kNumPrivateHands; ++my) {
                if (my == node.board) continue;
                for (size_t opp = 0; opp < kNumPrivateHands; ++opp)
                    if (my != opp && opp != node.board) result[my] += payoff * cfreach[opp];
            }
        } else {
            for (size_t my = 0; my < kNumPrivateHands; ++my) {
                if (my == node.board) continue;
                for (size_t opp = 0; opp < kNumPrivateHands; ++opp) {
                    if (my == opp || opp == node.board) continue;
                    float sign;
                    if (my / 2 == node.board / 2) sign = 1.0f;        // I pair the board
                    else if (opp / 2 == node.board / 2) sign = -1.0f; // opponent pairs it
                    else if (my / 2 == opp / 2) sign = 0.0f;          // same rank: tie
                    else if (my > opp) sign = 1.0f;
                    else sign = -1.0f;
                    result[my] += amount_normalized * sign * cfreach[opp];
                }
            }
        }
    }

private:
    static std::unique_ptr<LeducNode> build_tree() {
        auto root = std::make_unique<LeducNode>();
        root->player_ = kPlayerOop;
        root->board = kNotDealt;
        root->amount = 1;
        build_tree_recursive(*root, LAction{LAct::None, 0}, {0, 0});
        allocate_memory_recursive(*root);
        return root;
    }

    static void push_chance_actions(LeducNode& node) {
        // Only 3 of the 6 boards are enumerated; the other suit of each rank is
        // folded in by the isomorphism swap list.
        for (int32_t index = 0; index < 3; ++index) {
            auto child = std::make_unique<LeducNode>();
            child->player_ = kPlayerOop;
            child->board = static_cast<size_t>(index * 2);
            child->amount = node.amount;
            node.children.emplace_back(LAction{LAct::Chance, index * 2}, std::move(child));
        }
    }

    static std::vector<std::pair<LAction, size_t>> get_actions(const LeducNode& node,
                                                              LAction prev_action,
                                                              bool is_second_round) {
        const int32_t raise_amount = is_second_round ? 4 : 2;
        const size_t player = node.player_;
        const size_t opponent = player ^ 1u;

        const size_t player_after_call =
            is_second_round ? (kPlayerTerminalFlag | player) : kPlayerChance;
        const size_t player_after_check = (player == kPlayerOop) ? opponent : player_after_call;

        std::vector<std::pair<LAction, size_t>> actions;
        switch (prev_action.kind) {
            case LAct::None:
            case LAct::Check:
            case LAct::Chance:
                actions.push_back({LAction{LAct::Check, 0}, player_after_check});
                actions.push_back({LAction{LAct::Bet, raise_amount}, opponent});
                break;
            case LAct::Bet:
                actions.push_back({LAction{LAct::Fold, 0}, kPlayerFoldFlag | player});
                actions.push_back({LAction{LAct::Call, 0}, player_after_call});
                actions.push_back(
                    {LAction{LAct::Raise, prev_action.amount + raise_amount}, opponent});
                break;
            case LAct::Raise:
                actions.push_back({LAction{LAct::Fold, 0}, kPlayerFoldFlag | player});
                actions.push_back({LAction{LAct::Call, 0}, player_after_call});
                break;
            default:
                PFS_UNREACHABLE();
        }
        return actions;
    }

    static void build_tree_recursive(LeducNode& node, LAction prev_action,
                                     std::array<int32_t, 2> prev_amount) {
        if (node.is_terminal()) return;

        if (node.is_chance()) {
            push_chance_actions(node);
            for (size_t a = 0; a < node.num_actions(); ++a)
                build_tree_recursive(node.play(a), LAction{LAct::Chance, static_cast<int32_t>(a)},
                                     {0, 0});
            return;
        }

        const auto actions = get_actions(node, prev_action, node.board != kNotDealt);
        const int32_t prev_amount_min = std::min(prev_amount[0], prev_amount[1]);

        std::vector<std::array<int32_t, 2>> next_amounts;
        for (const auto& [action, next_player] : actions) {
            std::array<int32_t, 2> next_amount = prev_amount;
            if (action.kind == LAct::Call) next_amount[node.player_] = next_amount[node.player_ ^ 1];
            if (action.kind == LAct::Bet || action.kind == LAct::Raise)
                next_amount[node.player_] = action.amount;

            next_amounts.push_back(next_amount);
            const int32_t amount_diff =
                std::min(next_amount[0], next_amount[1]) - prev_amount_min;

            auto child = std::make_unique<LeducNode>();
            child->player_ = next_player;
            child->board = node.board;
            child->amount = node.amount + amount_diff;
            node.children.emplace_back(action, std::move(child));
        }

        for (size_t a = 0; a < node.num_actions(); ++a)
            build_tree_recursive(node.play(a), actions[a].first, next_amounts[a]);
    }

    static void allocate_memory_recursive(LeducNode& node) {
        if (node.is_terminal()) return;
        if (!node.is_chance()) {
            const size_t n = node.num_actions() * kNumPrivateHands;
            node.strategy_.assign(n, 0.0f);
            node.storage_.assign(n, 0.0f);
        }
        for (size_t a = 0; a < node.num_actions(); ++a) allocate_memory_recursive(node.play(a));
    }

    std::unique_ptr<LeducNode> root_;
    std::vector<float> initial_weight_;
    std::vector<uint8_t> isomorphism_;
    pfs::SwapList isomorphism_swap_;
    bool is_solved_ = false;
    bool is_compression_enabled_ = false;
};

static_assert(pfs::GameLike<LeducGame>, "LeducGame must satisfy the Game interface");

}  // namespace

PFS_TEST(leduc, root_ev_matches_openspiel) {
    constexpr float target = 1e-4f;
    LeducGame game(false);
    pfs::solve(game, 10000, target, false);

    LeducNode& root = game.root();

    std::vector<float> strategy(root.strategy().begin(), root.strategy().end());
    for (size_t i = 0; i < kNumPrivateHands; ++i) {
        const size_t j = i + kNumPrivateHands;
        const float sum = strategy[i] + strategy[j];
        strategy[i] /= sum;
        strategy[j] /= sum;
    }

    float root_ev = 0.0f;
    const std::span<float> cfv = root.cfvalues();
    for (size_t i = 0; i < cfv.size(); ++i) root_ev += cfv[i] * strategy[i];

    // Verified by OpenSpiel.
    CHECK_NEAR(root_ev, -0.0856f, 2.0 * target);
}

PFS_TEST(leduc, root_ev_matches_openspiel_compressed) {
    constexpr float target = 1e-3f;
    LeducGame game(true);
    pfs::solve(game, 10000, target, false);

    LeducNode& root = game.root();

    std::array<float, kNumPrivateHands * 2> strategy{};
    const std::span<uint16_t> raw_strategy = root.strategy_compressed();
    for (size_t i = 0; i < kNumPrivateHands; ++i) {
        const size_t j = i + kNumPrivateHands;
        const float sum = static_cast<float>(static_cast<uint32_t>(raw_strategy[i]) +
                                             static_cast<uint32_t>(raw_strategy[j]));
        strategy[i] = static_cast<float>(raw_strategy[i]) / sum;
        strategy[j] = static_cast<float>(raw_strategy[j]) / sum;
    }

    const float ev_decoder = root.cfvalue_scale() / 32767.0f;
    float root_ev = 0.0f;
    const std::span<int16_t> raw_cfv = root.cfvalues_compressed();
    for (size_t i = 0; i < strategy.size(); ++i)
        root_ev += ev_decoder * static_cast<float>(raw_cfv[i]) * strategy[i];

    CHECK_NEAR(root_ev, -0.0856f, 2.0 * target);
}

PFS_TEST(leduc, exploitability_converges_and_is_zero_sum) {
    LeducGame game(false);
    const float expl = pfs::solve(game, 10000, 1e-4f, false);
    CHECK(expl <= 1e-4f);
    CHECK(game.is_solved());

    const std::array<float, 2> ev = pfs::compute_current_ev(game);
    CHECK_NEAR(ev[0] + ev[1], 0.0f, 1e-4);
    CHECK_NEAR(ev[0], -0.0856f, 2e-4);
}
