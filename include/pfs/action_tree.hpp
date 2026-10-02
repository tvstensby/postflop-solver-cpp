// Port of src/action_tree.rs.
#pragma once

#include <pfs/bet_size.hpp>
#include <pfs/card.hpp>
#include <pfs/common.hpp>
#include <pfs/result.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pfs {

// Player encoding, packed into a single byte alongside the 0/1 player index.
// Note PLAYER_FOLD_FLAG == 24 == 8|16, so a fold is also terminal, and the fold
// test is an equality against the whole mask rather than `!= 0`.
inline constexpr uint8_t PLAYER_OOP = 0;
inline constexpr uint8_t PLAYER_IP = 1;
inline constexpr uint8_t PLAYER_CHANCE = 2;  // only with PLAYER_CHANCE_FLAG
inline constexpr uint8_t PLAYER_MASK = 3;
inline constexpr uint8_t PLAYER_CHANCE_FLAG = 4;  // chance = PLAYER_CHANCE_FLAG | prev_player
inline constexpr uint8_t PLAYER_TERMINAL_FLAG = 8;
inline constexpr uint8_t PLAYER_FOLD_FLAG = 24;

// Declaration order is load-bearing: Rust derives Ord on the Action enum, and
// the tree keeps each node's actions sorted by it. sort/dedup/binary_search all
// depend on this exact order.
enum class ActionKind : uint8_t {
    None = 0,
    Fold = 1,
    Check = 2,
    Call = 3,
    Bet = 4,
    Raise = 5,
    AllIn = 6,
    Chance = 7,
};

struct Action {
    ActionKind kind = ActionKind::None;
    // Bet / Raise / AllIn: the amount. Chance: the card id. For every other kind
    // this MUST stay 0, or two otherwise-equal actions would compare unequal.
    int32_t amount = 0;

    static Action none() { return {ActionKind::None, 0}; }
    static Action fold() { return {ActionKind::Fold, 0}; }
    static Action check() { return {ActionKind::Check, 0}; }
    static Action call() { return {ActionKind::Call, 0}; }
    static Action bet(int32_t a) { return {ActionKind::Bet, a}; }
    static Action raise(int32_t a) { return {ActionKind::Raise, a}; }
    static Action all_in(int32_t a) { return {ActionKind::AllIn, a}; }
    static Action chance(Card c) { return {ActionKind::Chance, static_cast<int32_t>(c)}; }

    bool is_bet_like() const noexcept {
        return kind == ActionKind::Bet || kind == ActionKind::Raise || kind == ActionKind::AllIn;
    }

    friend bool operator==(const Action& a, const Action& b) noexcept {
        return a.kind == b.kind && a.amount == b.amount;
    }
    friend bool operator!=(const Action& a, const Action& b) noexcept { return !(a == b); }
    // Rust's derived Ord: discriminant first, then payload.
    friend bool operator<(const Action& a, const Action& b) noexcept {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.amount < b.amount;
    }

    // Matches Rust's `{:?}`; examples/basic.rs asserts on this exact text.
    std::string to_string() const;
};

std::string actions_to_string(std::span<const Action> actions);

enum class BoardState : uint8_t { Flop = 0, Turn = 1, River = 2 };

struct TreeConfig {
    BoardState initial_state = BoardState::Flop;
    int32_t starting_pot = 0;     // must be > 0
    int32_t effective_stack = 0;  // must be > 0
    double rake_rate = 0.0;       // in [0, 1]
    double rake_cap = 0.0;        // >= 0
    std::array<BetSizeOptions, 2> flop_bet_sizes;   // [OOP, IP]
    std::array<BetSizeOptions, 2> turn_bet_sizes;
    std::array<BetSizeOptions, 2> river_bet_sizes;
    std::optional<DonkSizeOptions> turn_donk_sizes;   // nullopt => default bet sizes
    std::optional<DonkSizeOptions> river_donk_sizes;
    // Add an all-in action when the maximum bet size is <= this multiple of the pot.
    double add_allin_threshold = 0.0;
    // Force all-in when the SPR after the opponent's call is <= this.
    double force_allin_threshold = 0.0;
    // Merge bet actions with close values, PioSOLVER's algorithm.
    double merging_threshold = 0.0;
};

// A node of the abstract action tree. This is an owning pointer tree, distinct
// from PostFlopGame's flat node arena.
struct ActionTreeNode {
    uint8_t player = PLAYER_OOP;
    BoardState board_state = BoardState::Flop;
    int32_t amount = 0;
    std::vector<Action> actions;
    std::vector<ActionTreeNode> children;  // parallel to `actions`

    bool is_terminal() const noexcept { return (player & PLAYER_TERMINAL_FLAG) != 0; }
    bool is_chance() const noexcept { return (player & PLAYER_CHANCE_FLAG) != 0; }
};

// Rust's `Vec::binary_search`: on a miss the index is where the value belongs.
struct SearchResult {
    bool found;
    size_t index;
};
SearchResult binary_search_action(std::span<const Action> actions, const Action& a) noexcept;

// An abstract game tree. It does not distinguish between possible chance events
// (turn/river deals) and treats them as a single action.
class ActionTree {
public:
    static Result<ActionTree> create(TreeConfig config);

    const TreeConfig& config() const noexcept { return config_; }
    const std::vector<std::vector<Action>>& added_lines() const noexcept { return added_lines_; }
    const std::vector<std::vector<Action>>& removed_lines() const noexcept {
        return removed_lines_;
    }

    // Terminal nodes that should not be terminal, i.e. lines with no children.
    std::vector<std::vector<Action>> invalid_terminals() const;

    Status add_line(std::span<const Action> line);
    Status remove_line(std::span<const Action> line);

    void back_to_root() noexcept { history_.clear(); }
    const std::vector<Action>& history() const noexcept { return history_; }
    Status apply_history(std::span<const Action> history);

    bool is_terminal_node() const;
    bool is_chance_node() const;
    std::span<const Action> available_actions() const;
    Status play(Action action);
    Status undo();
    Status add_action(Action action);
    Status remove_action(Action action);
    Status remove_current_node();
    std::array<int32_t, 2> total_bet_amount() const;

    // Internal: hands the root over to PostFlopGame, consuming this tree.
    struct Ejected {
        TreeConfig config;
        std::vector<std::vector<Action>> added_lines;
        std::vector<std::vector<Action>> removed_lines;
        std::unique_ptr<ActionTreeNode> root;
    };
    Ejected eject() &&;

private:
    ActionTree() : root_(std::make_unique<ActionTreeNode>()) {}

    static Status check_config(const TreeConfig& config);
    void build_tree();

    const ActionTreeNode* current_node() const;
    const ActionTreeNode* current_node_skip_chance() const;

    TreeConfig config_;
    std::vector<std::vector<Action>> added_lines_;
    std::vector<std::vector<Action>> removed_lines_;
    std::unique_ptr<ActionTreeNode> root_;
    std::vector<Action> history_;
};

// Internal: per-street action-node counts, [flop, turn, river].
std::array<uint64_t, 3> count_num_action_nodes(const ActionTreeNode& node);

}  // namespace pfs
