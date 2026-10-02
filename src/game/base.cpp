// Port of src/game/base.rs -- configuration, the flat node arena and memory
// allocation.
#include <pfs/game.hpp>

#include "../hand.hpp"

#include <algorithm>
#include <cstring>

namespace pfs {

// Three independent bump cursors, one per street, so that all flop nodes occupy
// [0, n0), all turn nodes [n0, n0+n1) and all river nodes the rest. That
// contiguity is what later lets serialization truncate by street.
struct PostFlopGame::BuildInfo {
    size_t flop_index = 0;
    size_t turn_index = 0;
    size_t river_index = 0;
    uint64_t num_storage = 0;
    uint64_t num_storage_ip = 0;
    uint64_t num_storage_chance = 0;
};

namespace {

bool flop_contains(const std::array<Card, 3>& flop, Card c) {
    return flop[0] == c || flop[1] == c || flop[2] == c;
}

uint64_t mask_of(const std::array<Card, 3>& flop) {
    return (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) | (uint64_t{1} << flop[2]);
}

uint64_t mask_of(const std::vector<Card>& cards) {
    uint64_t m = 0;
    for (Card c : cards) m |= uint64_t{1} << c;
    return m;
}

template <class T>
uint64_t vec_memory_usage(const std::vector<T>& v) {
    return static_cast<uint64_t>(v.capacity()) * sizeof(T);
}

}  // namespace

Result<std::unique_ptr<PostFlopGame>> PostFlopGame::with_config(CardConfig card_config,
                                                               ActionTree action_tree) {
    auto game = std::unique_ptr<PostFlopGame>(new PostFlopGame());
    Status st = game->update_config(std::move(card_config), std::move(action_tree));
    if (!st) return Result<std::unique_ptr<PostFlopGame>>::err(st.error());
    return Result<std::unique_ptr<PostFlopGame>>(std::move(game));
}

Status PostFlopGame::update_config(CardConfig card_config, ActionTree action_tree) {
    state_ = GameState::ConfigError;  // fail closed

    if (!action_tree.invalid_terminals().empty())
        return Status::err("Invalid terminal is found in action tree");

    card_config_ = std::move(card_config);

    ActionTree::Ejected ejected = std::move(action_tree).eject();
    tree_config_ = std::move(ejected.config);
    added_lines_ = std::move(ejected.added_lines);
    removed_lines_ = std::move(ejected.removed_lines);
    action_root_ = std::move(ejected.root);

    Status st = check_card_config();
    if (!st) return st;
    init_card_fields();
    st = init_root();
    if (!st) return st;

    state_ = GameState::TreeBuilt;

    init_interpreter();
    clear_bunching_effect();
    return Status::ok();
}

Status PostFlopGame::check_card_config() {
    const std::array<Card, 3>& flop = card_config_.flop;
    const Card turn = card_config_.turn;
    const Card river = card_config_.river;

    if (flop_contains(flop, NOT_DEALT)) return Status::err("Flop cards not initialized");
    for (Card c : flop)
        if (c >= 52) return Status::err("Flop cards must be in [0, 52)");
    if (flop[0] == flop[1] || flop[0] == flop[2] || flop[1] == flop[2])
        return Status::err("Flop cards must be unique");

    if (turn != NOT_DEALT) {
        if (turn >= 52) return Status::err("Turn card must be in [0, 52)");
        if (flop_contains(flop, turn))
            return Status::err("Turn card must be different from flop cards");
    }

    if (river != NOT_DEALT) {
        if (river >= 52) return Status::err("River card must be in [0, 52)");
        if (flop_contains(flop, river))
            return Status::err("River card must be different from flop cards");
        if (turn == river) return Status::err("River card must be different from turn card");
        if (turn == NOT_DEALT) return Status::err("River card specified without turn card");
    }

    const BoardState expected = (turn == NOT_DEALT)
                                    ? BoardState::Flop
                                    : (river == NOT_DEALT ? BoardState::Turn : BoardState::River);
    if (tree_config_.initial_state != expected)
        return Status::err("Invalid initial state of `tree_config`");

    if (card_config_.range[0].is_empty()) return Status::err("OOP range is empty");
    if (card_config_.range[1].is_empty()) return Status::err("IP range is empty");
    if (!card_config_.range[0].is_valid())
        return Status::err("OOP range is invalid (loaded broken data?)");
    if (!card_config_.range[1].is_valid())
        return Status::err("IP range is invalid (loaded broken data?)");

    init_hands();

    num_combinations_ = 0.0;
    for (size_t i = 0; i < private_cards_[0].size(); ++i) {
        const uint64_t oop_mask = (uint64_t{1} << private_cards_[0][i].first) |
                                  (uint64_t{1} << private_cards_[0][i].second);
        const double w1 = static_cast<double>(initial_weights_[0][i]);
        for (size_t j = 0; j < private_cards_[1].size(); ++j) {
            const uint64_t ip_mask = (uint64_t{1} << private_cards_[1][j].first) |
                                     (uint64_t{1} << private_cards_[1][j].second);
            if ((oop_mask & ip_mask) == 0)
                num_combinations_ += w1 * static_cast<double>(initial_weights_[1][j]);
        }
    }

    if (num_combinations_ == 0.0) return Status::err("Valid card assignment does not exist");
    return Status::ok();
}

void PostFlopGame::init_hands() {
    uint64_t board_mask = mask_of(card_config_.flop);
    if (card_config_.turn != NOT_DEALT) board_mask |= uint64_t{1} << card_config_.turn;
    if (card_config_.river != NOT_DEALT) board_mask |= uint64_t{1} << card_config_.river;

    for (size_t player = 0; player < 2; ++player)
        card_config_.range[player].get_hands_weights(board_mask, private_cards_[player],
                                                     initial_weights_[player]);
}

void PostFlopGame::init_card_fields() {
    // same_hand_index[p][i] is where player p's hand i sits in the opponent's
    // list, or u16 max when the opponent cannot hold it. Used by the
    // inclusion-exclusion correction at terminal nodes.
    for (size_t player = 0; player < 2; ++player) {
        std::vector<uint16_t>& shi = same_hand_index_[player];
        shi.clear();
        const std::vector<Hole>& mine = private_cards_[player];
        const std::vector<Hole>& theirs = private_cards_[player ^ 1];
        for (const Hole& hand : mine) {
            const auto it = std::lower_bound(theirs.begin(), theirs.end(), hand);
            if (it != theirs.end() && *it == hand)
                shi.push_back(static_cast<uint16_t>(it - theirs.begin()));
            else
                shi.push_back(UINT16_MAX);
        }
    }

    CardConfig::ValidIndices vi = card_config_.valid_indices(private_cards_);
    valid_indices_flop_ = std::move(vi.flop);
    valid_indices_turn_ = std::move(vi.turn);
    valid_indices_river_ = std::move(vi.river);

    hand_strength_ = card_config_.hand_strength(private_cards_);
    iso_ = card_config_.isomorphism(private_cards_);
}

std::array<uint64_t, 3> PostFlopGame::count_num_nodes() const {
    uint64_t turn_coef = 0;
    uint64_t river_coef = 0;

    if (card_config_.turn == NOT_DEALT) {
        const uint64_t flop_mask = mask_of(card_config_.flop);
        const uint64_t skip_mask = mask_of(iso_.card_turn);
        for (Card t = 0; t < 52; ++t)
            if (((uint64_t{1} << t) & (flop_mask | skip_mask)) == 0)
                river_coef += 48 - iso_.card_river[t & 3].size();
        turn_coef = 49 - iso_.card_turn.size();
    } else if (card_config_.river == NOT_DEALT) {
        turn_coef = 1;
        river_coef = 48 - iso_.card_river[card_config_.turn & 3].size();
    } else {
        turn_coef = 0;
        river_coef = 1;
    }

    const std::array<uint64_t, 3> action_nodes = count_num_action_nodes(*action_root_);
    return {action_nodes[0], action_nodes[1] * turn_coef, action_nodes[2] * river_coef};
}

Status PostFlopGame::init_root() {
    const std::array<uint64_t, 3> num_nodes = count_num_nodes();
    const uint64_t total = num_nodes[0] + num_nodes[1] + num_nodes[2];

    if (total > UINT32_MAX ||
        static_cast<uint64_t>(sizeof(PostFlopNode)) * total > static_cast<uint64_t>(PTRDIFF_MAX))
        return Status::err("Too many nodes");

    num_nodes_ = num_nodes;
    node_arena_.assign(static_cast<size_t>(total), PostFlopNode{});
    clear_storage();

    BuildInfo info;
    info.turn_index = static_cast<size_t>(num_nodes[0]);
    info.river_index = static_cast<size_t>(num_nodes[0] + num_nodes[1]);
    switch (tree_config_.initial_state) {
        case BoardState::Flop: info.flop_index += 1; break;
        case BoardState::Turn: info.turn_index += 1; break;
        case BoardState::River: info.river_index += 1; break;
    }

    node_arena_[0].turn_ = card_config_.turn;
    node_arena_[0].river_ = card_config_.river;

    build_tree_recursive(0, *action_root_, info);

    num_storage_ = info.num_storage;
    num_storage_ip_ = info.num_storage_ip;
    num_storage_chance_ = info.num_storage_chance;
    misc_memory_usage_ = memory_usage_internal();
    return Status::ok();
}

void PostFlopGame::init_interpreter() {
    for (size_t p = 0; p < 2; ++p) {
        weights_[p].assign(num_private_hands(p), 0.0f);
        normalized_weights_[p].assign(num_private_hands(p), 0.0f);
        cfvalues_cache_[p].assign(num_private_hands(p), 0.0f);
    }
}

void PostFlopGame::clear_storage() {
    storage1_.reset();
    storage2_.reset();
    storage_ip_.reset();
    storage_chance_.reset();
    storage_bytes_ = storage_ip_bytes_ = storage_chance_bytes_ = 0;
}

void PostFlopGame::build_tree_recursive(size_t node_index, const ActionTreeNode& action_node,
                                       BuildInfo& info) {
    {
        PostFlopNode& node = node_arena_[node_index];
        node.player_ = action_node.player;
        node.amount_ = action_node.amount;
        if (node.is_terminal()) return;
    }

    if (node_arena_[node_index].is_chance()) {
        push_chances(node_index, info);
        const size_t n = node_arena_[node_index].num_actions();
        const size_t child0 = node_index + node_arena_[node_index].children_offset_;
        for (size_t a = 0; a < n; ++a)
            build_tree_recursive(child0 + a, action_node.children[0], info);
    } else {
        push_actions(node_index, action_node, info);
        const size_t n = node_arena_[node_index].num_actions();
        const size_t child0 = node_index + node_arena_[node_index].children_offset_;
        for (size_t a = 0; a < n; ++a)
            build_tree_recursive(child0 + a, action_node.children[a], info);
    }
}

void PostFlopGame::push_chances(size_t node_index, BuildInfo& info) {
    const uint64_t flop_mask = mask_of(card_config_.flop);
    PostFlopNode& node = node_arena_[node_index];

    if (node.turn_ == NOT_DEALT) {
        // deal the turn
        const uint64_t skip_mask = mask_of(iso_.card_turn);
        node.children_offset_ = static_cast<uint32_t>(info.turn_index - node_index);
        for (Card card = 0; card < 52; ++card) {
            if (((uint64_t{1} << card) & (flop_mask | skip_mask)) != 0) continue;
            const size_t child_index = info.turn_index + node.num_children_;
            ++node.num_children_;
            PostFlopNode& child = node_arena_[child_index];
            child.prev_action_ = Action::chance(card);
            child.turn_ = card;
        }
        info.turn_index += node.num_children_;
    } else {
        // deal the river
        const uint64_t turn_mask = flop_mask | (uint64_t{1} << node.turn_);
        const uint64_t skip_mask = mask_of(iso_.card_river[node.turn_ & 3]);
        node.children_offset_ = static_cast<uint32_t>(info.river_index - node_index);
        for (Card card = 0; card < 52; ++card) {
            if (((uint64_t{1} << card) & (turn_mask | skip_mask)) != 0) continue;
            const size_t child_index = info.river_index + node.num_children_;
            ++node.num_children_;
            PostFlopNode& child = node_arena_[child_index];
            child.prev_action_ = Action::chance(card);
            child.turn_ = node.turn_;
            child.river_ = card;
        }
        info.river_index += node.num_children_;
    }

    const std::optional<size_t> storage_player = node.cfvalue_storage_player();
    node.num_elements_ =
        storage_player ? static_cast<uint32_t>(num_private_hands(*storage_player)) : 0;
    info.num_storage_chance += node.num_elements_;
}

void PostFlopGame::push_actions(size_t node_index, const ActionTreeNode& action_node,
                               BuildInfo& info) {
    PostFlopNode& node = node_arena_[node_index];

    const BoardState street = (node.turn_ == NOT_DEALT)
                                  ? BoardState::Flop
                                  : (node.river_ == NOT_DEALT ? BoardState::Turn : BoardState::River);
    size_t* base = nullptr;
    switch (street) {
        case BoardState::Flop: base = &info.flop_index; break;
        case BoardState::Turn: base = &info.turn_index; break;
        case BoardState::River: base = &info.river_index; break;
    }

    node.children_offset_ = static_cast<uint32_t>(*base - node_index);
    node.num_children_ = static_cast<uint16_t>(action_node.children.size());
    const size_t child0 = *base;
    *base += node.num_children_;

    for (size_t a = 0; a < node.num_children_; ++a) {
        PostFlopNode& child = node_arena_[child0 + a];
        child.prev_action_ = action_node.actions[a];
        child.turn_ = node.turn_;
        child.river_ = node.river_;
    }

    const size_t nph = num_private_hands(node.player_);
    node.num_elements_ = static_cast<uint32_t>(node.num_actions() * nph);
    // IP cfvalues are stored only at the first decision node of each street.
    const ActionKind pk = node.prev_action_.kind;
    node.num_elements_ip_ = (pk == ActionKind::None || pk == ActionKind::Chance)
                                ? static_cast<uint16_t>(num_private_hands(PLAYER_IP))
                                : 0;

    info.num_storage += node.num_elements_;
    info.num_storage_ip += node.num_elements_ip_;
}

uint64_t PostFlopGame::memory_usage_internal() const {
    uint64_t m = sizeof(PostFlopGame);

    m += vec_memory_usage(added_lines_);
    m += vec_memory_usage(removed_lines_);
    for (const auto& line : added_lines_) m += vec_memory_usage(line);
    for (const auto& line : removed_lines_) m += vec_memory_usage(line);

    m += vec_memory_usage(valid_indices_turn_);
    m += vec_memory_usage(valid_indices_river_);
    m += vec_memory_usage(hand_strength_);
    m += vec_memory_usage(iso_.ref_turn);
    m += vec_memory_usage(iso_.card_turn);
    m += vec_memory_usage(iso_.ref_river);
    for (const auto& refs : iso_.ref_river) m += vec_memory_usage(refs);
    for (const auto& cards : iso_.card_river) m += vec_memory_usage(cards);

    for (size_t p = 0; p < 2; ++p) {
        m += vec_memory_usage(initial_weights_[p]);
        m += vec_memory_usage(private_cards_[p]);
        m += vec_memory_usage(same_hand_index_[p]);
        m += vec_memory_usage(valid_indices_flop_[p]);
        for (const auto& idx : valid_indices_turn_) m += vec_memory_usage(idx[p]);
        for (const auto& idx : valid_indices_river_) m += vec_memory_usage(idx[p]);
        for (const auto& s : hand_strength_) m += vec_memory_usage(s[p]);
        for (const auto& swap : iso_.swap_turn) m += vec_memory_usage(swap[p]);
        for (const auto& list : iso_.swap_river)
            for (const auto& swap : list) m += vec_memory_usage(swap[p]);
    }

    m += vec_memory_usage(node_arena_);
    return m;
}

std::pair<uint64_t, uint64_t> PostFlopGame::memory_usage() const {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");
    const uint64_t n = 2 * num_storage_ + num_storage_ip_ + num_storage_chance_;
    return {4 * n + misc_memory_usage_, 2 * n + misc_memory_usage_};
}

void PostFlopGame::allocate_memory(bool enable_compression) {
    if (state_ <= GameState::Uninitialized) PFS_PANIC("Game is not successfully initialized");

    if (state_ == GameState::MemoryAllocated && storage_mode_ == BoardState::River &&
        is_compression_enabled_ == enable_compression)
        return;

    const uint64_t num_bytes = enable_compression ? 2 : 4;
    if (num_bytes * num_storage_ > static_cast<uint64_t>(PTRDIFF_MAX) ||
        num_bytes * num_storage_chance_ > static_cast<uint64_t>(PTRDIFF_MAX))
        PFS_PANIC("Memory usage exceeds maximum size");

    // Note the downgrade: calling this again after solving resets the state, which
    // is the sanctioned re-solve path used by the node-locking tests.
    state_ = GameState::MemoryAllocated;
    is_compression_enabled_ = enable_compression;

    clear_storage();

    storage_bytes_ = static_cast<size_t>(num_bytes * num_storage_);
    storage_ip_bytes_ = static_cast<size_t>(num_bytes * num_storage_ip_);
    storage_chance_bytes_ = static_cast<size_t>(num_bytes * num_storage_chance_);

    // Zero-initialized, and never reallocated for the lifetime of the buffers.
    storage1_ = std::make_unique<std::byte[]>(storage_bytes_ ? storage_bytes_ : 1);
    storage2_ = std::make_unique<std::byte[]>(storage_bytes_ ? storage_bytes_ : 1);
    storage_ip_ = std::make_unique<std::byte[]>(storage_ip_bytes_ ? storage_ip_bytes_ : 1);
    storage_chance_ =
        std::make_unique<std::byte[]>(storage_chance_bytes_ ? storage_chance_bytes_ : 1);

    allocate_memory_nodes();

    storage_mode_ = BoardState::River;
    target_storage_mode_ = BoardState::River;
}

void PostFlopGame::allocate_memory_nodes() {
    const size_t num_bytes = is_compression_enabled_ ? 2 : 4;
    size_t action_counter = 0;
    size_t ip_counter = 0;
    size_t chance_counter = 0;

    // One linear pass over the arena. storage1 and storage2 deliberately share the
    // same offset -- the two buffers are parallel.
    for (PostFlopNode& node : node_arena_) {
        if (node.is_terminal()) continue;
        if (node.is_chance()) {
            node.storage1_ = storage_chance_.get() + chance_counter;
            chance_counter += num_bytes * node.num_elements_;
        } else {
            node.storage1_ = storage1_.get() + action_counter;
            node.storage2_ = storage2_.get() + action_counter;
            node.storage3_ = storage_ip_.get() + ip_counter;
            action_counter += num_bytes * node.num_elements_;
            ip_counter += num_bytes * node.num_elements_ip_;
        }
    }
}

// --------------------------------------------------------------------------
// Game interface bits that need the isomorphism tables
// --------------------------------------------------------------------------

std::span<const uint8_t> PostFlopGame::isomorphic_chances(const PostFlopNode& node) const {
    if (node.turn() == NOT_DEALT) return iso_.ref_turn;
    return iso_.ref_river[node.turn()];
}

const SwapList& PostFlopGame::isomorphic_swap(const PostFlopNode& node, size_t index) const {
    if (node.turn() == NOT_DEALT) return iso_.swap_turn[iso_.card_turn[index] & 3];
    const uint8_t turn_suit = node.turn() & 3;
    return iso_.swap_river[turn_suit][iso_.card_river[turn_suit][index] & 3];
}

std::span<const float> PostFlopGame::locking_strategy(const PostFlopNode& node) const {
    if (!node.is_locked()) return {};
    const auto it = locking_strategy_.find(node_index(node));
    if (it == locking_strategy_.end()) PFS_PANIC("locked node has no locking strategy");
    return it->second;
}

void PostFlopGame::set_solved() {
    state_ = GameState::Solved;
    // Replaying the history keeps the interpreter's cached cfvalues consistent
    // with the values finalize() just wrote.
    const std::vector<size_t> history = action_history_;
    apply_history(history);
}

// --------------------------------------------------------------------------
// remove_lines -- removing chance-specific lines after the game tree is built
// but before memory is allocated. This is possible here and not on the abstract
// ActionTree because only here do turn/river nodes exist separately.
// --------------------------------------------------------------------------

namespace {

// Reclaims the storage a removed subtree would have used, and zeroes the counts
// so a second pass cannot double-count.
void collect_removed_storage(PostFlopNode& node, uint64_t& num_storage, uint64_t& num_storage_ip,
                             uint64_t& num_storage_chance) {
    if (node.is_terminal()) return;

    if (node.is_chance()) {
        num_storage_chance += node.num_elements();
        node.set_num_elements(0);
    } else {
        num_storage += node.num_elements();
        num_storage_ip += node.num_elements_ip();
        node.set_num_elements(0);
        node.set_num_elements_ip(0);
    }

    for (size_t a = 0; a < node.num_actions(); ++a)
        collect_removed_storage(node.play(a), num_storage, num_storage_ip, num_storage_chance);
}

}  // namespace

Status PostFlopGame::remove_lines(const std::vector<std::vector<Action>>& lines) {
    if (state_ <= GameState::Uninitialized)
        return Status::err("Game is not successfully initialized");
    if (state_ >= GameState::MemoryAllocated) return Status::err("Game has already been allocated");

    for (const std::vector<Action>& line : lines) {
        uint64_t s = 0, s_ip = 0, s_chance = 0;
        Status st = remove_line_recursive(root(), line, s, s_ip, s_chance);
        if (!st) return st;
        num_storage_ -= s;
        num_storage_ip_ -= s_ip;
        num_storage_chance_ -= s_chance;
    }
    return Status::ok();
}

Status PostFlopGame::remove_line_recursive(PostFlopNode& node, std::span<const Action> line,
                                          uint64_t& num_storage, uint64_t& num_storage_ip,
                                          uint64_t& num_storage_chance) {
    if (line.empty()) return Status::err("Empty line");
    if (node.is_terminal()) return Status::err("Unexpected terminal node");

    const Action action = line[0];
    const std::span<PostFlopNode> children = node.children();

    // Children are kept sorted by prev_action, so this is a binary search on the
    // derived Action ordering.
    size_t lo = 0, hi = children.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (children[mid].prev_action() < action) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= children.size() || !(children[lo].prev_action() == action))
        return Status::err("Action does not exist: " + action.to_string());
    const size_t index = lo;

    if (line.size() > 1)
        return remove_line_recursive(children[index], line.subspan(1), num_storage, num_storage_ip,
                                    num_storage_chance);

    if (node.is_chance()) return Status::err("Cannot remove a line ending in a chance action");
    if (node.num_actions() <= 1) return Status::err("Cannot remove the last action from a node");

    // 1. account for the storage the subtree would have used, plus one row of this
    //    node's own action-major block.
    num_storage += num_private_hands(node.player());
    collect_removed_storage(children[index], num_storage, num_storage_ip, num_storage_chance);

    // 2. compact the siblings down over the removed one. Each moved node ends up
    //    one slot earlier, so its relative children_offset grows by one.
    for (size_t i = index; i + 1 < node.num_actions(); ++i) {
        std::swap(children[i], children[i + 1]);
        if (children[i].children_offset() > 0) children[i].bump_children_offset();
    }
    node.set_num_children(static_cast<uint16_t>(node.num_actions() - 1));

    // 3. shrink this node's element count by the row that just went away.
    node.set_num_elements(node.num_elements() -
                          static_cast<uint32_t>(num_private_hands(node.player())));
    return Status::ok();
}

}  // namespace pfs
