#include <pfs/action_tree.hpp>

#include <algorithm>
#include <cmath>

namespace pfs {

// ---------------------------------------------------------------------------
// Action
// ---------------------------------------------------------------------------

std::string Action::to_string() const {
    switch (kind) {
        case ActionKind::None: return "None";
        case ActionKind::Fold: return "Fold";
        case ActionKind::Check: return "Check";
        case ActionKind::Call: return "Call";
        case ActionKind::Bet: return "Bet(" + std::to_string(amount) + ")";
        case ActionKind::Raise: return "Raise(" + std::to_string(amount) + ")";
        case ActionKind::AllIn: return "AllIn(" + std::to_string(amount) + ")";
        case ActionKind::Chance: return "Chance(" + std::to_string(amount) + ")";
    }
    return "?";
}

std::string actions_to_string(std::span<const Action> actions) {
    std::string s = "[";
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i) s += ", ";
        s += actions[i].to_string();
    }
    s += "]";
    return s;
}

SearchResult binary_search_action(std::span<const Action> actions, const Action& a) noexcept {
    const auto it = std::lower_bound(actions.begin(), actions.end(), a);
    const size_t index = static_cast<size_t>(it - actions.begin());
    const bool found = it != actions.end() && !(a < *it);
    return {found, index};
}

namespace {

// Rust's i32::clamp panics if min > max; assert the same precondition.
int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (lo > hi) PFS_PANIC("clamp: min > max");
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t round_to_i32(double v) { return static_cast<int32_t>(std::round(v)); }

// Carries the state threaded down the tree during construction.
struct BuildTreeInfo {
    Action prev_action = Action::none();
    int32_t num_bets = 0;
    bool allin_flag = false;
    bool oop_call_flag = false;
    std::array<int32_t, 2> stack{0, 0};
    int32_t prev_amount = 0;

    static BuildTreeInfo create(int32_t stack) {
        BuildTreeInfo i;
        i.stack = {stack, stack};
        return i;
    }

    BuildTreeInfo create_next(uint8_t player, Action action) const {
        BuildTreeInfo n = *this;
        n.prev_action = action;
        const size_t p = player;
        const size_t o = player ^ 1u;

        switch (action.kind) {
            case ActionKind::Check:
                n.oop_call_flag = false;
                break;
            case ActionKind::Call:
                n.num_bets = 0;
                n.oop_call_flag = (player == PLAYER_OOP);
                n.stack[p] = n.stack[o];
                n.prev_amount = 0;
                break;
            case ActionKind::Bet:
            case ActionKind::Raise:
            case ActionKind::AllIn: {
                const int32_t to_call = n.stack[p] - n.stack[o];
                n.num_bets += 1;
                n.allin_flag = (action.kind == ActionKind::AllIn);
                n.stack[p] -= action.amount - n.prev_amount + to_call;
                n.prev_amount = action.amount;
                break;
            }
            default:
                break;
        }
        return n;
    }
};

// PioSOLVER's merging: walk the amounts in descending order and keep one only if
// it is far enough below the last kept one. cur_amount starts at INT32_MAX so the
// largest bet is always kept.
std::vector<Action> merge_bet_actions(const std::vector<Action>& actions, int32_t pot,
                                      int32_t offset, double param) {
    constexpr double kEps = 1e-12;
    auto get_amount = [](const Action& a) { return a.is_bet_like() ? a.amount : -1; };

    int32_t cur_amount = INT32_MAX;
    std::vector<Action> ret;
    ret.reserve(actions.size());

    for (size_t i = actions.size(); i-- > 0;) {
        const Action action = actions[i];
        const int32_t amount = get_amount(action);
        if (amount > 0) {
            const double ratio = static_cast<double>(amount - offset) / static_cast<double>(pot);
            const double cur_ratio =
                static_cast<double>(cur_amount - offset) / static_cast<double>(pot);
            const double threshold_ratio = (cur_ratio - param) / (1.0 + param);
            if (ratio < threshold_ratio * (1.0 - kEps)) {
                ret.push_back(action);
                cur_amount = amount;
            }
        } else {
            ret.push_back(action);
        }
    }

    std::reverse(ret.begin(), ret.end());
    return ret;
}

void count_recursive(const ActionTreeNode& node, size_t street, std::array<uint64_t, 3>& count) {
    count[street] += 1;
    if (node.is_terminal()) {
        // nothing
    } else if (node.is_chance()) {
        count_recursive(node.children[0], street + 1, count);
    } else {
        for (const ActionTreeNode& c : node.children) count_recursive(c, street, count);
    }
}

bool starts_with(const std::vector<Action>& v, std::span<const Action> prefix) {
    if (v.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (!(v[i] == prefix[i])) return false;
    return true;
}

}  // namespace

std::array<uint64_t, 3> count_num_action_nodes(const ActionTreeNode& node) {
    std::array<uint64_t, 3> ret{0, 0, 0};
    count_recursive(node, 0, ret);
    // Shift the counts when the tree starts on the turn or the river.
    if (ret[1] == 0) {
        ret = {0, 0, ret[0]};
    } else if (ret[2] == 0) {
        ret = {0, ret[0], ret[1]};
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Tree construction
// ---------------------------------------------------------------------------

namespace {

// Free functions rather than members so the recursion can be shared without
// exposing BuildTreeInfo in the header.
void build_tree_recursive(const TreeConfig& cfg, ActionTreeNode& node, const BuildTreeInfo& info);

void push_actions(const TreeConfig& cfg, ActionTreeNode& node, const BuildTreeInfo& info) {
    const uint8_t player = node.player;
    const uint8_t opponent = static_cast<uint8_t>(node.player ^ 1u);

    const int32_t player_stack = info.stack[player];
    const int32_t opponent_stack = info.stack[opponent];
    const int32_t prev_amount = info.prev_amount;
    const int32_t to_call = player_stack - opponent_stack;

    const int32_t pot = cfg.starting_pot + 2 * (node.amount + to_call);
    const int32_t max_amount = opponent_stack + prev_amount;
    const int32_t min_amount = clamp_i32(prev_amount + to_call, 1, max_amount);

    const double spr_after_call =
        static_cast<double>(opponent_stack) / static_cast<double>(pot);
    auto compute_geometric = [&](int32_t num_streets, double max_ratio) {
        const double ratio =
            (std::pow(2.0 * spr_after_call + 1.0, 1.0 / static_cast<double>(num_streets)) - 1.0) /
            2.0;
        return round_to_i32(static_cast<double>(pot) * std::min(ratio, max_ratio));
    };

    const std::array<BetSizeOptions, 2>* bet_options = nullptr;
    const std::optional<DonkSizeOptions>* donk_options = nullptr;
    int32_t num_remaining_streets = 0;
    static const std::optional<DonkSizeOptions> kNoDonk;
    switch (node.board_state) {
        case BoardState::Flop:
            bet_options = &cfg.flop_bet_sizes;
            donk_options = &kNoDonk;
            num_remaining_streets = 3;
            break;
        case BoardState::Turn:
            bet_options = &cfg.turn_bet_sizes;
            donk_options = &cfg.turn_donk_sizes;
            num_remaining_streets = 2;
            break;
        case BoardState::River:
            bet_options = &cfg.river_bet_sizes;
            donk_options = &cfg.river_donk_sizes;
            num_remaining_streets = 1;
            break;
    }

    std::vector<Action> actions;

    auto push_open_sizes = [&](const std::vector<BetSize>& sizes) {
        for (const BetSize& s : sizes) {
            switch (s.kind) {
                case BetSizeKind::PotRelative:
                    actions.push_back(Action::bet(round_to_i32(static_cast<double>(pot) * s.ratio)));
                    break;
                case BetSizeKind::PrevBetRelative:
                    PFS_PANIC("Unexpected `PrevBetRelative`");
                    break;
                case BetSizeKind::Additive:
                    actions.push_back(Action::bet(s.amount));
                    break;
                case BetSizeKind::Geometric: {
                    const int32_t streets = s.amount == 0 ? num_remaining_streets : s.amount;
                    actions.push_back(Action::bet(compute_geometric(streets, s.ratio)));
                    break;
                }
                case BetSizeKind::AllIn:
                    actions.push_back(Action::all_in(max_amount));
                    break;
            }
        }
    };

    const bool is_donk_spot = donk_options->has_value() &&
                              info.prev_action.kind == ActionKind::Chance && info.oop_call_flag;
    const bool is_open_spot = info.prev_action.kind == ActionKind::None ||
                              info.prev_action.kind == ActionKind::Check ||
                              info.prev_action.kind == ActionKind::Chance;

    if (is_donk_spot) {
        actions.push_back(Action::check());
        push_open_sizes((*donk_options)->donk);
        if (max_amount <= round_to_i32(static_cast<double>(pot) * cfg.add_allin_threshold))
            actions.push_back(Action::all_in(max_amount));
    } else if (is_open_spot) {
        actions.push_back(Action::check());
        push_open_sizes((*bet_options)[player].bet);
        if (max_amount <= round_to_i32(static_cast<double>(pot) * cfg.add_allin_threshold))
            actions.push_back(Action::all_in(max_amount));
    } else {
        actions.push_back(Action::fold());
        actions.push_back(Action::call());

        if (!info.allin_flag) {
            for (const BetSize& s : (*bet_options)[player].raise) {
                switch (s.kind) {
                    case BetSizeKind::PotRelative:
                        actions.push_back(Action::raise(
                            prev_amount + round_to_i32(static_cast<double>(pot) * s.ratio)));
                        break;
                    case BetSizeKind::PrevBetRelative:
                        actions.push_back(Action::raise(
                            round_to_i32(static_cast<double>(prev_amount) * s.ratio)));
                        break;
                    case BetSizeKind::Additive:
                        if (s.cap == 0 || info.num_bets <= s.cap)
                            actions.push_back(Action::raise(prev_amount + s.amount));
                        break;
                    case BetSizeKind::Geometric: {
                        const int32_t base =
                            s.amount == 0 ? num_remaining_streets : s.amount;
                        const int32_t streets = std::max(base - info.num_bets + 1, 1);
                        actions.push_back(
                            Action::raise(prev_amount + compute_geometric(streets, s.ratio)));
                        break;
                    }
                    case BetSizeKind::AllIn:
                        actions.push_back(Action::all_in(max_amount));
                        break;
                }
            }

            const double allin_threshold =
                static_cast<double>(pot) * cfg.add_allin_threshold;
            if (max_amount <= prev_amount + round_to_i32(allin_threshold))
                actions.push_back(Action::all_in(max_amount));
        }
    }

    auto is_above_threshold = [&](int32_t amount) {
        const int32_t new_amount_diff = amount - prev_amount;
        const int32_t new_pot = pot + 2 * new_amount_diff;
        const int32_t threshold =
            round_to_i32(static_cast<double>(new_pot) * cfg.force_allin_threshold);
        return max_amount <= amount + threshold;
    };

    // Clamp, then convert to all-in when the SPR after a call is small enough.
    for (Action& action : actions) {
        if (action.kind != ActionKind::Bet && action.kind != ActionKind::Raise) continue;
        const int32_t clamped = clamp_i32(action.amount, min_amount, max_amount);
        if (is_above_threshold(clamped)) {
            action = Action::all_in(max_amount);
        } else if (clamped != action.amount) {
            action.amount = clamped;
        }
    }

    std::sort(actions.begin(), actions.end());
    actions.erase(std::unique(actions.begin(), actions.end(),
                              [](const Action& a, const Action& b) { return a == b; }),
                  actions.end());

    actions = merge_bet_actions(actions, pot, prev_amount, cfg.merging_threshold);

    const uint8_t player_after_call =
        node.board_state == BoardState::River
            ? PLAYER_TERMINAL_FLAG
            : static_cast<uint8_t>(PLAYER_CHANCE_FLAG | player);
    const uint8_t player_after_check = player == PLAYER_OOP ? opponent : player_after_call;

    for (const Action& action : actions) {
        int32_t amount = node.amount;
        uint8_t next_player = 0;
        switch (action.kind) {
            case ActionKind::Fold:
                next_player = static_cast<uint8_t>(PLAYER_FOLD_FLAG | player);
                break;
            case ActionKind::Check:
                next_player = player_after_check;
                break;
            case ActionKind::Call:
                amount += to_call;
                next_player = player_after_call;
                break;
            case ActionKind::Bet:
            case ActionKind::Raise:
            case ActionKind::AllIn:
                amount += to_call;
                next_player = opponent;
                break;
            default:
                PFS_PANIC("Unexpected action");
        }

        node.actions.push_back(action);
        ActionTreeNode child;
        child.player = next_player;
        child.board_state = node.board_state;
        child.amount = amount;
        node.children.push_back(std::move(child));
    }
}

void build_tree_recursive(const TreeConfig& cfg, ActionTreeNode& node, const BuildTreeInfo& info) {
    if (node.is_terminal()) return;

    if (node.is_chance()) {
        const BoardState next_state =
            node.board_state == BoardState::Flop ? BoardState::Turn : BoardState::River;
        if (node.board_state == BoardState::River) PFS_UNREACHABLE();

        uint8_t next_player;
        if (!info.allin_flag) {
            next_player = PLAYER_OOP;
        } else if (node.board_state == BoardState::Flop) {
            next_player = static_cast<uint8_t>(PLAYER_CHANCE_FLAG | PLAYER_CHANCE);
        } else {
            next_player = PLAYER_TERMINAL_FLAG;
        }

        node.actions.push_back(Action::chance(0));
        ActionTreeNode child;
        child.player = next_player;
        child.board_state = next_state;
        child.amount = node.amount;
        node.children.push_back(std::move(child));

        build_tree_recursive(cfg, node.children[0], info.create_next(0, Action::chance(0)));
        return;
    }

    push_actions(cfg, node, info);
    for (size_t i = 0; i < node.actions.size(); ++i)
        build_tree_recursive(cfg, node.children[i], info.create_next(node.player, node.actions[i]));
}

void invalid_terminals_recursive(const ActionTreeNode& node,
                                 std::vector<std::vector<Action>>& result,
                                 std::vector<Action>& line) {
    if (node.is_terminal()) {
        // nothing
    } else if (node.children.empty()) {
        result.push_back(line);
    } else if (node.is_chance()) {
        invalid_terminals_recursive(node.children[0], result, line);
    } else {
        for (size_t i = 0; i < node.actions.size(); ++i) {
            line.push_back(node.actions[i]);
            invalid_terminals_recursive(node.children[i], result, line);
            line.pop_back();
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ActionTree
// ---------------------------------------------------------------------------

Status ActionTree::check_config(const TreeConfig& c) {
    if (c.starting_pot <= 0)
        return Status::err("Starting pot must be positive: " + std::to_string(c.starting_pot));
    if (c.effective_stack <= 0)
        return Status::err("Effective stack must be positive: " +
                           std::to_string(c.effective_stack));
    if (c.rake_rate < 0.0)
        return Status::err("Rake rate must be non-negative: " + std::to_string(c.rake_rate));
    if (c.rake_rate > 1.0)
        return Status::err("Rake rate must be less than or equal to 1.0: " +
                           std::to_string(c.rake_rate));
    if (c.rake_cap < 0.0)
        return Status::err("Rake cap must be non-negative: " + std::to_string(c.rake_cap));
    if (c.add_allin_threshold < 0.0)
        return Status::err("Add all-in threshold must be non-negative: " +
                           std::to_string(c.add_allin_threshold));
    if (c.force_allin_threshold < 0.0)
        return Status::err("Force all-in threshold must be non-negative: " +
                           std::to_string(c.force_allin_threshold));
    if (c.merging_threshold < 0.0)
        return Status::err("Merging threshold must be non-negative: " +
                           std::to_string(c.merging_threshold));
    return Status::ok();
}

Result<ActionTree> ActionTree::create(TreeConfig config) {
    Status st = check_config(config);
    if (!st) return Result<ActionTree>::err(st.error());
    ActionTree tree;
    tree.config_ = std::move(config);
    tree.build_tree();
    return Result<ActionTree>(std::move(tree));
}

void ActionTree::build_tree() {
    *root_ = ActionTreeNode();
    root_->board_state = config_.initial_state;
    build_tree_recursive(config_, *root_, BuildTreeInfo::create(config_.effective_stack));
}

const ActionTreeNode* ActionTree::current_node() const {
    const ActionTreeNode* node = root_.get();
    for (const Action& action : history_) {
        while (node->is_chance()) node = &node->children[0];
        const auto it = std::find(node->actions.begin(), node->actions.end(), action);
        if (it == node->actions.end()) PFS_PANIC("action not found in history");
        node = &node->children[static_cast<size_t>(it - node->actions.begin())];
    }
    return node;
}

const ActionTreeNode* ActionTree::current_node_skip_chance() const {
    const ActionTreeNode* node = current_node();
    while (node->is_chance()) node = &node->children[0];
    return node;
}

std::vector<std::vector<Action>> ActionTree::invalid_terminals() const {
    std::vector<std::vector<Action>> ret;
    std::vector<Action> line;
    invalid_terminals_recursive(*root_, ret, line);
    return ret;
}

bool ActionTree::is_terminal_node() const { return current_node_skip_chance()->is_terminal(); }

bool ActionTree::is_chance_node() const {
    return current_node()->is_chance() && !is_terminal_node();
}

std::span<const Action> ActionTree::available_actions() const {
    return current_node_skip_chance()->actions;
}

Status ActionTree::play(Action action) {
    const ActionTreeNode* node = current_node_skip_chance();
    if (std::find(node->actions.begin(), node->actions.end(), action) == node->actions.end())
        return Status::err("Action `" + action.to_string() + "` is not available");
    history_.push_back(action);
    return Status::ok();
}

Status ActionTree::undo() {
    if (history_.empty()) return Status::err("No action to undo");
    history_.pop_back();
    return Status::ok();
}

Status ActionTree::apply_history(std::span<const Action> history) {
    back_to_root();
    for (const Action& a : history) {
        Status st = play(a);
        if (!st) return st;
    }
    return Status::ok();
}

Status ActionTree::add_action(Action action) {
    std::vector<Action> line = history_;
    line.push_back(action);
    return add_line(line);
}

Status ActionTree::remove_action(Action action) {
    std::vector<Action> line = history_;
    line.push_back(action);
    return remove_line(line);
}

Status ActionTree::remove_current_node() {
    const std::vector<Action> line = history_;
    return remove_line(line);
}

ActionTree::Ejected ActionTree::eject() && {
    return Ejected{std::move(config_), std::move(added_lines_), std::move(removed_lines_),
                   std::move(root_)};
}

namespace {

// Returns whether the requested Bet/Raise was replaced by AllIn because the
// amount equalled max_amount.
Result<bool> add_line_recursive(const TreeConfig& cfg, ActionTreeNode& node,
                                std::span<const Action> line, bool was_removed,
                                const BuildTreeInfo& info) {
    if (line.empty()) return Result<bool>::err("Empty line");
    if (node.is_terminal()) return Result<bool>::err("Unexpected terminal node");
    if (node.is_chance())
        return add_line_recursive(cfg, node.children[0], line, was_removed,
                                  info.create_next(0, Action::chance(0)));

    Action action = line[0];
    const SearchResult sr = binary_search_action(node.actions, action);
    const uint8_t player = node.player;
    const uint8_t opponent = static_cast<uint8_t>(node.player ^ 1u);

    if (line.size() > 1) {
        if (!sr.found) return Result<bool>::err("Action does not exist: " + action.to_string());
        return add_line_recursive(cfg, node.children[sr.index], line.subspan(1), was_removed,
                                  info.create_next(player, action));
    }

    if (sr.found) return Result<bool>::err("Action already exists: " + action.to_string());

    if (info.allin_flag && action.is_bet_like())
        return Result<bool>::err("Bet action after all-in: " + action.to_string());

    const int32_t player_stack = info.stack[player];
    const int32_t opponent_stack = info.stack[opponent];
    const int32_t prev_amount = info.prev_amount;
    const int32_t to_call = player_stack - opponent_stack;
    const int32_t max_amount = opponent_stack + prev_amount;
    const int32_t min_amount = clamp_i32(prev_amount + to_call, 1, max_amount);

    bool is_replaced = false;
    if ((action.kind == ActionKind::Bet || action.kind == ActionKind::Raise) &&
        action.amount == max_amount) {
        is_replaced = true;
        action = Action::all_in(action.amount);
    }

    bool is_valid_bet = false;
    if (action.kind == ActionKind::Bet && action.amount >= min_amount &&
        action.amount < max_amount) {
        is_valid_bet = info.prev_action.kind == ActionKind::None ||
                       info.prev_action.kind == ActionKind::Check ||
                       info.prev_action.kind == ActionKind::Chance;
    } else if (action.kind == ActionKind::Raise && action.amount >= min_amount &&
               action.amount < max_amount) {
        is_valid_bet = info.prev_action.kind == ActionKind::Bet ||
                       info.prev_action.kind == ActionKind::Raise;
    } else if (action.kind == ActionKind::AllIn) {
        is_valid_bet = action.amount == max_amount;
    }

    if (!was_removed && !is_valid_bet) {
        if (action.kind == ActionKind::Bet || action.kind == ActionKind::Raise)
            return Result<bool>::err("Invalid bet amount: " + std::to_string(action.amount) +
                                     " (min: " + std::to_string(min_amount) +
                                     ", max: " + std::to_string(max_amount) + ")");
        if (action.kind == ActionKind::AllIn)
            return Result<bool>::err("Invalid all-in amount: " + std::to_string(action.amount) +
                                     " (expected: " + std::to_string(max_amount) + ")");
        return Result<bool>::err("Invalid action: " + action.to_string());
    }

    const uint8_t player_after_call =
        node.board_state == BoardState::River
            ? PLAYER_TERMINAL_FLAG
            : static_cast<uint8_t>(PLAYER_CHANCE_FLAG | player);
    const uint8_t player_after_check = player == PLAYER_OOP ? opponent : player_after_call;

    int32_t amount = node.amount;
    uint8_t next_player = 0;
    switch (action.kind) {
        case ActionKind::Fold:
            next_player = static_cast<uint8_t>(PLAYER_FOLD_FLAG | player);
            break;
        case ActionKind::Check:
            next_player = player_after_check;
            break;
        case ActionKind::Call:
            amount += to_call;
            next_player = player_after_call;
            break;
        case ActionKind::Bet:
        case ActionKind::Raise:
        case ActionKind::AllIn:
            amount += to_call;
            next_player = opponent;
            break;
        default:
            return Result<bool>::err("Unexpected action: " + action.to_string());
    }

    ActionTreeNode child;
    child.player = next_player;
    child.board_state = node.board_state;
    child.amount = amount;

    node.actions.insert(node.actions.begin() + static_cast<ptrdiff_t>(sr.index), action);
    node.children.insert(node.children.begin() + static_cast<ptrdiff_t>(sr.index),
                         std::move(child));

    build_tree_recursive(cfg, node.children[sr.index], info.create_next(player, action));
    return is_replaced;
}

Status remove_line_recursive(ActionTreeNode& node, std::span<const Action> line) {
    if (line.empty()) return Status::err("Empty line");
    if (node.is_terminal()) return Status::err("Unexpected terminal node");
    if (node.is_chance()) return remove_line_recursive(node.children[0], line);

    const Action action = line[0];
    const SearchResult sr = binary_search_action(node.actions, action);
    if (!sr.found) return Status::err("Action does not exist: " + action.to_string());

    if (line.size() > 1) return remove_line_recursive(node.children[sr.index], line.subspan(1));

    node.actions.erase(node.actions.begin() + static_cast<ptrdiff_t>(sr.index));
    node.children.erase(node.children.begin() + static_cast<ptrdiff_t>(sr.index));
    return Status::ok();
}

std::array<int32_t, 2> total_bet_amount_recursive(const TreeConfig& cfg,
                                                  const ActionTreeNode& node,
                                                  std::span<const Action> line,
                                                  const BuildTreeInfo& info) {
    if (line.empty() || node.is_terminal()) {
        const int32_t stack = cfg.effective_stack;
        return {stack - info.stack[0], stack - info.stack[1]};
    }
    if (node.is_chance())
        return total_bet_amount_recursive(cfg, node.children[0], line, info);

    const Action action = line[0];
    const SearchResult sr = binary_search_action(node.actions, action);
    if (!sr.found) PFS_PANIC("Action does not exist");
    return total_bet_amount_recursive(cfg, node.children[sr.index], line.subspan(1),
                                      info.create_next(node.player, action));
}

}  // namespace

Status ActionTree::add_line(std::span<const Action> line) {
    const std::vector<Action> line_vec(line.begin(), line.end());

    size_t removed_index = removed_lines_.size();
    for (size_t i = 0; i < removed_lines_.size(); ++i)
        if (removed_lines_[i] == line_vec) {
            removed_index = i;
            break;
        }
    const bool was_removed = removed_index < removed_lines_.size();

    Result<bool> res = add_line_recursive(config_, *root_, line, was_removed,
                                         BuildTreeInfo::create(config_.effective_stack));
    if (!res) return Status::err(res.error());

    if (was_removed) {
        removed_lines_.erase(removed_lines_.begin() + static_cast<ptrdiff_t>(removed_index));
    } else {
        std::vector<Action> added = line_vec;
        if (res.value() && !added.empty()) {
            Action& last = added.back();
            if (last.kind == ActionKind::Bet || last.kind == ActionKind::Raise)
                last = Action::all_in(last.amount);
        }
        added_lines_.push_back(std::move(added));
    }
    return Status::ok();
}

Status ActionTree::remove_line(std::span<const Action> line) {
    Status st = remove_line_recursive(*root_, line);
    if (!st) return st;

    const std::vector<Action> line_vec(line.begin(), line.end());
    bool was_added = false;
    for (const auto& l : added_lines_)
        if (l == line_vec) {
            was_added = true;
            break;
        }

    auto drop_prefixed = [&](std::vector<std::vector<Action>>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const std::vector<Action>& l) { return starts_with(l, line); }),
                v.end());
    };
    drop_prefixed(added_lines_);
    drop_prefixed(removed_lines_);

    if (!was_added) removed_lines_.push_back(line_vec);

    if (starts_with(history_, line)) history_.resize(line.size() - 1);
    return Status::ok();
}

std::array<int32_t, 2> ActionTree::total_bet_amount() const {
    return total_bet_amount_recursive(config_, *root_, history_,
                                      BuildTreeInfo::create(config_.effective_stack));
}

}  // namespace pfs
