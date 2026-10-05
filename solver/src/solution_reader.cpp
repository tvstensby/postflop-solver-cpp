#include "solution_reader.hpp"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace pfs_solver {

using namespace solution;

namespace {

template <class T>
pfs::Result<T> error(const std::string& message) {
    return pfs::Result<T>::err(message);
}

uint32_t crc32(const uint8_t* data, size_t size) {
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data, size));
}

}  // namespace

pfs::Result<std::unique_ptr<SolutionReader>> SolutionReader::open(const std::filesystem::path& path) {
    using R = pfs::Result<std::unique_ptr<SolutionReader>>;
    std::unique_ptr<SolutionReader> reader(new SolutionReader());
    reader->stream_.open(path, std::ios::binary);
    if (!reader->stream_) return R::err("could not open " + path.string());
    reader->stream_.seekg(0, std::ios::end);
    reader->length_ = static_cast<uint64_t>(reader->stream_.tellg());

    pfs::Result<std::vector<uint8_t>> header = reader->read(0, kHeaderSize);
    if (!header) return R::err(header.error());
    const uint8_t* h = header.value().data();
    if (!std::equal(kMagic.begin(), kMagic.end(), h)) return R::err("not a solution file");
    if (get_u16(h + 8) != kVersionMajor) return R::err("unsupported version");
    if (get_u32(h + 68) != crc32(h, 68)) return R::err("the header is corrupt");
    if (get_u64(h + 16) != reader->length_) return R::err("the file is truncated");
    reader->crc_ = (get_u32(h + 12) & kFlagCrc) != 0;
    const BlobRef metadata_ref = get_blob_ref(h + 24);
    reader->root_ = get_segment_ref(h + 40);

    pfs::Result<std::vector<uint8_t>> metadata = reader->read_blob(metadata_ref, BlobType::Metadata);
    if (!metadata) return R::err(metadata.error());
    try {
        reader->metadata_ = nlohmann::json::parse(metadata.value().begin(), metadata.value().end());
        const nlohmann::json& m = reader->metadata_;
        const char* names[2] = {"OOP", "IP"};
        for (size_t p = 0; p < 2; ++p) {
            reader->hand_index_[p].assign(1326, -1);
            for (const auto& name : m.at("hands").at(names[p])) {
                const std::string text = name.get<std::string>();
                pfs::Result<pfs::Card> c1 = pfs::card_from_str(text.substr(0, 2));
                pfs::Result<pfs::Card> c2 = pfs::card_from_str(text.substr(2, 2));
                if (!c1 || !c2) return R::err("invalid hand " + text);
                const pfs::Hole hole{std::min(c1.value(), c2.value()), std::max(c1.value(), c2.value())};
                reader->hand_index_[p][pfs::card_pair_to_index(hole.first, hole.second)] =
                    static_cast<int32_t>(reader->hands_[p].size());
                reader->hands_[p].push_back(hole);
            }
        }
        for (const auto& g : m.at("groups")) reader->group_roots_.push_back(g.at("root").get<size_t>());
        for (const auto& n : m.at("tree")) {
            TreeNode node;
            node.terminal = n.at("kind") == "terminal";
            node.group = n.at("group").get<size_t>();
            if (n.contains("slot")) node.slot = n.at("slot").get<size_t>();
            if (n.contains("exit")) node.exit = n.at("exit").get<size_t>();
            if (!n.at("parent").is_null()) {
                node.has_parent = true;
                node.parent = n.at("parent").get<size_t>();
            }
            if (n.contains("player")) node.player = n.at("player") == "OOP" ? 0 : 1;
            reader->tree_.push_back(node);
        }
    } catch (const std::exception& e) {
        return R::err(std::string("invalid metadata: ") + e.what());
    }
    return reader;
}

pfs::Result<std::vector<uint8_t>> SolutionReader::read(uint64_t offset, uint64_t length) {
    if (offset > length_ || length > length_ - offset) return error<std::vector<uint8_t>>("read past the end");
    std::vector<uint8_t> data(static_cast<size_t>(length));
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length));
    if (!stream_) return error<std::vector<uint8_t>>("read failed");
    return data;
}

pfs::Result<std::vector<uint8_t>> SolutionReader::read_blob(const BlobRef& ref, BlobType type) {
    using R = pfs::Result<std::vector<uint8_t>>;
    if (ref.length < kBlobEnvelopeSize) return R::err("invalid blob reference");
    R stored = read(ref.offset, ref.length);
    if (!stored) return stored;
    const std::vector<uint8_t>& bytes = stored.value();
    if (crc_ && crc32(bytes.data(), bytes.size()) != ref.crc32) return R::err("a blob is corrupt");
    if (bytes[0] != static_cast<uint8_t>(type)) return R::err("unexpected blob type");
    const uint32_t raw_length = get_u32(bytes.data() + 4);
    const uint8_t* payload = bytes.data() + kBlobEnvelopeSize;
    const size_t payload_length = bytes.size() - kBlobEnvelopeSize;

    std::vector<uint8_t> raw(raw_length);
    switch (static_cast<Codec>(bytes[1])) {
        case Codec::None:
            if (payload_length != raw_length) return R::err("bad blob length");
            std::memcpy(raw.data(), payload, raw_length);
            return raw;
        case Codec::Zlib: {
            mz_ulong length = raw_length;
            if (mz_uncompress(raw.data(), &length, payload, static_cast<mz_ulong>(payload_length)) != MZ_OK ||
                length != raw_length)
                return R::err("zlib decompression failed");
            return raw;
        }
        default:
            return R::err("unsupported codec");
    }
}

pfs::Result<const SolutionReader::Segment*> SolutionReader::segment(const SegmentRef& ref) {
    using R = pfs::Result<const Segment*>;
    const auto found = segments_.find(ref.offset);
    if (found != segments_.end()) return &found->second;

    pfs::Result<std::vector<uint8_t>> table = read(ref.offset, ref.table_length);
    if (!table) return R::err(table.error());
    const std::vector<uint8_t>& t = table.value();
    if (crc_ && crc32(t.data(), t.size()) != ref.crc32) return R::err("a segment table is corrupt");
    if (t.size() < kSegmentHeaderSize || get_u32(t.data()) != kSegmentMagic) return R::err("invalid segment table");
    const size_t num_slots = get_u32(t.data() + 16);
    const size_t num_exits = get_u32(t.data() + 20);
    if (t.size() != segment_table_size(num_slots, num_exits)) return R::err("invalid segment table size");

    Segment s;
    const uint8_t* p = t.data() + kSegmentHeaderSize;
    for (size_t i = 0; i < num_slots; ++i) s.slots.push_back(get_blob_ref(p + kBlobRefSize * i));
    p += kBlobRefSize * num_slots;
    for (size_t i = 0; i < num_slots; ++i) s.states.push_back(static_cast<SlotState>(p[i]));
    p += pad8(num_slots);
    for (size_t i = 0; i < num_exits * 52; ++i) s.exits.push_back(get_segment_ref(p + kSegmentRefSize * i));
    return &segments_.emplace(ref.offset, std::move(s)).first->second;
}

pfs::Result<std::vector<size_t>> SolutionReader::hand_permutation(size_t player, uint8_t perm) const {
    const std::vector<pfs::Hole>& hands = hands_[player];
    std::vector<size_t> out(hands.size());
    for (size_t h = 0; h < hands.size(); ++h) {
        const pfs::Card c1 = perm_card(perm, hands[h].first);
        const pfs::Card c2 = perm_card(perm, hands[h].second);
        const int32_t index = hand_index_[player][pfs::card_pair_to_index(c1, c2)];
        if (index < 0) return pfs::Result<std::vector<size_t>>::err("the ranges are not suit-symmetric");
        out[h] = static_cast<size_t>(index);
    }
    return out;
}

pfs::Result<SolutionReader::Node> SolutionReader::load(size_t template_id, std::span<const pfs::Card> cards) {
    using R = pfs::Result<Node>;
    if (template_id >= tree_.size()) return R::err("no such template node");
    const TreeNode& target = tree_[template_id];
    if (target.terminal) return R::err("terminal nodes have no data");

    Node node;
    node.template_id = template_id;
    node.player = target.player;

    // The chance nodes between the root and the node, from the root down.
    std::vector<size_t> chances;
    for (size_t g = target.group; tree_[group_roots_[g]].has_parent;) {
        const size_t chance = tree_[group_roots_[g]].parent;
        chances.push_back(chance);
        g = tree_[chance].group;
    }
    std::reverse(chances.begin(), chances.end());
    if (cards.size() != chances.size())
        return R::err("the node needs " + std::to_string(chances.size()) + " dealt card(s)");

    pfs::Result<const Segment*> seg = segment(root_);
    if (!seg) return R::err(seg.error());
    uint8_t pi = kIdentityPerm;
    for (size_t i = 0; i < chances.size(); ++i) {
        const size_t chance = chances[i];
        if (cards[i] >= 52) return R::err("invalid card");
        const SegmentRef& entry = seg.value()->exits[tree_[chance].exit * 52 + perm_card(pi, cards[i])];
        switch (entry.kind) {
            case EntryKind::Segment: break;
            case EntryKind::Impossible: node.state = State::Impossible; return node;
            case EntryKind::Empty: node.state = State::Empty; return node;
            case EntryKind::Pruned: node.state = State::Pruned; return node;
            default: node.state = State::NotStored; return node;
        }
        pi = perm_compose(entry.suit_perm, pi);
        seg = segment(entry);
        if (!seg) return R::err(seg.error());
    }

    const Segment& s = *seg.value();
    if (target.slot >= s.slots.size()) return R::err("invalid slot");
    if (s.states[target.slot] != SlotState::Stored) {
        node.state = s.states[target.slot] == SlotState::Pruned ? State::Pruned : State::NotStored;
        return node;
    }
    pfs::Result<std::vector<uint8_t>> blob = read_blob(s.slots[target.slot], BlobType::Node);
    if (!blob) return R::err(blob.error());
    const std::vector<uint8_t>& raw = blob.value();
    if (raw.size() < kNodeHeaderSize || get_u32(raw.data()) != kNodeMagic ||
        get_u32(raw.data() + 4) != template_id)
        return R::err("invalid node data");

    std::array<std::vector<size_t>, 2> perm;
    for (size_t p = 0; p < 2; ++p) {
        pfs::Result<std::vector<size_t>> r = hand_permutation(p, pi);
        if (!r) return R::err(r.error());
        perm[p] = std::move(r.value());
    }

    const size_t num_fields = get_u16(raw.data() + 8);
    if (raw.size() < kNodeHeaderSize + kFieldDescSize * num_fields) return R::err("invalid node data");
    for (size_t i = 0; i < num_fields; ++i) {
        const uint8_t* d = raw.data() + kNodeHeaderSize + kFieldDescSize * i;
        const FieldId id = static_cast<FieldId>(d[0]);
        const uint8_t player = d[1];
        const Encoding encoding = static_cast<Encoding>(d[2]);
        const Filter filter = static_cast<Filter>(d[3]);
        const size_t count = get_u32(d + 4);
        const float scale = get_f32(d + 8);
        const size_t offset = get_u32(d + 12);
        if (static_cast<uint8_t>(encoding) > 3) continue;
        const size_t width = encoding_size(encoding);
        if (offset > raw.size() || count * width > raw.size() - offset) return R::err("invalid field");

        std::vector<uint8_t> bytes(raw.begin() + static_cast<std::ptrdiff_t>(offset),
                                   raw.begin() + static_cast<std::ptrdiff_t>(offset + count * width));
        if (filter == Filter::ByteShuffle) byte_unshuffle(bytes.data(), count, width);
        std::vector<float> values(count);
        for (size_t k = 0; k < count; ++k) {
            const uint8_t* b = bytes.data() + k * width;
            switch (encoding) {
                case Encoding::F32: values[k] = get_f32(b); break;
                case Encoding::U16Unit: values[k] = static_cast<float>(get_u16(b)) / 65535.0f; break;
                case Encoding::I16Scaled: values[k] = static_cast<float>(static_cast<int16_t>(get_u16(b))) * scale; break;
                case Encoding::U16Scaled: values[k] = static_cast<float>(get_u16(b)) * scale; break;
            }
        }

        if (id == FieldId::Summary) {
            node.summary = std::move(values);
            continue;
        }
        if (player > 1) continue;
        // Translate from the stored suits into the actual ones.
        const std::vector<size_t>& pm = perm[player];
        const size_t num_hands = pm.size();
        if (num_hands == 0 || count % num_hands != 0) return R::err("invalid field length");
        std::vector<float> actual(count);
        for (size_t row = 0; row < count / num_hands; ++row)
            for (size_t h = 0; h < num_hands; ++h)
                actual[row * num_hands + h] = values[row * num_hands + pm[h]];

        switch (id) {
            case FieldId::Strategy:
                node.num_actions = count / num_hands;
                node.strategy = std::move(actual);
                break;
            case FieldId::ActionEv: node.action_ev = std::move(actual); break;
            case FieldId::Weight: node.weight[player] = std::move(actual); break;
            case FieldId::NormalizedWeight: node.normalized_weight[player] = std::move(actual); break;
            case FieldId::Ev: node.ev[player] = std::move(actual); break;
            case FieldId::Equity: node.equity[player] = std::move(actual); break;
            default: break;
        }
    }
    node.state = State::Stored;
    return node;
}

}  // namespace pfs_solver
