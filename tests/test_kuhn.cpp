// Port of tests/kuhn.rs.
//
// Like the Rust original this is an *external* implementation of the Game and
// GameNode interfaces: it uses only the public surface, defines its own node
// type, its own action enum and its own player-flag constants (deliberately
// different values from the library's internal ones, to prove the solver does not
// depend on them). If this reproduces the game-theoretic value of Kuhn poker, the
// abstraction and the DCFR core are correct independently of any postflop code.
#include "harness.hpp"

#include <pfs/interface.hpp>
#include <pfs/solver.hpp>
#include <pfs/utility.hpp>

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

enum class KAct { None, Fold, Check, Call, Bet };

constexpr size_t kNumPrivateHands = 3;
constexpr size_t kPlayerOop = 0;
constexpr size_t kPlayerMask = 0xff;
constexpr size_t kPlayerTerminalFlag = 0x100;
constexpr size_t kPlayerFoldFlag = 0x300;

class KuhnNode : public pfs::GameNodeBase<KuhnNode> {
public:
    size_t player_ = kPlayerOop;
    int32_t amount = 1;
    std::vector<std::pair<KAct, std::unique_ptr<KuhnNode>>> children;
    std::vector<float> strategy_;
    std::vector<float> storage_;

    bool is_terminal() { return (player_ & kPlayerTerminalFlag) != 0; }
    bool is_chance() { return false; }
    size_t player() { return player_; }
    size_t num_actions() { return children.size(); }
    KuhnNode& play(size_t action) { return *children[action].second; }

    std::span<float> strategy() { return strategy_; }
    // regrets and cfvalues alias the same storage, exactly as in the Rust.
    std::span<float> regrets() { return storage_; }
    std::span<float> cfvalues() { return storage_; }
};

class KuhnGame : public pfs::GameBase<KuhnGame, KuhnNode> {
public:
    KuhnGame() : root_(build_tree()), initial_weight_(kNumPrivateHands, 1.0f) {}

    KuhnNode& root() { return *root_; }
    size_t num_private_hands(size_t) { return kNumPrivateHands; }
    std::span<const float> initial_weights(size_t) { return initial_weight_; }
    size_t chance_factor(KuhnNode&) { PFS_UNREACHABLE(); }
    bool is_solved() { return is_solved_; }
    void set_solved() { is_solved_ = true; }

    void evaluate(std::span<float> result, KuhnNode& node, size_t player,
                  std::span<const float> cfreach) {
        for (float& r : result) r = 0.0f;

        const size_t num_hands = kNumPrivateHands * (kNumPrivateHands - 1);  // 6
        const float num_hands_inv = 1.0f / static_cast<float>(num_hands);
        const float amount_normalized = static_cast<float>(node.amount) * num_hands_inv;

        if ((node.player_ & kPlayerFoldFlag) == kPlayerFoldFlag) {
            const size_t folded_player = node.player_ & kPlayerMask;
            const float sign = (player == folded_player) ? -1.0f : 1.0f;
            const float payoff = amount_normalized * sign;
            for (size_t my = 0; my < kNumPrivateHands; ++my)
                for (size_t opp = 0; opp < kNumPrivateHands; ++opp)
                    if (my != opp) result[my] += payoff * cfreach[opp];
        } else {
            for (size_t my = 0; my < kNumPrivateHands; ++my)
                for (size_t opp = 0; opp < kNumPrivateHands; ++opp)
                    if (my != opp) {
                        // Higher card index wins.
                        const float sign = (my < opp) ? -1.0f : 1.0f;
                        result[my] += amount_normalized * sign * cfreach[opp];
                    }
        }
    }

private:
    static std::unique_ptr<KuhnNode> build_tree() {
        auto root = std::make_unique<KuhnNode>();
        root->player_ = kPlayerOop;
        root->amount = 1;
        build_tree_recursive(*root, KAct::None);
        allocate_memory_recursive(*root);
        return root;
    }

    static void build_tree_recursive(KuhnNode& node, KAct prev_action) {
        if (node.is_terminal()) return;

        std::vector<KAct> actions;
        if (prev_action == KAct::None || prev_action == KAct::Check) {
            actions = {KAct::Check, KAct::Bet};
        } else if (prev_action == KAct::Bet) {
            actions = {KAct::Fold, KAct::Call};
        } else {
            PFS_UNREACHABLE();
        }

        for (KAct action : actions) {
            size_t next_player;
            if (action == KAct::Check && prev_action == KAct::Check) {
                next_player = kPlayerTerminalFlag;
            } else if (action == KAct::Fold) {
                next_player = kPlayerFoldFlag | node.player_;
            } else if (action == KAct::Call) {
                next_player = kPlayerTerminalFlag;
            } else {
                next_player = node.player_ ^ 1u;
            }

            auto child = std::make_unique<KuhnNode>();
            child->player_ = next_player;
            child->amount = node.amount + (action == KAct::Call ? 1 : 0);
            node.children.emplace_back(action, std::move(child));
        }

        for (auto& [action, child] : node.children) build_tree_recursive(*child, action);
    }

    static void allocate_memory_recursive(KuhnNode& node) {
        if (node.is_terminal()) return;
        const size_t n = node.num_actions() * kNumPrivateHands;
        node.strategy_.assign(n, 0.0f);
        node.storage_.assign(n, 0.0f);
        for (size_t a = 0; a < node.num_actions(); ++a) allocate_memory_recursive(node.play(a));
    }

    std::unique_ptr<KuhnNode> root_;
    std::vector<float> initial_weight_;
    bool is_solved_ = false;
};

static_assert(pfs::GameLike<KuhnGame>, "KuhnGame must satisfy the Game interface");

}  // namespace

PFS_TEST(kuhn, root_ev_matches_game_theoretic_value) {
    constexpr float target = 1e-4f;
    KuhnGame game;
    pfs::solve(game, 10000, target, false);

    KuhnNode& root = game.root();

    // Renormalize the root's cumulative strategy per hand over the two actions.
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

    // The exact value of Kuhn poker for the first player.
    CHECK_NEAR(root_ev, -1.0f / 18.0f, 2.0 * target);
}

PFS_TEST(kuhn, exploitability_converges) {
    KuhnGame game;
    const float initial = pfs::compute_exploitability(game);
    CHECK(initial > 0.01f);

    const float final_expl = pfs::solve(game, 10000, 1e-4f, false);
    CHECK(final_expl <= 1e-4f);
    CHECK(game.is_solved());

    // An unraked game is zero-sum once the pot bias is removed.
    const std::array<float, 2> ev = pfs::compute_current_ev(game);
    CHECK_NEAR(ev[0] + ev[1], 0.0f, 1e-4);
    CHECK_NEAR(ev[0], -1.0f / 18.0f, 2e-4);
}

PFS_TEST(kuhn, solve_step_drives_the_same_result) {
    // The manual loop from the commented-out block in examples/basic.rs.
    KuhnGame game;
    for (uint32_t i = 0; i < 2000; ++i) pfs::solve_step(game, i);
    const float expl = pfs::compute_exploitability(game);
    CHECK(expl < 1e-3f);

    pfs::finalize(game);
    CHECK(game.is_solved());

    const std::array<float, 2> ev = pfs::compute_current_ev(game);
    CHECK_NEAR(ev[0], -1.0f / 18.0f, 1e-3);
}
