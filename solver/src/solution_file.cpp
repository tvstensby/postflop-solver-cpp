#include "solution_file.hpp"

#include <core/thread_pool.hpp>
#include <pfs/card.hpp>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using nlohmann::ordered_json;

namespace pfs_solver {

namespace {

using namespace solution;

const char* const kPlayerNames[2] = {"OOP", "IP"};
const char* const kStreetNames[3] = {"flop", "turn", "river"};
constexpr int kZlibLevel = 6;

pfs::BoardState next_street(pfs::BoardState street) {
    return static_cast<pfs::BoardState>(static_cast<int>(street) + 1);
}

const char* street_name(pfs::BoardState street) { return kStreetNames[static_cast<int>(street)]; }

uint32_t crc32(const uint8_t* data, size_t size) {
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data, size));
}

// ---------------------------------------------------------------------------
// The template (§5): the action tree, built by following the first represented
// card at every chance node. The tree has the same shape for every card.
// ---------------------------------------------------------------------------

enum class NodeKind { Action, Chance, Terminal };

struct TemplateNode {
    NodeKind kind = NodeKind::Terminal;
    pfs::BoardState street = pfs::BoardState::Flop;
    size_t group = 0;
    std::optional<size_t> slot;
    std::optional<size_t> exit;
    std::optional<size_t> parent;
    size_t parent_action = 0;
    size_t player = 0;
    std::array<int32_t, 2> contributed{0, 0};
    std::vector<pfs::Action> actions;
    std::vector<size_t> children;
    bool fold = false;
    std::vector<size_t> local_path;  // interpreter actions from the root of the group
    std::vector<pfs::Action> line;   // the players' actions from the root, as in the game file
};

struct Group {
    pfs::BoardState street = pfs::BoardState::Flop;
    size_t root = 0;
    std::vector<size_t> slots;
    std::vector<size_t> exits;
};

struct Template {
    std::vector<TemplateNode> nodes;
    std::vector<Group> groups;

    void build(pfs::PostFlopGame& game) {
        groups.push_back(Group{game.tree_config().initial_state, 0, {}, {}});
        std::vector<size_t> history;
        add(game, history, 0, std::nullopt, 0, {}, {});
        game.back_to_root();
    }

private:
    size_t add(pfs::PostFlopGame& game, std::vector<size_t>& history, size_t group,
               std::optional<size_t> parent, size_t parent_action, const std::vector<size_t>& local_path,
               const std::vector<pfs::Action>& line) {
        const size_t id = nodes.size();
        nodes.emplace_back();
        {
            TemplateNode& node = nodes[id];
            node.street = groups[group].street;
            node.group = group;
            node.parent = parent;
            node.parent_action = parent_action;
            node.local_path = local_path;
            node.line = line;
        }

        game.apply_history(history);
        nodes[id].contributed = game.total_bet_amount();

        if (game.is_terminal_node()) {
            nodes[id].kind = NodeKind::Terminal;
            nodes[id].fold =
                parent && nodes[*parent].actions[parent_action].kind == pfs::ActionKind::Fold;
            return id;
        }

        nodes[id].slot = groups[group].slots.size();
        groups[group].slots.push_back(id);

        if (game.is_chance_node()) {
            nodes[id].kind = NodeKind::Chance;
            nodes[id].exit = groups[group].exits.size();
            groups[group].exits.push_back(id);
            const std::vector<pfs::Action> cards = game.available_actions();
            if (cards.empty()) throw std::runtime_error("a chance node has no cards");
            const size_t child_group = groups.size();
            groups.push_back(Group{next_street(groups[group].street), nodes.size(), {}, {}});
            history.push_back(static_cast<size_t>(cards[0].amount));
            const size_t child = add(game, history, child_group, id, 0, {}, line);
            history.pop_back();
            nodes[id].children = {child};
            return id;
        }

        nodes[id].kind = NodeKind::Action;
        nodes[id].player = game.current_player();
        nodes[id].actions = game.available_actions();
        const std::vector<pfs::Action> actions = nodes[id].actions;
        for (size_t i = 0; i < actions.size(); ++i) {
            std::vector<size_t> child_path = local_path;
            child_path.push_back(i);
            std::vector<pfs::Action> child_line = line;
            child_line.push_back(actions[i]);
            history.push_back(i);
            const size_t child = add(game, history, group, id, i, child_path, child_line);
            history.pop_back();
            nodes[id].children.push_back(child);
        }
        return id;
    }
};

// ---------------------------------------------------------------------------
// Node data (§9). No field is byte shuffled: on real solves the shuffle made
// the zlib output larger, for 16-bit and for 32-bit values.
// ---------------------------------------------------------------------------

struct Field {
    FieldId id = FieldId::Summary;
    uint8_t player = kNoPlayer;
    Encoding encoding = Encoding::F32;
    uint32_t count = 0;
    float scale = 0.0f;
    std::vector<uint8_t> bytes;
};

float finite(float v) { return std::isfinite(v) ? v : 0.0f; }

Field encode_f32(FieldId id, uint8_t player, std::span<const float> values) {
    Field f{id, player, Encoding::F32, static_cast<uint32_t>(values.size()), 0.0f, {}};
    f.bytes.resize(values.size() * 4);
    for (size_t i = 0; i < values.size(); ++i) put_f32(f.bytes.data() + 4 * i, finite(values[i]));
    return f;
}

Field encode_u16(FieldId id, uint8_t player, Encoding encoding, float scale,
                 const std::vector<uint16_t>& q) {
    Field f{id, player, encoding, static_cast<uint32_t>(q.size()), scale, {}};
    f.bytes.resize(q.size() * 2);
    for (size_t i = 0; i < q.size(); ++i) put_u16(f.bytes.data() + 2 * i, q[i]);
    return f;
}

// Each hand's frequencies are rounded with the largest-remainder method, so they add up to
// exactly 65535.
Field encode_strategy(uint8_t player, std::span<const float> strategy, size_t num_actions,
                      size_t num_hands) {
    std::vector<uint16_t> q(strategy.size());
    std::vector<double> remainder(num_actions);
    for (size_t h = 0; h < num_hands; ++h) {
        double sum = 0.0;
        for (size_t a = 0; a < num_actions; ++a)
            sum += std::max(static_cast<double>(finite(strategy[a * num_hands + h])), 0.0);
        uint32_t total = 0;
        for (size_t a = 0; a < num_actions; ++a) {
            const double s = std::max(static_cast<double>(finite(strategy[a * num_hands + h])), 0.0);
            const double x = (sum > 0.0 ? s / sum : 1.0 / static_cast<double>(num_actions)) * 65535.0;
            const double fl = std::floor(x);
            q[a * num_hands + h] = static_cast<uint16_t>(fl);
            remainder[a] = x - fl;
            total += static_cast<uint32_t>(fl);
        }
        for (uint32_t left = 65535 - total; left > 0; --left) {
            size_t best = 0;
            for (size_t a = 1; a < num_actions; ++a)
                if (remainder[a] > remainder[best]) best = a;
            ++q[best * num_hands + h];
            remainder[best] = -1.0;
        }
    }
    return encode_u16(FieldId::Strategy, player, Encoding::U16Unit, 0.0f, q);
}

Field encode_unit(FieldId id, uint8_t player, std::span<const float> values) {
    std::vector<uint16_t> q(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        const double v = std::clamp(static_cast<double>(finite(values[i])), 0.0, 1.0);
        q[i] = static_cast<uint16_t>(std::lround(v * 65535.0));
    }
    return encode_u16(id, player, Encoding::U16Unit, 0.0f, q);
}

Field encode_signed(FieldId id, uint8_t player, std::span<const float> values) {
    float max_abs = 0.0f;
    for (float v : values) max_abs = std::max(max_abs, std::fabs(finite(v)));
    const float scale = max_abs / 32767.0f;
    std::vector<uint16_t> q(values.size(), 0);
    if (scale > 0.0f) {
        for (size_t i = 0; i < values.size(); ++i) {
            const long r = std::lround(static_cast<double>(finite(values[i])) / static_cast<double>(scale));
            q[i] = static_cast<uint16_t>(static_cast<int16_t>(std::clamp(r, -32767L, 32767L)));
        }
    }
    return encode_u16(id, player, Encoding::I16Scaled, scale, q);
}

Field encode_scaled(FieldId id, uint8_t player, std::span<const float> values) {
    float max = 0.0f;
    for (float v : values) max = std::max(max, finite(v));
    const float scale = max / 65535.0f;
    std::vector<uint16_t> q(values.size(), 0);
    if (scale > 0.0f) {
        for (size_t i = 0; i < values.size(); ++i) {
            const long r = std::lround(static_cast<double>(finite(values[i])) / static_cast<double>(scale));
            q[i] = static_cast<uint16_t>(std::clamp(r, 0L, 65535L));
        }
    }
    return encode_u16(id, player, Encoding::U16Scaled, scale, q);
}

std::vector<uint8_t> assemble_payload(uint32_t template_id, const std::vector<Field>& fields) {
    size_t offset = pad8(kNodeHeaderSize + kFieldDescSize * fields.size());
    std::vector<size_t> offsets;
    for (const Field& f : fields) {
        offsets.push_back(offset);
        offset = pad8(offset + f.bytes.size());
    }
    std::vector<uint8_t> out(offset, 0);
    put_u32(out.data(), kNodeMagic);
    put_u32(out.data() + 4, template_id);
    put_u16(out.data() + 8, static_cast<uint16_t>(fields.size()));
    for (size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        uint8_t* d = out.data() + kNodeHeaderSize + kFieldDescSize * i;
        put_u8(d, static_cast<uint8_t>(f.id));
        put_u8(d + 1, f.player);
        put_u8(d + 2, static_cast<uint8_t>(f.encoding));
        put_u8(d + 3, static_cast<uint8_t>(Filter::None));
        put_u32(d + 4, f.count);
        put_f32(d + 8, f.scale);
        put_u32(d + 12, static_cast<uint32_t>(offsets[i]));
        std::copy(f.bytes.begin(), f.bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(offsets[i]));
    }
    return out;
}

double weighted_average(std::span<const float> values, std::span<const float> weights) {
    double sum = 0.0;
    double total = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        sum += static_cast<double>(values[i]) * weights[i];
        total += weights[i];
    }
    return total > 0.0 ? sum / total : 0.0;
}

double sum_of(std::span<const float> values) {
    double sum = 0.0;
    for (float v : values) sum += v;
    return sum;
}

struct NodeData {
    std::vector<uint8_t> payload;
    std::vector<double> frequencies;  // the range frequency of each action
};

// The data of the interpreter's current node, which must be an action or a chance node.
NodeData compute_node(pfs::PostFlopGame& game, uint32_t template_id, bool action_node,
                      bool full_precision) {
    game.cache_normalized_weights();
    NodeData out;
    std::vector<Field> fields;

    if (action_node) {
        const size_t player = game.current_player();
        const uint8_t p = static_cast<uint8_t>(player);
        const size_t num_hands = game.num_private_hands(player);
        const std::vector<float> strategy = game.strategy();
        const std::vector<float> action_ev = game.expected_values_detail(player);
        const size_t num_actions = strategy.size() / num_hands;
        const std::span<const float> weights = game.normalized_weights(player);

        // As rangeFrequencies in result_file.cpp.
        out.frequencies.assign(num_actions, 0.0);
        double total = 0.0;
        for (size_t h = 0; h < num_hands; ++h) {
            for (size_t a = 0; a < num_actions; ++a)
                out.frequencies[a] += static_cast<double>(weights[h]) * strategy[a * num_hands + h];
            total += weights[h];
        }
        for (double& f : out.frequencies) f = total > 0.0 ? f / total : 0.0;

        if (full_precision) {
            fields.push_back(encode_f32(FieldId::Strategy, p, strategy));
            fields.push_back(encode_f32(FieldId::ActionEv, p, action_ev));
        } else {
            fields.push_back(encode_strategy(p, strategy, num_actions, num_hands));
            fields.push_back(encode_signed(FieldId::ActionEv, p, action_ev));
        }
    }

    std::array<std::vector<float>, 2> ev;
    std::array<std::vector<float>, 2> equity;
    for (size_t q = 0; q < 2; ++q) {
        ev[q] = game.expected_values(q);
        equity[q] = game.equity(q);
    }

    for (size_t q = 0; q < 2; ++q) {
        const uint8_t p = static_cast<uint8_t>(q);
        fields.push_back(full_precision ? encode_f32(FieldId::Weight, p, game.weights(q))
                                        : encode_scaled(FieldId::Weight, p, game.weights(q)));
    }
    for (size_t q = 0; q < 2; ++q) {
        const uint8_t p = static_cast<uint8_t>(q);
        fields.push_back(full_precision ? encode_f32(FieldId::Ev, p, ev[q])
                                        : encode_signed(FieldId::Ev, p, ev[q]));
    }
    for (size_t q = 0; q < 2; ++q) {
        const uint8_t p = static_cast<uint8_t>(q);
        fields.push_back(full_precision ? encode_f32(FieldId::Equity, p, equity[q])
                                        : encode_unit(FieldId::Equity, p, equity[q]));
    }

    std::vector<float> summary(kSummaryFixed + out.frequencies.size());
    for (size_t q = 0; q < 2; ++q) {
        const std::span<const float> weights = game.normalized_weights(q);
        summary[q] = static_cast<float>(weighted_average(ev[q], weights));
        summary[2 + q] = static_cast<float>(weighted_average(equity[q], weights));
        summary[4 + q] = static_cast<float>(sum_of(game.weights(q)));
    }
    for (size_t a = 0; a < out.frequencies.size(); ++a)
        summary[kSummaryFixed + a] = static_cast<float>(out.frequencies[a]);
    fields.push_back(encode_f32(FieldId::Summary, kNoPlayer, summary));

    out.payload = assemble_payload(template_id, fields);
    return out;
}

// ---------------------------------------------------------------------------
// Blobs (§4) and the output file
// ---------------------------------------------------------------------------

std::vector<uint8_t> make_blob(BlobType type, const std::vector<uint8_t>& payload, Codec codec) {
    std::vector<uint8_t> blob(kBlobEnvelopeSize, 0);
    Codec used = Codec::None;
    if (codec == Codec::Zlib && !payload.empty()) {
        mz_ulong length = mz_compressBound(static_cast<mz_ulong>(payload.size()));
        std::vector<uint8_t> compressed(length);
        if (mz_compress2(compressed.data(), &length, payload.data(),
                         static_cast<mz_ulong>(payload.size()), kZlibLevel) == MZ_OK &&
            length < payload.size()) {
            used = Codec::Zlib;
            blob.insert(blob.end(), compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(length));
        }
    }
    if (used == Codec::None) blob.insert(blob.end(), payload.begin(), payload.end());
    put_u8(blob.data(), static_cast<uint8_t>(type));
    put_u8(blob.data() + 1, static_cast<uint8_t>(used));
    put_u32(blob.data() + 4, static_cast<uint32_t>(payload.size()));
    return blob;
}

class Output {
public:
    explicit Output(const std::filesystem::path& path)
        : stream_(path, std::ios::binary | std::ios::trunc) {}

    bool good() const { return static_cast<bool>(stream_); }
    uint64_t position() const { return position_; }

    void write(const std::vector<uint8_t>& data) {
        stream_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        position_ += data.size();
    }
    void zeros(size_t count) { write(std::vector<uint8_t>(count, 0)); }
    void align8() { zeros(pad8(position_) - position_); }

    // Overwrites earlier bytes and returns to the end.
    void patch(uint64_t offset, const std::vector<uint8_t>& data) {
        stream_.seekp(static_cast<std::streamoff>(offset));
        stream_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        stream_.seekp(static_cast<std::streamoff>(position_));
    }

    void close() { stream_.close(); }

private:
    std::ofstream stream_;
    uint64_t position_ = 0;
};

// ---------------------------------------------------------------------------
// Segments (§7, §8, §11)
// ---------------------------------------------------------------------------

class SegmentWriter {
public:
    SegmentWriter(pfs::PostFlopGame& game, const Template& tmpl, const SolutionSettings& settings,
                  Output& out)
        : game_(game), tmpl_(tmpl), settings_(settings), out_(out),
          action_threshold_(settings.prune_action_percent / 100.0),
          line_threshold_(settings.prune_line_percent / 100.0),
          turn_given_(game.card_config().turn != pfs::NOT_DEALT) {
        const std::array<pfs::Range, 2>& ranges = game.card_config().range;
        for (uint8_t s1 = 0; s1 < 4; ++s1)
            for (uint8_t s2 = 0; s2 < 4; ++s2)
                suit_isomorphic_[s1][s2] =
                    ranges[0].is_suit_isomorphic(s1, s2) && ranges[1].is_suit_isomorphic(s1, s2);
    }

    SolutionStats stats;

    // Writes the segment of a group whose root the interpreter reaches with history (which is
    // restored afterwards), followed by its node blobs and then its child segments.
    SegmentRef write(size_t group_id, std::vector<size_t>& history,
                     const std::array<uint8_t, 5>& board, double entry_line) {
        const Group& group = tmpl_.groups[group_id];
        const size_t num_slots = group.slots.size();
        const size_t num_exits = group.exits.size();
        const size_t base = history.size();

        out_.align8();
        const uint64_t table_offset = out_.position();
        std::vector<uint8_t> table(segment_table_size(num_slots, num_exits), 0);
        out_.zeros(table.size());

        // The nodes in pre-order, so a node's parent is decided before the node.
        std::vector<SlotState> states(num_slots, SlotState::Stored);
        std::vector<double> lines(num_slots, entry_line);
        std::vector<std::vector<double>> frequencies(num_slots);
        std::vector<std::vector<uint8_t>> blobs(num_slots);
        for (size_t s = 0; s < num_slots; ++s) {
            const size_t id = group.slots[s];
            const TemplateNode& node = tmpl_.nodes[id];
            if (id != group.root) {
                const size_t parent = *tmpl_.nodes[*node.parent].slot;
                if (states[parent] == SlotState::Pruned) {
                    states[s] = SlotState::Pruned;
                    continue;
                }
                const double frequency = frequencies[parent][node.parent_action];
                lines[s] = lines[parent] * frequency;
                if (frequency < action_threshold_ || lines[s] < line_threshold_) {
                    states[s] = SlotState::Pruned;
                    continue;
                }
            }
            history.resize(base);
            history.insert(history.end(), node.local_path.begin(), node.local_path.end());
            game_.apply_history(history);
            NodeData data = compute_node(game_, static_cast<uint32_t>(id),
                                         node.kind == NodeKind::Action, settings_.full_precision);
            frequencies[s] = std::move(data.frequencies);
            blobs[s] = std::move(data.payload);
        }
        history.resize(base);

        // Compressing is independent per blob; the blobs are written in slot order.
        pfs::ThreadPool::global().parallel_for(0, num_slots, [&](size_t s) {
            if (states[s] == SlotState::Stored) blobs[s] = make_blob(BlobType::Node, blobs[s], settings_.codec);
        });

        const size_t states_offset = kSegmentHeaderSize + kBlobRefSize * num_slots;
        for (size_t s = 0; s < num_slots; ++s) {
            if (states[s] == SlotState::Stored) {
                out_.align8();
                const BlobRef ref{out_.position(), static_cast<uint32_t>(blobs[s].size()),
                                  crc32(blobs[s].data(), blobs[s].size())};
                out_.write(blobs[s]);
                put_blob_ref(table.data() + kSegmentHeaderSize + kBlobRefSize * s, ref);
                ++stats.stored_nodes;
            } else {
                ++stats.pruned_nodes;
            }
            table[states_offset + s] = static_cast<uint8_t>(states[s]);
        }
        const uint64_t span = out_.position() - table_offset;
        blobs.clear();

        const size_t exits_offset = states_offset + pad8(num_slots);
        for (size_t e = 0; e < num_exits; ++e) {
            const TemplateNode& node = tmpl_.nodes[group.exits[e]];
            const size_t slot = *node.slot;
            const bool pruned = states[slot] == SlotState::Pruned;
            const pfs::BoardState street = next_street(group.street);
            const bool stored = street <= settings_.max_street;

            history.resize(base);
            history.insert(history.end(), node.local_path.begin(), node.local_path.end());
            game_.apply_history(history);
            const uint64_t possible = game_.possible_cards();
            uint64_t represented = 0;
            for (const pfs::Action& action : game_.available_actions())
                represented |= uint64_t{1} << action.amount;

            std::array<SegmentRef, 52> refs{};
            for (uint8_t card = 0; card < 52; ++card) {
                if (((possible >> card) & 1) == 0) continue;
                refs[card].kind = pruned ? EntryKind::Pruned : stored ? EntryKind::Segment : EntryKind::NotStored;
            }
            if (pruned) ++stats.pruned_exits;

            if (!pruned && stored) {
                const size_t child_group = tmpl_.nodes[node.children[0]].group;
                for (uint8_t card = 0; card < 52; ++card) {
                    if (refs[card].kind != EntryKind::Segment || ((represented >> card) & 1) == 0) continue;
                    std::array<uint8_t, 5> child_board = board;
                    *std::find(child_board.begin(), child_board.end(), pfs::NOT_DEALT) = card;
                    history.push_back(card);
                    refs[card] = write(child_group, history, child_board, lines[slot]);
                    history.pop_back();
                }
                for (uint8_t card = 0; card < 52; ++card) {
                    if (refs[card].kind != EntryKind::Segment || ((represented >> card) & 1) != 0) continue;
                    const std::optional<std::pair<uint8_t, uint8_t>> iso =
                        isomorphic_card(card, represented, board, street == pfs::BoardState::Turn);
                    if (!iso)
                        throw std::runtime_error("no represented card is isomorphic to " +
                                                 pfs::card_to_string(card).value());
                    refs[card] = refs[iso->first];
                    refs[card].suit_perm = iso->second;
                }
            }
            for (uint8_t card = 0; card < 52; ++card)
                put_segment_ref(table.data() + exits_offset + kChanceTableSize * e + kSegmentRefSize * card,
                                refs[card]);
        }
        history.resize(base);

        put_u32(table.data(), kSegmentMagic);
        put_u16(table.data() + 4, static_cast<uint16_t>(group_id));
        put_u8(table.data() + 6, static_cast<uint8_t>(group.street));
        std::copy(board.begin(), board.end(), table.begin() + 8);
        put_u32(table.data() + 16, static_cast<uint32_t>(num_slots));
        put_u32(table.data() + 20, static_cast<uint32_t>(num_exits));
        const uint32_t crc = crc32(table.data(), table.size());
        out_.patch(table_offset, table);

        ++stats.segments;
        if (settings_.progress) settings_.progress(stats.segments);
        return SegmentRef{table_offset, static_cast<uint32_t>(table.size()), static_cast<uint32_t>(span),
                          crc, EntryKind::Segment, kIdentityPerm};
    }

private:
    // For a card without a child of its own: the represented card it is isomorphic to and the
    // suit transposition between them. Mirrors CardConfig::isomorphism, which picks the lowest
    // isomorphic suit.
    std::optional<std::pair<uint8_t, uint8_t>> isomorphic_card(uint8_t card, uint64_t represented,
                                                               const std::array<uint8_t, 5>& board,
                                                               bool dealing_turn) const {
        std::array<int32_t, 4> flop_rankset{0, 0, 0, 0};
        for (size_t i = 0; i < 3; ++i) flop_rankset[board[i] & 3] |= int32_t{1} << (board[i] >> 2);
        std::array<int32_t, 4> turn_rankset = flop_rankset;
        if (board[3] != pfs::NOT_DEALT) turn_rankset[board[3] & 3] |= int32_t{1} << (board[3] >> 2);

        const uint8_t s1 = card & 3;
        for (uint8_t s2 = 0; s2 < s1; ++s2) {
            if (!suit_isomorphic_[s1][s2]) continue;
            const bool flop_same = flop_rankset[s1] == flop_rankset[s2];
            const bool same = dealing_turn ? flop_same
                                           : (flop_same || turn_given_) && turn_rankset[s1] == turn_rankset[s2];
            if (!same) continue;
            const uint8_t target = static_cast<uint8_t>(card - s1 + s2);
            if (((represented >> target) & 1) == 0) return std::nullopt;
            return std::pair<uint8_t, uint8_t>(target, perm_transposition(s1, s2));
        }
        return std::nullopt;
    }

    pfs::PostFlopGame& game_;
    const Template& tmpl_;
    const SolutionSettings& settings_;
    Output& out_;
    double action_threshold_;
    double line_threshold_;
    bool turn_given_;
    bool suit_isomorphic_[4][4]{};
};

// ---------------------------------------------------------------------------
// Metadata (§5)
// ---------------------------------------------------------------------------

ordered_json build_metadata(pfs::PostFlopGame& game, const GameFile& file, const std::string& input_name,
                            const SolveSettings& solve_settings, const SolveStats& solve_stats,
                            const SolutionSettings& settings, const Template& tmpl) {
    ordered_json out;
    out["format"] = "postflop-solver/solution";
    out["version"] = 1;
    add_solve_fields(out, file, input_name, solve_settings, solve_stats);

    ordered_json streets = ordered_json::array();
    for (int s = static_cast<int>(game.tree_config().initial_state); s <= static_cast<int>(settings.max_street); ++s)
        streets.push_back(kStreetNames[s]);
    out["storedStreets"] = streets;
    if (settings.prune_action_percent > 0.0 || settings.prune_line_percent > 0.0)
        out["pruning"] = {{"minActionFrequencyPercent", settings.prune_action_percent},
                          {"minLineFrequencyPercent", settings.prune_line_percent}};
    else
        out["pruning"] = nullptr;

    ordered_json hands;
    ordered_json weights;
    for (size_t p = 0; p < 2; ++p) {
        ordered_json names = ordered_json::array();
        for (const pfs::Hole& hole : game.private_cards(p)) names.push_back(hand_name(hole));
        ordered_json w = ordered_json::array();
        for (float v : game.initial_weights(p)) w.push_back(rounded(v, 6));
        hands[kPlayerNames[p]] = names;
        weights[kPlayerNames[p]] = w;
    }
    out["hands"] = hands;
    out["initialWeights"] = weights;

    ordered_json groups = ordered_json::array();
    for (size_t g = 0; g < tmpl.groups.size(); ++g) {
        const Group& group = tmpl.groups[g];
        groups.push_back({{"id", g},
                          {"street", street_name(group.street)},
                          {"root", group.root},
                          {"slots", group.slots},
                          {"exits", group.exits}});
    }
    out["groups"] = groups;

    const int32_t pot = game.tree_config().starting_pot;
    ordered_json tree = ordered_json::array();
    for (size_t id = 0; id < tmpl.nodes.size(); ++id) {
        const TemplateNode& node = tmpl.nodes[id];
        ordered_json j;
        j["id"] = id;
        j["kind"] = node.kind == NodeKind::Action ? "action" : node.kind == NodeKind::Chance ? "chance" : "terminal";
        j["street"] = street_name(node.street);
        j["group"] = node.group;
        if (node.slot) j["slot"] = *node.slot;
        if (node.exit) j["exit"] = *node.exit;
        j["parent"] = node.parent ? ordered_json(*node.parent) : ordered_json(nullptr);
        j["parentAction"] = node.parent ? ordered_json(node.parent_action) : ordered_json(nullptr);
        if (node.kind == NodeKind::Action) {
            for (const GameFileNode& file_node : file.nodes)
                if (file_node.history == node.line) {
                    j["gameFileId"] = file_node.id;
                    break;
                }
            j["player"] = kPlayerNames[node.player];
        }
        j["pot"] = pot + node.contributed[0] + node.contributed[1];
        j["contributed"] = {node.contributed[0], node.contributed[1]};
        if (node.kind == NodeKind::Action) {
            ordered_json actions = ordered_json::array();
            for (const pfs::Action& action : node.actions) actions.push_back(action.to_string());
            j["actions"] = actions;
        }
        if (node.kind != NodeKind::Terminal) j["children"] = node.children;
        if (node.kind == NodeKind::Terminal) {
            j["terminal"] = node.fold ? "fold" : "showdown";
            if (node.fold) j["folded"] = kPlayerNames[tmpl.nodes[*node.parent].player];
        }
        tree.push_back(j);
    }
    out["tree"] = tree;
    return out;
}

}  // namespace

pfs::Result<SolutionStats> write_solution(pfs::PostFlopGame& game, const GameFile& file,
                                          const std::string& input_name,
                                          const SolveSettings& solve_settings,
                                          const SolveStats& solve_stats,
                                          const SolutionSettings& settings,
                                          const std::filesystem::path& path) {
    using R = pfs::Result<SolutionStats>;
    if (!game.is_solved()) return R::err("the game is not solved");
    if (settings.max_street < game.tree_config().initial_state)
        return R::err(std::string("the game starts on the ") + street_name(game.tree_config().initial_state) +
                      ", so the stored streets cannot end on the " + street_name(settings.max_street));
    if (settings.codec == Codec::Zstd) return R::err("zstd is not supported by the solution writer");

    try {
        Template tmpl;
        tmpl.build(game);

        Output out(path);
        if (!out.good()) return R::err("could not create " + path.string());
        out.zeros(kHeaderSize);

        const std::string json =
            build_metadata(game, file, input_name, solve_settings, solve_stats, settings, tmpl).dump();
        const std::vector<uint8_t> metadata =
            make_blob(BlobType::Metadata, std::vector<uint8_t>(json.begin(), json.end()), settings.codec);
        const BlobRef metadata_ref{out.position(), static_cast<uint32_t>(metadata.size()),
                                   crc32(metadata.data(), metadata.size())};
        out.write(metadata);

        const pfs::CardConfig& cards = game.card_config();
        const std::array<uint8_t, 5> board{cards.flop[0], cards.flop[1], cards.flop[2], cards.turn, cards.river};
        SegmentWriter writer(game, tmpl, settings, out);
        std::vector<size_t> history;
        const SegmentRef root = writer.write(0, history, board, 1.0);

        std::vector<uint8_t> header(kHeaderSize, 0);
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        put_u16(header.data() + 8, kVersionMajor);
        put_u16(header.data() + 10, kVersionMinor);
        put_u32(header.data() + 12, kFlagCrc);
        put_u64(header.data() + 16, out.position());
        put_blob_ref(header.data() + 24, metadata_ref);
        put_segment_ref(header.data() + 40, root);
        put_u32(header.data() + 68, crc32(header.data(), 68));
        out.patch(0, header);

        SolutionStats stats = writer.stats;
        stats.file_bytes = out.position();
        const bool ok = out.good();
        out.close();
        game.back_to_root();
        if (!ok) return R::err("could not write " + path.string());
        return stats;
    } catch (const std::exception& e) {
        game.back_to_root();
        return R::err(e.what());
    }
}

}  // namespace pfs_solver
