// Port of src/game/serialization.rs.
//
// The Rust has to smuggle the storage base pointers to the node encoder through
// thread_local Cells, because bincode gives a nested Encode impl no context. Here
// the writer walks the arena itself, so pointers become offsets inline and the
// thread-local channel is simply gone.
//
// What is NOT saved and is recomputed on load: everything derived from the card
// configuration (private cards, weights, hand strength, valid indices,
// isomorphism), the interpreter state, and -- when the target mode is River -- the
// counterfactual values, which finalize() regenerates from the saved strategy.
#include <pfs/game.hpp>
#include <pfs/utility.hpp>  // finalize, to regenerate cfvalues on load

#include "../io/binary_archive.hpp"

#include <algorithm>
#include <cstring>

namespace pfs {

Status PostFlopGame::set_target_storage_mode(BoardState mode) {
    if (mode > storage_mode_)
        return Status::err("Cannot set target to a higher value than the current storage");
    if (mode < tree_config_.initial_state)
        return Status::err("Cannot set target to a lower value than the initial state");
    target_storage_mode_ = mode;
    return Status::ok();
}

std::array<size_t, 4> PostFlopGame::num_target_storage() const {
    if (state_ <= GameState::TreeBuilt) return {0, 0, 0, 0};

    const size_t num_bytes = is_compression_enabled_ ? 2 : 4;

    if (target_storage_mode_ == BoardState::River)
        // Only the strategy is saved; the cfvalues are recomputed on load.
        return {num_bytes * static_cast<size_t>(num_storage_), 0, 0, 0};

    // Walk backwards from the street boundary until both the last player node and
    // the last chance node have been seen. This relies on the arena being
    // street-contiguous.
    size_t node_index = static_cast<size_t>(target_storage_mode_ == BoardState::Flop
                                                ? num_nodes_[0]
                                                : num_nodes_[0] + num_nodes_[1]);
    std::array<size_t, 4> num_storage{0, 0, 0, 0};

    while ((num_storage[0] == 0 || num_storage[3] == 0) && node_index > 0) {
        --node_index;
        const PostFlopNode& node = node_arena_[node_index];
        if (num_storage[0] == 0 && !node.is_terminal() && !node.is_chance()) {
            const size_t offset = node_storage_offset(node, 0);
            const size_t offset_ip = node_storage_offset(node, 2);
            num_storage[0] = offset + num_bytes * node.num_elements();
            num_storage[1] = num_storage[0];
            num_storage[2] = offset_ip + num_bytes * node.num_elements_ip();
        }
        if (num_storage[3] == 0 && node.is_chance()) {
            const size_t offset = node_storage_offset(node, 3);
            num_storage[3] = offset + num_bytes * node.num_elements();
        }
    }

    return num_storage;
}

uint64_t PostFlopGame::target_memory_usage() const {
    if (target_storage_mode_ == BoardState::River) {
        const auto [uncompressed, compressed] = memory_usage();
        return is_compression_enabled_ ? compressed : uncompressed;
    }
    const std::array<size_t, 4> n = num_target_storage();
    uint64_t sum = misc_memory_usage_;
    for (size_t x : n) sum += x;
    return sum;
}

size_t PostFlopGame::node_storage_offset(const PostFlopNode& node, int which) const {
    const std::byte* base = nullptr;
    const std::byte* ptr = nullptr;
    switch (which) {
        case 0: base = storage1_.get(); ptr = node.storage1_; break;
        case 1: base = storage2_.get(); ptr = node.storage2_; break;
        case 2: base = storage_ip_.get(); ptr = node.storage3_; break;
        default: base = storage_chance_.get(); ptr = node.storage1_; break;
    }
    if (ptr == nullptr || base == nullptr) return 0;
    return static_cast<size_t>(ptr - base);
}

// --------------------------------------------------------------------------

namespace {

void write_action(BinWriter& w, const Action& a) {
    w.u8(static_cast<uint8_t>(a.kind));
    w.i32(a.amount);
}

Action read_action(BinReader& r) {
    Action a;
    a.kind = static_cast<ActionKind>(r.u8());
    a.amount = r.i32();
    return a;
}

void write_lines(BinWriter& w, const std::vector<std::vector<Action>>& lines) {
    w.u64(lines.size());
    for (const auto& line : lines) {
        w.u64(line.size());
        for (const Action& a : line) write_action(w, a);
    }
}

std::vector<std::vector<Action>> read_lines(BinReader& r) {
    std::vector<std::vector<Action>> lines;
    const uint64_t n = r.u64();
    if (!r.ok() || n > (1u << 24)) return lines;
    lines.resize(static_cast<size_t>(n));
    for (auto& line : lines) {
        const uint64_t m = r.u64();
        if (!r.ok() || m > (1u << 16)) return lines;
        line.resize(static_cast<size_t>(m));
        for (Action& a : line) a = read_action(r);
    }
    return lines;
}

void write_bet_sizes(BinWriter& w, const BetSizeOptions& o) {
    auto one = [&](const std::vector<BetSize>& v) {
        w.u64(v.size());
        for (const BetSize& b : v) {
            w.u8(static_cast<uint8_t>(b.kind));
            w.f64(b.ratio);
            w.i32(b.amount);
            w.i32(b.cap);
        }
    };
    one(o.bet);
    one(o.raise);
}

BetSizeOptions read_bet_sizes(BinReader& r) {
    auto one = [&](std::vector<BetSize>& v) {
        const uint64_t n = r.u64();
        if (!r.ok() || n > 4096) return;
        v.resize(static_cast<size_t>(n));
        for (BetSize& b : v) {
            b.kind = static_cast<BetSizeKind>(r.u8());
            b.ratio = r.f64();
            b.amount = r.i32();
            b.cap = r.i32();
        }
    };
    BetSizeOptions o;
    one(o.bet);
    one(o.raise);
    return o;
}

void write_tree_config(BinWriter& w, const TreeConfig& c) {
    w.u8(static_cast<uint8_t>(c.initial_state));
    w.i32(c.starting_pot);
    w.i32(c.effective_stack);
    w.f64(c.rake_rate);
    w.f64(c.rake_cap);
    for (const auto& s : c.flop_bet_sizes) write_bet_sizes(w, s);
    for (const auto& s : c.turn_bet_sizes) write_bet_sizes(w, s);
    for (const auto& s : c.river_bet_sizes) write_bet_sizes(w, s);
    for (const std::optional<DonkSizeOptions>* d : {&c.turn_donk_sizes, &c.river_donk_sizes}) {
        w.boolean(d->has_value());
        if (*d) {
            w.u64((*d)->donk.size());
            for (const BetSize& b : (*d)->donk) {
                w.u8(static_cast<uint8_t>(b.kind));
                w.f64(b.ratio);
                w.i32(b.amount);
                w.i32(b.cap);
            }
        }
    }
    w.f64(c.add_allin_threshold);
    w.f64(c.force_allin_threshold);
    w.f64(c.merging_threshold);
}

TreeConfig read_tree_config(BinReader& r) {
    TreeConfig c;
    c.initial_state = static_cast<BoardState>(r.u8());
    c.starting_pot = r.i32();
    c.effective_stack = r.i32();
    c.rake_rate = r.f64();
    c.rake_cap = r.f64();
    for (auto& s : c.flop_bet_sizes) s = read_bet_sizes(r);
    for (auto& s : c.turn_bet_sizes) s = read_bet_sizes(r);
    for (auto& s : c.river_bet_sizes) s = read_bet_sizes(r);
    for (std::optional<DonkSizeOptions>* d : {&c.turn_donk_sizes, &c.river_donk_sizes}) {
        if (r.boolean()) {
            DonkSizeOptions o;
            const uint64_t n = r.u64();
            if (r.ok() && n <= 4096) {
                o.donk.resize(static_cast<size_t>(n));
                for (BetSize& b : o.donk) {
                    b.kind = static_cast<BetSizeKind>(r.u8());
                    b.ratio = r.f64();
                    b.amount = r.i32();
                    b.cap = r.i32();
                }
            }
            *d = std::move(o);
        }
    }
    c.add_allin_threshold = r.f64();
    c.force_allin_threshold = r.f64();
    c.merging_threshold = r.f64();
    return c;
}

void write_action_tree(BinWriter& w, const ActionTreeNode& node) {
    w.u8(node.player);
    w.u8(static_cast<uint8_t>(node.board_state));
    w.i32(node.amount);
    w.u64(node.actions.size());
    for (const Action& a : node.actions) write_action(w, a);
    w.u64(node.children.size());
    for (const ActionTreeNode& c : node.children) write_action_tree(w, c);
}

bool read_action_tree(BinReader& r, ActionTreeNode& node, int depth) {
    if (depth > 256) return false;
    node.player = r.u8();
    node.board_state = static_cast<BoardState>(r.u8());
    node.amount = r.i32();

    const uint64_t na = r.u64();
    if (!r.ok() || na > 4096) return false;
    node.actions.resize(static_cast<size_t>(na));
    for (Action& a : node.actions) a = read_action(r);

    const uint64_t nc = r.u64();
    if (!r.ok() || nc > 4096) return false;
    node.children.resize(static_cast<size_t>(nc));
    for (ActionTreeNode& c : node.children)
        if (!read_action_tree(r, c, depth + 1)) return false;
    return r.ok();
}

}  // namespace

void PostFlopGame::serialize(BinWriter& w) const {
    w.u8(static_cast<uint8_t>(state_));

    // card config
    for (const Range& range : card_config_.range) {
        const std::span<const float> raw = range.raw_data();
        w.blob(raw.data(), raw.size() * sizeof(float));
    }
    for (Card c : card_config_.flop) w.u8(c);
    w.u8(card_config_.turn);
    w.u8(card_config_.river);

    write_tree_config(w, tree_config_);
    write_lines(w, added_lines_);
    write_lines(w, removed_lines_);
    write_action_tree(w, *action_root_);

    w.u8(static_cast<uint8_t>(target_storage_mode_));
    for (uint64_t n : num_nodes_) w.u64(n);
    w.boolean(is_compression_enabled_);
    w.u64(num_storage_);
    w.u64(num_storage_ip_);
    w.u64(num_storage_chance_);
    w.u64(misc_memory_usage_);

    // Storage, truncated to the target mode.
    const std::array<size_t, 4> n = num_target_storage();
    const std::byte* bases[4] = {storage1_.get(), storage2_.get(), storage_ip_.get(),
                                 storage_chance_.get()};
    for (size_t i = 0; i < 4; ++i) w.blob(bases[i], n[i]);

    // Locking strategies, restricted to nodes that survive the truncation.
    const uint64_t node_limit = target_storage_mode_ == BoardState::River
                                    ? num_nodes_[0] + num_nodes_[1] + num_nodes_[2]
                                    : (target_storage_mode_ == BoardState::Flop
                                           ? num_nodes_[0]
                                           : num_nodes_[0] + num_nodes_[1]);
    uint64_t count = 0;
    for (const auto& [index, _] : locking_strategy_)
        if (index < node_limit) ++count;
    w.u64(count);
    for (const auto& [index, values] : locking_strategy_) {
        if (index >= node_limit) continue;
        w.u64(index);
        w.vec(values);
    }

    // Node array, up to the truncation point. Storage pointers become offsets.
    w.u64(node_limit);
    for (uint64_t i = 0; i < node_limit; ++i) {
        const PostFlopNode& node = node_arena_[static_cast<size_t>(i)];
        write_action(w, node.prev_action());
        w.u8(node.player_);
        w.u8(node.turn_);
        w.u8(node.river_);
        w.boolean(node.is_locked_);
        w.i32(node.amount_);
        w.u32(node.children_offset_);
        w.u16(node.num_children_);
        w.u16(node.num_elements_ip_);
        w.u32(node.num_elements_);
        w.f32(node.scale1_);
        w.f32(node.scale2_);
        w.f32(node.scale3_);
        // A flag rather than an implicit invariant: the Rust writes the offsets
        // only when storage1 is non-null but reads them whenever the base pointer
        // is set, which is consistent only by accident.
        const bool has_storage = node.storage1_ != nullptr;
        w.boolean(has_storage);
        if (has_storage) {
            if (node.is_chance()) {
                w.u64(node_storage_offset(node, 3));
            } else if (!node.is_terminal()) {
                w.u64(node_storage_offset(node, 0));
                w.u64(node_storage_offset(node, 2));
            }
        }
    }
}

Result<std::unique_ptr<PostFlopGame>> PostFlopGame::deserialize(BinReader& r) {
    auto game = std::unique_ptr<PostFlopGame>(new PostFlopGame());

    game->state_ = static_cast<GameState>(r.u8());
    if (!r.ok() || game->state_ > GameState::Solved)
        return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: bad state");

    for (Range& range : game->card_config_.range) {
        std::array<float, kNumHandIndices> raw{};
        if (!r.blob_into(raw.data(), raw.size() * sizeof(float)))
            return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: range");
        Result<Range> parsed = Range::from_raw_data(raw);
        if (!parsed) return Result<std::unique_ptr<PostFlopGame>>::err(parsed.error());
        range = parsed.value();
    }
    for (Card& c : game->card_config_.flop) c = r.u8();
    game->card_config_.turn = r.u8();
    game->card_config_.river = r.u8();

    game->tree_config_ = read_tree_config(r);
    game->added_lines_ = read_lines(r);
    game->removed_lines_ = read_lines(r);

    game->action_root_ = std::make_unique<ActionTreeNode>();
    if (!read_action_tree(r, *game->action_root_, 0))
        return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: action tree");

    game->storage_mode_ = static_cast<BoardState>(r.u8());
    game->target_storage_mode_ = game->storage_mode_;
    for (uint64_t& n : game->num_nodes_) n = r.u64();
    game->is_compression_enabled_ = r.boolean();
    game->num_storage_ = r.u64();
    game->num_storage_ip_ = r.u64();
    game->num_storage_chance_ = r.u64();
    game->misc_memory_usage_ = r.u64();

    if (!r.ok()) return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: header");

    // Allocate the buffers before reading, so the recorded offsets resolve.
    const uint64_t num_bytes = game->is_compression_enabled_ ? 2 : 4;
    const size_t full_bytes = static_cast<size_t>(num_bytes * game->num_storage_);
    const size_t full_ip_bytes = static_cast<size_t>(num_bytes * game->num_storage_ip_);
    const size_t full_chance_bytes = static_cast<size_t>(num_bytes * game->num_storage_chance_);

    auto alloc = [](size_t n) { return std::make_unique<std::byte[]>(n ? n : 1); };
    game->storage1_ = alloc(full_bytes);
    game->storage2_ = alloc(full_bytes);
    game->storage_ip_ = alloc(full_ip_bytes);
    game->storage_chance_ = alloc(full_chance_bytes);
    game->storage_bytes_ = full_bytes;
    game->storage_ip_bytes_ = full_ip_bytes;
    game->storage_chance_bytes_ = full_chance_bytes;

    // Each buffer was saved truncated to the target mode; the rest stays zeroed and
    // is regenerated by finalize() below.
    std::byte* dests[4] = {game->storage1_.get(), game->storage2_.get(), game->storage_ip_.get(),
                           game->storage_chance_.get()};
    const size_t caps[4] = {full_bytes, full_bytes, full_ip_bytes, full_chance_bytes};
    for (size_t i = 0; i < 4; ++i) {
        size_t read_len = 0;
        if (!r.blob_into_prefix(dests[i], caps[i], read_len))
            return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: storage");
    }

    const uint64_t num_locks = r.u64();
    if (!r.ok() || num_locks > (1u << 24))
        return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: locks");
    for (uint64_t i = 0; i < num_locks; ++i) {
        const uint64_t index = r.u64();
        std::vector<float> values = r.vec<float>();
        if (!r.ok()) return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: locks");
        game->locking_strategy_[static_cast<size_t>(index)] = std::move(values);
    }

    const uint64_t num_saved_nodes = r.u64();
    const uint64_t total_nodes =
        game->num_nodes_[0] + game->num_nodes_[1] + game->num_nodes_[2];
    if (!r.ok() || num_saved_nodes > total_nodes || total_nodes > UINT32_MAX)
        return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: node count");

    game->node_arena_.assign(static_cast<size_t>(total_nodes), PostFlopNode{});
    for (uint64_t i = 0; i < num_saved_nodes; ++i) {
        PostFlopNode& node = game->node_arena_[static_cast<size_t>(i)];
        node.prev_action_ = read_action(r);
        node.player_ = r.u8();
        node.turn_ = r.u8();
        node.river_ = r.u8();
        node.is_locked_ = r.boolean();
        node.amount_ = r.i32();
        node.children_offset_ = r.u32();
        node.num_children_ = r.u16();
        node.num_elements_ip_ = r.u16();
        node.num_elements_ = r.u32();
        node.scale1_ = r.f32();
        node.scale2_ = r.f32();
        node.scale3_ = r.f32();

        if (r.boolean()) {
            if (node.is_chance()) {
                const uint64_t off = r.u64();
                if (off > full_chance_bytes)
                    return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: offset");
                node.storage1_ = game->storage_chance_.get() + off;
            } else if (!node.is_terminal()) {
                const uint64_t off = r.u64();
                const uint64_t off_ip = r.u64();
                if (off > full_bytes || off_ip > full_ip_bytes)
                    return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: offset");
                // storage1 and storage2 are parallel buffers at the same offset.
                node.storage1_ = game->storage1_.get() + off;
                node.storage2_ = game->storage2_.get() + off;
                node.storage3_ = game->storage_ip_.get() + off_ip;
            }
        }
    }

    if (!r.ok()) return Result<std::unique_ptr<PostFlopGame>>::err("Corrupt data: nodes");

    Status st = game->check_card_config();
    if (!st) return Result<std::unique_ptr<PostFlopGame>>::err(st.error());
    game->init_card_fields();
    game->init_interpreter();
    game->back_to_root();

    // Only the strategy was saved, so regenerate the counterfactual values.
    if (game->storage_mode_ == BoardState::River && game->state_ == GameState::Solved) {
        game->state_ = GameState::MemoryAllocated;
        finalize(*game);
    }

    return Result<std::unique_ptr<PostFlopGame>>(std::move(game));
}

}  // namespace pfs
