// Port of src/game/{mod,node,base,evaluation,interpreter}.rs.
#pragma once

#include <pfs/action_tree.hpp>
#include <pfs/card_config.hpp>
#include <pfs/common.hpp>
#include <pfs/interface.hpp>
#include <pfs/result.hpp>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pfs {

class PostFlopGame;

// A node of the flat game-tree arena.
//
// Children are addressed by a RELATIVE arena index, so the arena stays
// relocatable and serializable, and remove_line can compact siblings by bumping
// the offset. The three storage pointers are absolute interior pointers into the
// owning game's four byte buffers, exactly as in the Rust: GameNode::strategy() is
// called on the node alone with no access to the game, so a stored byte offset
// could not be resolved. That is what makes PostFlopGame non-movable.
//
// num_elements is an ELEMENT count; the byte stride is 4 (float) or 2
// (uint16/int16) depending on whether compression is enabled.
class PostFlopNode : public GameNodeBase<PostFlopNode> {
public:
    // ---- GameNode interface ----
    bool is_terminal() const noexcept { return (player_ & PLAYER_TERMINAL_FLAG) != 0; }
    bool is_chance() const noexcept { return (player_ & PLAYER_CHANCE_FLAG) != 0; }

    // The RAW player byte, flags included. The solver's `player() == player` test
    // is only correct because is_terminal() and is_chance() are checked first.
    size_t player() const noexcept { return player_; }

    size_t num_actions() const noexcept { return num_children_; }
    PostFlopNode& play(size_t action) noexcept { return children()[action]; }

    // Parallelize above the river, i.e. at every flop and turn node.
    bool enable_parallelization() const noexcept { return river_ == NOT_DEALT; }

    std::optional<size_t> cfvalue_storage_player() const noexcept {
        const uint8_t prev = player_ & PLAYER_MASK;
        if (prev == 0) return size_t{1};
        if (prev == 1) return size_t{0};
        return std::nullopt;
    }

    bool has_cfvalues_ip() const noexcept { return num_elements_ip_ != 0; }

    // storage1 holds the strategy at action nodes and the chance cfvalues at
    // chance nodes; storage2 holds regrets during solving and cfvalues after
    // finalize (deliberately the same memory); storage3 holds IP cfvalues.
    std::span<float> strategy() noexcept { return as_span<float>(storage1_, num_elements_); }
    std::span<float> regrets() noexcept { return as_span<float>(storage2_, num_elements_); }
    std::span<float> cfvalues() noexcept { return as_span<float>(storage2_, num_elements_); }
    std::span<float> cfvalues_ip() noexcept {
        return as_span<float>(storage3_, num_elements_ip_);
    }
    std::span<float> cfvalues_chance() noexcept {
        return as_span<float>(storage1_, num_elements_);
    }

    std::span<uint16_t> strategy_compressed() noexcept {
        return as_span<uint16_t>(storage1_, num_elements_);
    }
    std::span<int16_t> regrets_compressed() noexcept {
        return as_span<int16_t>(storage2_, num_elements_);
    }
    std::span<int16_t> cfvalues_compressed() noexcept {
        return as_span<int16_t>(storage2_, num_elements_);
    }
    std::span<int16_t> cfvalues_ip_compressed() noexcept {
        return as_span<int16_t>(storage3_, num_elements_ip_);
    }
    std::span<int16_t> cfvalues_chance_compressed() noexcept {
        return as_span<int16_t>(storage1_, num_elements_);
    }

    float& strategy_scale() noexcept { return scale1_; }
    float& regret_scale() noexcept { return scale2_; }
    float& cfvalue_scale() noexcept { return scale2_; }
    float& cfvalue_ip_scale() noexcept { return scale3_; }
    float& cfvalue_chance_scale() noexcept { return scale1_; }

    // ---- postflop-specific ----
    Action prev_action() const noexcept { return prev_action_; }
    Card turn() const noexcept { return turn_; }
    Card river() const noexcept { return river_; }
    int32_t amount() const noexcept { return amount_; }
    bool is_locked() const noexcept { return is_locked_; }
    uint32_t num_elements() const noexcept { return num_elements_; }
    uint16_t num_elements_ip() const noexcept { return num_elements_ip_; }
    uint32_t children_offset() const noexcept { return children_offset_; }

    // Used only by remove_lines, which rewrites the arena in place.
    void set_num_elements(uint32_t n) noexcept { num_elements_ = n; }
    void set_num_elements_ip(uint16_t n) noexcept { num_elements_ip_ = n; }
    void set_num_children(uint16_t n) noexcept { num_children_ = n; }
    void bump_children_offset() noexcept { ++children_offset_; }

    std::span<PostFlopNode> children() noexcept {
        return {this + children_offset_, num_children_};
    }
    std::span<const PostFlopNode> children() const noexcept {
        return {this + children_offset_, num_children_};
    }

private:
    friend class PostFlopGame;

    template <class T>
    std::span<T> as_span(std::byte* base, uint32_t n) const noexcept {
        return {reinterpret_cast<T*>(base), n};
    }

    Action prev_action_{};
    uint8_t player_ = PLAYER_OOP;
    Card turn_ = NOT_DEALT;
    Card river_ = NOT_DEALT;
    bool is_locked_ = false;
    int32_t amount_ = 0;
    uint32_t children_offset_ = 0;  // relative arena index
    uint16_t num_children_ = 0;
    uint16_t num_elements_ip_ = 0;
    uint32_t num_elements_ = 0;
    float scale1_ = 0.0f;
    float scale2_ = 0.0f;
    float scale3_ = 0.0f;
    std::byte* storage1_ = nullptr;
    std::byte* storage2_ = nullptr;
    std::byte* storage3_ = nullptr;
};

// Lifecycle state. The API is guarded by ORDERED comparisons on this, and the
// numeric values are serialized, so they must not be renumbered.
enum class GameState : uint8_t {
    ConfigError = 0,
    Uninitialized = 1,
    TreeBuilt = 2,
    MemoryAllocated = 3,
    Solved = 4,
};

class PostFlopGame : public GameBase<PostFlopGame, PostFlopNode> {
public:
    PostFlopGame() = default;

    // Nodes hold interior pointers into this object's buffers and do arithmetic
    // off their own address inside node_arena_, so the game cannot be relocated.
    PostFlopGame(const PostFlopGame&) = delete;
    PostFlopGame& operator=(const PostFlopGame&) = delete;
    PostFlopGame(PostFlopGame&&) = delete;
    PostFlopGame& operator=(PostFlopGame&&) = delete;

    // Consumes the action tree.
    static Result<std::unique_ptr<PostFlopGame>> with_config(CardConfig card_config,
                                                            ActionTree action_tree);
    Status update_config(CardConfig card_config, ActionTree action_tree);

    // ---- Game interface ----
    PostFlopNode& root() noexcept { return node_arena_[0]; }
    size_t num_private_hands(size_t player) const noexcept {
        return private_cards_[player].size();
    }
    std::span<const float> initial_weights(size_t player) const noexcept {
        return initial_weights_[player];
    }
    void evaluate(std::span<float> result, PostFlopNode& node, size_t player,
                  std::span<const float> cfreach);
    size_t chance_factor(const PostFlopNode& node) const noexcept {
        return (node.turn() == NOT_DEALT ? 45 : 44) - bunching_num_dead_cards_;
    }
    bool is_solved() const noexcept { return state_ == GameState::Solved; }
    void set_solved();
    bool is_ready() const noexcept {
        return state_ == GameState::MemoryAllocated && storage_mode_ == BoardState::River;
    }
    bool is_raked() const noexcept {
        return tree_config_.rake_rate > 0.0 && tree_config_.rake_cap > 0.0;
    }
    bool is_compression_enabled() const noexcept { return is_compression_enabled_; }
    std::span<const uint8_t> isomorphic_chances(const PostFlopNode& node) const;
    const SwapList& isomorphic_swap(const PostFlopNode& node, size_t index) const;
    std::span<const float> locking_strategy(const PostFlopNode& node) const;

    // ---- configuration and memory ----
    const CardConfig& card_config() const noexcept { return card_config_; }
    const TreeConfig& tree_config() const noexcept { return tree_config_; }
    const std::vector<std::vector<Action>>& added_lines() const noexcept { return added_lines_; }
    const std::vector<std::vector<Action>>& removed_lines() const noexcept {
        return removed_lines_;
    }
    std::span<const Hole> private_cards(size_t player) const noexcept {
        return private_cards_[player];
    }

    // (uncompressed, compressed) estimates in bytes.
    std::pair<uint64_t, uint64_t> memory_usage() const;
    std::optional<bool> is_memory_allocated() const noexcept {
        if (state_ <= GameState::TreeBuilt) return std::nullopt;
        return is_compression_enabled_;
    }
    // Calling this a second time downgrades Solved back to MemoryAllocated, which
    // is the sanctioned way to re-solve after changing a node lock.
    void allocate_memory(bool enable_compression);

    // Legal only in the TreeBuilt state, i.e. before allocate_memory.
    Status remove_lines(const std::vector<std::vector<Action>>& lines);

    // ---- bunching effect ----
    // Enabling this raises the terminal-evaluation cost from
    // O(#oop + #ip) to O(#oop * #ip), so it slows solving down considerably.
    Status set_bunching_effect(const class BunchingData& data);
    void clear_bunching_effect();
    uint64_t memory_usage_bunching() const;

    // ---- interpreter ----
    void back_to_root();
    std::span<const size_t> history() const noexcept { return action_history_; }
    void apply_history(std::span<const size_t> history);
    bool is_terminal_node() const;
    bool is_chance_node() const;
    std::vector<Action> available_actions() const;
    uint64_t possible_cards() const;
    size_t current_player() const;
    std::vector<Card> current_board() const;
    // At a chance node `action` is the card id (or SIZE_MAX for the lowest
    // available card); elsewhere it is an index into available_actions().
    void play(size_t action);
    void cache_normalized_weights();
    std::span<const float> weights(size_t player) const;
    std::span<const float> normalized_weights(size_t player) const;
    std::vector<float> equity(size_t player) const;
    std::vector<float> expected_values(size_t player) const;
    std::vector<float> expected_values_detail(size_t player) const;
    // Action-major: element `i * num_hands + j` is the probability of action i for
    // hand j.
    std::vector<float> strategy() const;
    std::array<int32_t, 2> total_bet_amount() const noexcept { return total_bet_amount_; }
    void lock_current_strategy(std::span<const float> strategy);
    void unlock_current_strategy();
    std::optional<std::vector<float>> current_locking_strategy() const;

    // ---- serialization (needs PFS_ENABLE_SERIALIZATION) ----
    // set_target_storage_mode, target_memory_usage, serialize and deserialize are
    // defined in src/game/serialization.cpp, which is only built when
    // PFS_ENABLE_SERIALIZATION is on.
    BoardState storage_mode() const noexcept { return storage_mode_; }
    BoardState target_storage_mode() const noexcept { return target_storage_mode_; }
    // Narrowing the target discards the deeper streets when saving. The data is
    // recomputed by finalize() on load, so no information about the tree is lost --
    // only the ability to browse past that street.
    Status set_target_storage_mode(BoardState mode);
    uint64_t target_memory_usage() const;

    // Internal.
    size_t node_index(const PostFlopNode& node) const noexcept {
        return static_cast<size_t>(&node - node_arena_.data());
    }
    GameState state() const noexcept { return state_; }
    // The suit swaps currently in effect, from having played an isomorphic card.
    std::optional<uint8_t> turn_swap() const noexcept { return turn_swap_; }
    std::optional<std::pair<uint8_t, uint8_t>> river_swap() const noexcept { return river_swap_; }
    double num_combinations() const noexcept { return num_combinations_; }

    // Internal, used by file.cpp.
    void serialize(class BinWriter& w) const;
    static Result<std::unique_ptr<PostFlopGame>> deserialize(class BinReader& r);

private:
    struct BuildInfo;

    Status check_card_config();
    void init_hands();
    void init_card_fields();
    Status init_root();
    void init_interpreter();
    void clear_storage();
    std::array<uint64_t, 3> count_num_nodes() const;
    uint64_t memory_usage_internal() const;
    void build_tree_recursive(size_t node_index, const ActionTreeNode& action_node,
                              BuildInfo& info);
    void push_chances(size_t node_index, BuildInfo& info);
    void push_actions(size_t node_index, const ActionTreeNode& action_node, BuildInfo& info);
    void allocate_memory_nodes();
    Status remove_line_recursive(PostFlopNode& node, std::span<const Action> line,
                                 uint64_t& num_storage, uint64_t& num_storage_ip,
                                 uint64_t& num_storage_chance);
    // Byte lengths to write for [storage1, storage2, storage_ip, storage_chance]
    // under the current target storage mode.
    std::array<size_t, 4> num_target_storage() const;
    // which: 0 = storage1, 1 = storage2, 2 = storage_ip, 3 = storage_chance.
    size_t node_storage_offset(const PostFlopNode& node, int which) const;

    void evaluate_internal(std::span<float> result, const PostFlopNode& node, size_t player,
                          std::span<const float> cfreach);
    void evaluate_internal_bunching(std::span<float> result, const PostFlopNode& node,
                                   size_t player, std::span<const float> cfreach);
    Status set_bunching_effect_internal(const class BunchingData& data);
    std::vector<float> equity_internal_bunching(size_t player) const;

    PostFlopNode& node_at(size_t index) noexcept { return node_arena_[index]; }
    const PostFlopNode& current_node() const;
    PostFlopNode& current_node_mut();
    void assign_zero_weights();
    void apply_swap_to(std::span<float> slice, size_t player, bool reverse) const;
    void equity_internal(std::span<double> result, size_t player, Card turn, Card river,
                         double amount) const;

    GameState state_ = GameState::Uninitialized;

    CardConfig card_config_;
    TreeConfig tree_config_;
    std::vector<std::vector<Action>> added_lines_;
    std::vector<std::vector<Action>> removed_lines_;
    std::unique_ptr<ActionTreeNode> action_root_;

    double num_combinations_ = 0.0;
    std::array<std::vector<float>, 2> initial_weights_;
    PrivateCards private_cards_;
    std::array<std::vector<uint16_t>, 2> same_hand_index_;

    Indices valid_indices_flop_;
    std::vector<Indices> valid_indices_turn_;
    std::vector<Indices> valid_indices_river_;
    std::vector<HandStrength> hand_strength_;
    CardConfig::Isomorphism iso_;

    // Bunching effect. bunching_arena_[0] is a dummy so that an index of 0 can
    // mean "absent"; every other index is the start of an opponent-length run.
    size_t bunching_num_dead_cards_ = 0;
    double bunching_num_combinations_ = 0.0;
    std::vector<float> bunching_arena_;
    std::vector<std::array<std::vector<uint16_t>, 2>> bunching_strength_;
    std::array<std::vector<size_t>, 2> bunching_num_flop_;
    std::array<std::vector<std::vector<size_t>>, 2> bunching_num_turn_;
    std::array<std::vector<std::vector<size_t>>, 2> bunching_num_river_;
    std::array<std::vector<size_t>, 2> bunching_coef_flop_;
    std::array<std::vector<std::vector<size_t>>, 2> bunching_coef_turn_;

    BoardState storage_mode_ = BoardState::Flop;
    BoardState target_storage_mode_ = BoardState::Flop;
    std::array<uint64_t, 3> num_nodes_{0, 0, 0};
    bool is_compression_enabled_ = false;
    uint64_t num_storage_ = 0;
    uint64_t num_storage_ip_ = 0;
    uint64_t num_storage_chance_ = 0;
    uint64_t misc_memory_usage_ = 0;

    // Never std::vector: nodes hold interior pointers into these, so there must be
    // no reallocation path.
    std::vector<PostFlopNode> node_arena_;
    std::unique_ptr<std::byte[]> storage1_;
    std::unique_ptr<std::byte[]> storage2_;
    std::unique_ptr<std::byte[]> storage_ip_;
    std::unique_ptr<std::byte[]> storage_chance_;
    size_t storage_bytes_ = 0;
    size_t storage_ip_bytes_ = 0;
    size_t storage_chance_bytes_ = 0;
    std::map<size_t, std::vector<float>> locking_strategy_;

    // interpreter state
    std::vector<size_t> action_history_;
    std::vector<size_t> node_history_;
    bool is_normalized_weight_cached_ = false;
    Card turn_ = NOT_DEALT;
    Card river_ = NOT_DEALT;
    std::optional<std::pair<uint8_t, uint8_t>> turn_swapped_suit_;
    std::optional<uint8_t> turn_swap_;
    std::optional<std::pair<uint8_t, uint8_t>> river_swap_;
    std::array<int32_t, 2> total_bet_amount_{0, 0};
    std::array<std::vector<float>, 2> weights_;
    std::array<std::vector<float>, 2> normalized_weights_;
    std::array<std::vector<float>, 2> cfvalues_cache_;
};

}  // namespace pfs
