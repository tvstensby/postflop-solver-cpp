#include <pfs/range.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace pfs {

// ---------------------------------------------------------------------------
// Shortest round-trip float formatting (Rust's f32 Display)
// ---------------------------------------------------------------------------

std::string f32_to_shortest(float v) {
    char buf[64];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    // No precision argument => shortest representation that round-trips.
    const auto res = std::to_chars(buf, buf + sizeof buf, v);
    if (res.ec == std::errc{}) return std::string(buf, res.ptr);
#endif
    // Fallback: first precision whose output round-trips.
    for (int p = 1; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, static_cast<double>(v));
        if (std::strtof(buf, nullptr) == v) return std::string(buf);
    }
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    return std::string(buf);
}

namespace {

// ---------------------------------------------------------------------------
// Rank / suit character helpers (duplicated from card.cpp because those are
// file-local there; kept in sync deliberately)
// ---------------------------------------------------------------------------

bool is_rank_char(char c) {
    switch (c) {
        case 'A': case 'a': case 'K': case 'k': case 'Q': case 'q':
        case 'J': case 'j': case 'T': case 't':
            return true;
        default:
            return c >= '2' && c <= '9';
    }
}

bool is_suit_char(char c) { return c == 'c' || c == 'd' || c == 'h' || c == 's'; }

uint8_t rank_of(char c) {
    switch (c) {
        case 'A': case 'a': return 12;
        case 'K': case 'k': return 11;
        case 'Q': case 'q': return 10;
        case 'J': case 'j': return 9;
        case 'T': case 't': return 8;
        default: return static_cast<uint8_t>(c - '2');
    }
}

uint8_t suit_of(char c) {
    switch (c) {
        case 'c': return 0;
        case 'd': return 1;
        case 'h': return 2;
        default: return 3;  // 's'
    }
}

char rank_char_of(uint8_t rank) {
    switch (rank) {
        case 12: return 'A';
        case 11: return 'K';
        case 10: return 'Q';
        case 9: return 'J';
        case 8: return 'T';
        default: return static_cast<char>(rank + '2');
    }
}

char suit_char_of(uint8_t suit) {
    switch (suit) {
        case 0: return 'c';
        case 1: return 'd';
        case 2: return 'h';
        default: return 's';
    }
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ---------------------------------------------------------------------------
// Suitedness
// ---------------------------------------------------------------------------

enum class SuitKind : uint8_t { Suited, Offsuit, All, Specific };

struct Suitedness {
    SuitKind kind = SuitKind::All;
    uint8_t suit1 = 0;
    uint8_t suit2 = 0;

    friend bool operator==(const Suitedness& a, const Suitedness& b) {
        if (a.kind != b.kind) return false;
        if (a.kind != SuitKind::Specific) return true;
        return a.suit1 == b.suit1 && a.suit2 == b.suit2;
    }
};

// ---------------------------------------------------------------------------
// Index sets. Rust returns Vec<usize>; a fixed 16-slot buffer avoids the
// allocation without changing behaviour (the largest set is nonpair, 16).
// ---------------------------------------------------------------------------

struct IndexSet {
    std::array<size_t, 16> idx{};
    size_t n = 0;
    void push(size_t v) { idx[n++] = v; }
    const size_t* begin() const { return idx.data(); }
    const size_t* end() const { return idx.data() + n; }
    size_t size() const { return n; }
    size_t operator[](size_t i) const { return idx[i]; }
};

IndexSet pair_indices(uint8_t rank) {
    IndexSet s;
    for (Card i = 0; i < 4; ++i)
        for (Card j = static_cast<Card>(i + 1); j < 4; ++j)
            s.push(card_pair_to_index(static_cast<Card>(4 * rank + i),
                                      static_cast<Card>(4 * rank + j)));
    return s;
}

IndexSet nonpair_indices(uint8_t rank1, uint8_t rank2) {
    IndexSet s;
    for (Card i = 0; i < 4; ++i)
        for (Card j = 0; j < 4; ++j)
            s.push(card_pair_to_index(static_cast<Card>(4 * rank1 + i),
                                      static_cast<Card>(4 * rank2 + j)));
    return s;
}

IndexSet suited_indices(uint8_t rank1, uint8_t rank2) {
    IndexSet s;
    for (Card i = 0; i < 4; ++i)
        s.push(card_pair_to_index(static_cast<Card>(4 * rank1 + i),
                                  static_cast<Card>(4 * rank2 + i)));
    return s;
}

IndexSet offsuit_indices(uint8_t rank1, uint8_t rank2) {
    IndexSet s;
    for (Card i = 0; i < 4; ++i)
        for (Card j = 0; j < 4; ++j)
            if (i != j)
                s.push(card_pair_to_index(static_cast<Card>(4 * rank1 + i),
                                          static_cast<Card>(4 * rank2 + j)));
    return s;
}

Result<IndexSet> indices_with_suitedness(uint8_t rank1, uint8_t rank2, Suitedness s) {
    if (rank1 == rank2) {
        if (s.kind == SuitKind::All) return pair_indices(rank1);
        if (s.kind == SuitKind::Specific) {
            IndexSet out;
            out.push(card_pair_to_index(static_cast<Card>(4 * rank1 + s.suit1),
                                        static_cast<Card>(4 * rank1 + s.suit2)));
            return out;
        }
        // Rust panics here; the parser rejects this case first.
        return Result<IndexSet>::err("invalid suitedness with a pair");
    }
    switch (s.kind) {
        case SuitKind::Suited: return suited_indices(rank1, rank2);
        case SuitKind::Offsuit: return offsuit_indices(rank1, rank2);
        case SuitKind::All: return nonpair_indices(rank1, rank2);
        default: {
            IndexSet out;
            out.push(card_pair_to_index(static_cast<Card>(4 * rank1 + s.suit1),
                                        static_cast<Card>(4 * rank2 + s.suit2)));
            return out;
        }
    }
}

// ---------------------------------------------------------------------------
// Singleton parsing
// ---------------------------------------------------------------------------

struct Singleton {
    uint8_t rank1 = 0;
    uint8_t rank2 = 0;
    Suitedness suitedness;
};

Result<Singleton> parse_simple_singleton(const std::string& combo) {
    // 4 chars: rank suit rank suit
    if (combo.size() != 4) return Result<Singleton>::err("Unexpected end");
    if (!is_rank_char(combo[0]) || !is_suit_char(combo[1]) || !is_rank_char(combo[2]) ||
        !is_suit_char(combo[3]))
        return Result<Singleton>::err("Failed to parse range: " + combo);

    Singleton out;
    out.rank1 = rank_of(combo[0]);
    const uint8_t suit1 = suit_of(combo[1]);
    out.rank2 = rank_of(combo[2]);
    const uint8_t suit2 = suit_of(combo[3]);

    if (out.rank1 < out.rank2)
        return Result<Singleton>::err(
            "The first rank must be equal or higher than the second rank: " + combo);
    if (out.rank1 == out.rank2 && suit1 == suit2)
        return Result<Singleton>::err("Duplicate cards are not allowed: " + combo);

    out.suitedness = Suitedness{SuitKind::Specific, suit1, suit2};
    return out;
}

Result<Singleton> parse_compound_singleton(const std::string& combo) {
    if (combo.size() < 2) return Result<Singleton>::err("Unexpected end");
    if (!is_rank_char(combo[0]) || !is_rank_char(combo[1]))
        return Result<Singleton>::err("Failed to parse range: " + combo);

    Singleton out;
    out.rank1 = rank_of(combo[0]);
    out.rank2 = rank_of(combo[1]);

    if (combo.size() == 2) {
        out.suitedness = Suitedness{SuitKind::All, 0, 0};
    } else if (combo.size() == 3 && combo[2] == 's') {
        out.suitedness = Suitedness{SuitKind::Suited, 0, 0};
    } else if (combo.size() == 3 && combo[2] == 'o') {
        out.suitedness = Suitedness{SuitKind::Offsuit, 0, 0};
    } else {
        return Result<Singleton>::err("Invalid suitedness: " + combo);
    }

    if (out.rank1 < out.rank2)
        return Result<Singleton>::err(
            "The first rank must be equal or higher than the second rank: " + combo);
    if (out.rank1 == out.rank2 && out.suitedness.kind != SuitKind::All)
        return Result<Singleton>::err("A pair with suitedness is not allowed: " + combo);

    return out;
}

Result<Singleton> parse_singleton(const std::string& combo) {
    if (combo.size() == 4) return parse_simple_singleton(combo);
    return parse_compound_singleton(combo);
}

bool check_weight(float w) { return w >= 0.0f && w <= 1.0f; }

// ---------------------------------------------------------------------------
// The scanner that replaces RANGE_REGEX:
//   ^ COMBO ( '+' | '-' COMBO )? ( ':' WEIGHT )? $
//   COMBO  = [rank]{2}[os]?  |  ([rank][suit]){2}
//   WEIGHT = [01](\.[0-9]*)? |  \.[0-9]+
//
// Rank and suit character sets are disjoint, so the two COMBO alternatives can
// never both match at a position -- no ambiguity to resolve.
// ---------------------------------------------------------------------------

// Candidate lengths at `pos`, in regex preference order (greedy [os]? first).
size_t combo_candidates(const std::string& s, size_t pos, std::array<size_t, 2>& out) {
    size_t n = 0;
    const size_t left = s.size() - pos;
    if (left >= 2 && is_rank_char(s[pos]) && is_rank_char(s[pos + 1])) {
        if (left >= 3 && (s[pos + 2] == 'o' || s[pos + 2] == 's')) out[n++] = 3;
        out[n++] = 2;
        return n;
    }
    if (left >= 4 && is_rank_char(s[pos]) && is_suit_char(s[pos + 1]) &&
        is_rank_char(s[pos + 2]) && is_suit_char(s[pos + 3])) {
        out[n++] = 4;
    }
    return n;
}

// Matches WEIGHT against exactly s[pos..end).
bool match_weight(const std::string& s, size_t pos, float& weight) {
    const size_t len = s.size() - pos;
    if (len == 0) return false;
    size_t i = pos;
    if (s[i] == '0' || s[i] == '1') {
        ++i;
        if (i < s.size()) {
            if (s[i] != '.') return false;
            ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
    } else if (s[i] == '.') {
        ++i;
        if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    } else {
        return false;
    }
    if (i != s.size()) return false;

    const std::string num = s.substr(pos, s.size() - pos);
    weight = std::strtof(num.c_str(), nullptr);
    return true;
}

struct Group {
    std::string range;  // the combo part, without the :weight suffix
    float weight = 1.0f;
};

// Returns the group, or an empty optional if the whole pattern does not match.
bool match_group(const std::string& s, Group& out) {
    std::array<size_t, 2> c1{};
    const size_t n1 = combo_candidates(s, 0, c1);

    for (size_t a = 0; a < n1; ++a) {
        const size_t after1 = c1[a];

        // Helper: given the end of the range part, try the optional weight and
        // require the string to be fully consumed.
        auto finish = [&](size_t range_end) -> bool {
            if (range_end == s.size()) {
                out.range = s.substr(0, range_end);
                out.weight = 1.0f;
                return true;
            }
            if (s[range_end] == ':') {
                float w = 1.0f;
                if (!match_weight(s, range_end + 1, w)) return false;
                out.range = s.substr(0, range_end);
                out.weight = w;
                return true;
            }
            return false;
        };

        // '+' branch
        if (after1 < s.size() && s[after1] == '+' && finish(after1 + 1)) return true;

        // '-' COMBO branch
        if (after1 < s.size() && s[after1] == '-') {
            std::array<size_t, 2> c2{};
            const size_t n2 = combo_candidates(s, after1 + 1, c2);
            for (size_t b = 0; b < n2; ++b)
                if (finish(after1 + 1 + c2[b])) return true;
        }

        // bare combo
        if (finish(after1)) return true;
    }
    return false;
}

// Replacement for TRIM_REGEX: `\s*([-:,])\s*` -> `$1`, then trim the ends.
// Whitespace *inside* a combo is deliberately left alone, so "A s" stays invalid.
std::string trim_range_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (is_space(s[i])) {
            size_t j = i;
            while (j < s.size() && is_space(s[j])) ++j;
            if (j < s.size() && (s[j] == '-' || s[j] == ':' || s[j] == ',')) {
                out.push_back(s[j]);
                ++j;
                while (j < s.size() && is_space(s[j])) ++j;
                i = j;
                continue;
            }
            for (size_t k = i; k < j; ++k) out.push_back(s[k]);
            i = j;
            continue;
        }
        if (s[i] == '-' || s[i] == ':' || s[i] == ',') {
            out.push_back(s[i]);
            ++i;
            while (i < s.size() && is_space(s[i])) ++i;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    // .trim()
    size_t b = 0, e = out.size();
    while (b < e && is_space(out[b])) ++b;
    while (e > b && is_space(out[e - 1])) --e;
    return out.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

}  // namespace

// ---------------------------------------------------------------------------
// Range
// ---------------------------------------------------------------------------

Range Range::ones() {
    Range r;
    r.data_.fill(1.0f);
    return r;
}

Result<Range> Range::from_raw_data(std::span<const float> data) {
    if (data.size() != kNumHandIndices)
        return Result<Range>::err("Expected exactly " + std::to_string(kNumHandIndices) +
                                  " elements");
    for (float w : data)
        if (!check_weight(w))
            return Result<Range>::err("Invalid weight: " + f32_to_shortest(w));
    Range r;
    std::copy(data.begin(), data.end(), r.data_.begin());
    return r;
}

Result<Range> Range::from_hands_weights(const std::vector<Hole>& hands,
                                        std::span<const float> weights) {
    Range r;
    const size_t n = std::min(hands.size(), weights.size());
    for (size_t i = 0; i < n; ++i) {
        const Card c1 = hands[i].first;
        const Card c2 = hands[i].second;
        const float w = weights[i];
        if (c1 >= 52) return Result<Range>::err("Invalid card: " + std::to_string(c1));
        if (c2 >= 52) return Result<Range>::err("Invalid card: " + std::to_string(c2));
        if (!check_weight(w)) return Result<Range>::err("Invalid weight: " + f32_to_shortest(w));
        if (c1 == c2) return Result<Range>::err("Hand must consist of two different cards");
        r.set_weight_by_cards(c1, c2, w);
    }
    return r;
}

bool Range::is_empty() const noexcept {
    for (float w : data_)
        if (w != 0.0f) return false;
    return true;
}

void Range::invert() {
    // Round-trip through the shortest decimal so that 0.9 inverts to exactly
    // 0.1 rather than 0.100000024.
    for (float& el : data_)
        el = static_cast<float>(1.0 - std::strtod(f32_to_shortest(el).c_str(), nullptr));
}

float Range::get_weight_pair(uint8_t rank) const {
    const IndexSet s = pair_indices(rank);
    double sum = 0.0;
    for (size_t i : s) sum += static_cast<double>(data_[i]);
    return static_cast<float>(sum / static_cast<double>(s.size()));
}

float Range::get_weight_suited(uint8_t rank1, uint8_t rank2) const {
    const IndexSet s = suited_indices(rank1, rank2);
    double sum = 0.0;
    for (size_t i : s) sum += static_cast<double>(data_[i]);
    return static_cast<float>(sum / static_cast<double>(s.size()));
}

float Range::get_weight_offsuit(uint8_t rank1, uint8_t rank2) const {
    const IndexSet s = offsuit_indices(rank1, rank2);
    double sum = 0.0;
    for (size_t i : s) sum += static_cast<double>(data_[i]);
    return static_cast<float>(sum / static_cast<double>(s.size()));
}

void Range::set_weight_pair(uint8_t rank, float weight) {
    for (size_t i : pair_indices(rank)) data_[i] = weight;
}
void Range::set_weight_suited(uint8_t rank1, uint8_t rank2, float weight) {
    for (size_t i : suited_indices(rank1, rank2)) data_[i] = weight;
}
void Range::set_weight_offsuit(uint8_t rank1, uint8_t rank2, float weight) {
    for (size_t i : offsuit_indices(rank1, rank2)) data_[i] = weight;
}

void Range::get_hands_weights(uint64_t dead_cards_mask, std::vector<Hole>& hands,
                              std::vector<float>& weights) const {
    hands.clear();
    weights.clear();
    for (Card card1 = 0; card1 < 52; ++card1) {
        for (Card card2 = static_cast<Card>(card1 + 1); card2 < 52; ++card2) {
            const uint64_t hand_mask = (uint64_t{1} << card1) | (uint64_t{1} << card2);
            const float weight = get_weight_by_cards(card1, card2);
            if (weight > 0.0f && (hand_mask & dead_cards_mask) == 0) {
                hands.emplace_back(card1, card2);
                weights.push_back(weight);
            }
        }
    }
}

bool Range::is_valid() const noexcept {
    for (float w : data_)
        if (!(w >= 0.0f && w <= 1.0f)) return false;
    return true;
}

namespace {
bool is_same_weight(const std::array<float, kNumHandIndices>& d, const IndexSet& s) {
    const float w = d[s[0]];
    for (size_t i : s)
        if (d[i] != w) return false;
    return true;
}
}  // namespace

bool Range::is_suit_symmetric() const {
    for (uint8_t rank1 = 0; rank1 < 13; ++rank1) {
        if (!is_same_weight(data_, pair_indices(rank1))) return false;
        for (uint8_t rank2 = static_cast<uint8_t>(rank1 + 1); rank2 < 13; ++rank2) {
            if (!is_same_weight(data_, suited_indices(rank1, rank2))) return false;
            if (!is_same_weight(data_, offsuit_indices(rank1, rank2))) return false;
        }
    }
    return true;
}

bool Range::is_suit_isomorphic(uint8_t suit1, uint8_t suit2) const {
    auto replace = [suit1, suit2](uint8_t suit) -> uint8_t {
        if (suit == suit1) return suit2;
        if (suit == suit2) return suit1;
        return suit;
    };
    for (Card card1 = 0; card1 < 52; ++card1) {
        for (Card card2 = static_cast<Card>(card1 + 1); card2 < 52; ++card2) {
            const Card c1r =
                static_cast<Card>((card1 & ~3) | replace(static_cast<uint8_t>(card1 & 3)));
            const Card c2r =
                static_cast<Card>((card2 & ~3) | replace(static_cast<uint8_t>(card2 & 3)));
            if (get_weight_by_cards(card1, card2) != get_weight_by_cards(c1r, c2r)) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

Status set_with_suitedness(Range& r, uint8_t rank1, uint8_t rank2, Suitedness s, float weight) {
    Result<IndexSet> idx = indices_with_suitedness(rank1, rank2, s);
    if (!idx) return Status::err(idx.error());
    for (size_t i : idx.value()) r.set_weight_at(i, weight);
    return Status::ok();
}

Status update_with_singleton(Range& r, const std::string& combo, float weight) {
    Result<Singleton> s = parse_singleton(combo);
    if (!s) return Status::err(s.error());
    return set_with_suitedness(r, s.value().rank1, s.value().rank2, s.value().suitedness, weight);
}

Status update_with_plus_range(Range& r, const std::string& range, float weight) {
    const std::string lowest = range.substr(0, range.size() - 1);
    Result<Singleton> s = parse_singleton(lowest);
    if (!s) return Status::err(s.error());
    const uint8_t rank1 = s.value().rank1;
    const uint8_t rank2 = s.value().rank2;
    const uint8_t gap = static_cast<uint8_t>(rank1 - rank2);

    if (gap <= 1) {
        // pair or connector: walk the diagonal up to A (88+, T9s+)
        for (uint8_t i = rank1; i < 13; ++i) {
            Status st = set_with_suitedness(r, i, static_cast<uint8_t>(i - gap),
                                            s.value().suitedness, weight);
            if (!st) return st;
        }
    } else {
        // fixed first rank, sweep the second up (ATo+)
        for (uint8_t i = rank2; i < rank1; ++i) {
            Status st = set_with_suitedness(r, rank1, i, s.value().suitedness, weight);
            if (!st) return st;
        }
    }
    return Status::ok();
}

Status update_with_dash_range(Range& r, const std::string& range, float weight) {
    const std::vector<std::string> parts = split(range, '-');
    if (parts.size() != 2) return Status::err("Invalid range: " + range);

    Result<Singleton> a = parse_singleton(parts[0]);
    if (!a) return Status::err(a.error());
    Result<Singleton> b = parse_singleton(parts[1]);
    if (!b) return Status::err(b.error());

    const uint8_t rank11 = a.value().rank1, rank12 = a.value().rank2;
    const uint8_t rank21 = b.value().rank1, rank22 = b.value().rank2;
    const uint8_t gap = static_cast<uint8_t>(rank11 - rank12);
    const uint8_t gap2 = static_cast<uint8_t>(rank21 - rank22);

    if (!(a.value().suitedness == b.value().suitedness))
        return Status::err("Suitedness does not match: " + range);

    if (gap == gap2) {
        // same gap (88-55, KQo-JTo)
        if (rank11 <= rank21) return Status::err("Range must be in descending order: " + range);
        for (uint8_t i = rank21; i <= rank11; ++i) {
            Status st = set_with_suitedness(r, i, static_cast<uint8_t>(i - gap),
                                            a.value().suitedness, weight);
            if (!st) return st;
        }
        return Status::ok();
    }
    if (rank11 == rank21) {
        // same first rank (A5s-A2s)
        if (rank12 <= rank22) return Status::err("Range must be in descending order: " + range);
        for (uint8_t i = rank22; i <= rank12; ++i) {
            Status st = set_with_suitedness(r, rank11, i, a.value().suitedness, weight);
            if (!st) return st;
        }
        return Status::ok();
    }
    return Status::err("Invalid range: " + range);
}

Status apply_group(Range& r, const std::string& range, float weight) {
    if (range.find('-') != std::string::npos) return update_with_dash_range(r, range, weight);
    if (range.find('+') != std::string::npos) return update_with_plus_range(r, range, weight);
    return update_with_singleton(r, range, weight);
}

}  // namespace

Result<Range> Range::parse(const std::string& input) {
    const std::string s = trim_range_string(input);
    std::vector<std::string> groups = split(s, ',');

    // A single trailing comma is allowed ("AK," is fine, "AK,," is not).
    if (!groups.empty() && groups.back().empty()) groups.pop_back();

    Range result;
    // Right to left, so an earlier group wins over a later overlapping one.
    for (size_t i = groups.size(); i-- > 0;) {
        Group g;
        if (!match_group(groups[i], g))
            return Result<Range>::err("Failed to parse range: " + groups[i]);
        if (!check_weight(g.weight))
            return Result<Range>::err("Invalid weight: " + f32_to_shortest(g.weight));
        Status st = apply_group(result, g.range, g.weight);
        if (!st) return Result<Range>::err(st.error());
    }
    return result;
}

Result<Range> Range::from_sanitized_str(const std::string& input) {
    std::vector<std::string> groups = split(input, ',');
    if (!groups.empty() && groups.back().empty()) groups.pop_back();

    Range result;
    for (size_t i = groups.size(); i-- > 0;) {
        const std::vector<std::string> parts = split(groups[i], ':');
        if (parts.size() > 2) return Result<Range>::err("Invalid range: " + parts[0]);

        float weight = 1.0f;
        if (parts.size() == 2) weight = std::strtof(parts[1].c_str(), nullptr);
        if (!check_weight(weight))
            return Result<Range>::err("Invalid weight: " + f32_to_shortest(weight));

        Status st = apply_group(result, parts[0], weight);
        if (!st) return Result<Range>::err(st.error());
    }
    return result;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

namespace {

void append_weight(std::string& s, float weight) {
    if (weight != 1.0f) {
        s.push_back(':');
        s += f32_to_shortest(weight);
    }
}

double average_weight(const Range& r, const IndexSet& s) {
    double sum = 0.0;
    for (size_t i : s) sum += static_cast<double>(r.get_weight_at(i));
    return sum / static_cast<double>(s.size());
}

float average_weight_f(const Range& r, const IndexSet& s) {
    return static_cast<float>(average_weight(r, s));
}

using IndexGetter = IndexSet (*)(uint8_t, uint8_t);

}  // namespace

std::string Range::to_string() const {
    std::vector<std::string> out;

    auto same_weight = [this](const IndexSet& s) { return is_same_weight(data_, s); };

    // --- pairs -------------------------------------------------------------
    {
        bool have_start = false;
        uint8_t start_rank = 0;
        float start_weight = 0.0f;

        // Rust iterates (-1..13).rev(), i.e. 12 down to -1, using the -1 pass to
        // flush a run in progress.
        for (int i = 12; i >= -1; --i) {
            const uint8_t rank = static_cast<uint8_t>(i);
            const uint8_t prev_rank = static_cast<uint8_t>(i + 1);

            if (have_start && (i == -1 || !same_weight(pair_indices(rank)) ||
                               start_weight != get_weight_pair(rank))) {
                const char s = rank_char_of(start_rank);
                const char e = rank_char_of(prev_rank);
                std::string tmp;
                if (start_rank == prev_rank) {
                    tmp = std::string{s, s};
                } else if (start_rank == 12) {
                    tmp = std::string{e, e, '+'};
                } else {
                    tmp = std::string{s, s, '-', e, e};
                }
                append_weight(tmp, start_weight);
                out.push_back(tmp);
                have_start = false;
            }

            if (i >= 0 && same_weight(pair_indices(rank)) && get_weight_pair(rank) > 0.0f &&
                !have_start) {
                have_start = true;
                start_rank = rank;
                start_weight = get_weight_pair(rank);
            }
        }
    }

    // --- non-pairs ---------------------------------------------------------
    auto high_cards = [&](uint8_t rank1, SuitKind kind) {
        const char rank1_char = rank_char_of(rank1);
        IndexGetter getter = nullptr;
        const char* suit_char = "";
        switch (kind) {
            case SuitKind::Suited: getter = &suited_indices; suit_char = "s"; break;
            case SuitKind::Offsuit: getter = &offsuit_indices; suit_char = "o"; break;
            default: getter = &nonpair_indices; suit_char = ""; break;
        }

        bool have_start = false;
        uint8_t start_rank2 = 0;
        float start_weight = 0.0f;

        for (int i = static_cast<int>(rank1) - 1; i >= -1; --i) {
            const uint8_t rank2 = static_cast<uint8_t>(i);
            const uint8_t prev_rank2 = static_cast<uint8_t>(i + 1);

            if (have_start &&
                (i == -1 || !same_weight(getter(rank1, rank2)) ||
                 start_weight != average_weight_f(*this, getter(rank1, rank2)))) {
                const char s = rank_char_of(start_rank2);
                const char e = rank_char_of(prev_rank2);
                std::string tmp;
                if (start_rank2 == prev_rank2) {
                    tmp = std::string{rank1_char, s} + suit_char;
                } else if (start_rank2 == rank1 - 1) {
                    tmp = std::string{rank1_char, e} + suit_char + "+";
                } else {
                    tmp = std::string{rank1_char, s} + suit_char + "-" +
                          std::string{rank1_char, e} + suit_char;
                }
                append_weight(tmp, start_weight);
                out.push_back(tmp);
                have_start = false;
            }

            if (i >= 0 && same_weight(getter(rank1, rank2)) &&
                average_weight_f(*this, getter(rank1, rank2)) > 0.0f && !have_start) {
                have_start = true;
                start_rank2 = rank2;
                start_weight = average_weight_f(*this, getter(rank1, rank2));
            }
        }
    };

    auto can_unsuit = [&](uint8_t rank1) {
        for (uint8_t rank2 = 0; rank2 < rank1; ++rank2) {
            const bool same_suited = same_weight(suited_indices(rank1, rank2));
            const bool same_offsuit = same_weight(offsuit_indices(rank1, rank2));
            const float ws = get_weight_suited(rank1, rank2);
            const float wo = get_weight_offsuit(rank1, rank2);
            if ((same_suited && same_offsuit && ws != wo) ||
                (same_suited != same_offsuit && ws > 0.0f && wo > 0.0f))
                return false;
        }
        return true;
    };

    for (int r1 = 12; r1 >= 1; --r1) {
        const uint8_t rank1 = static_cast<uint8_t>(r1);
        if (can_unsuit(rank1)) {
            high_cards(rank1, SuitKind::All);
        } else {
            high_cards(rank1, SuitKind::Suited);
            high_cards(rank1, SuitKind::Offsuit);
        }
    }

    // --- explicit-suit fallbacks ------------------------------------------
    for (int r = 12; r >= 0; --r) {
        const uint8_t rank = static_cast<uint8_t>(r);
        if (same_weight(pair_indices(rank))) continue;
        for (int s1 = 3; s1 >= 0; --s1) {
            for (int s2 = s1 - 1; s2 >= 0; --s2) {
                const float w = get_weight_by_cards(static_cast<Card>(4 * rank + s1),
                                                    static_cast<Card>(4 * rank + s2));
                if (w <= 0.0f) continue;
                std::string tmp{rank_char_of(rank), suit_char_of(static_cast<uint8_t>(s1)),
                                rank_char_of(rank), suit_char_of(static_cast<uint8_t>(s2))};
                append_weight(tmp, w);
                out.push_back(tmp);
            }
        }
    }

    for (int r1 = 12; r1 >= 0; --r1) {
        const uint8_t rank1 = static_cast<uint8_t>(r1);
        for (int r2 = r1 - 1; r2 >= 0; --r2) {
            const uint8_t rank2 = static_cast<uint8_t>(r2);

            if (!same_weight(suited_indices(rank1, rank2))) {
                for (int s = 3; s >= 0; --s) {
                    const float w = get_weight_by_cards(static_cast<Card>(4 * rank1 + s),
                                                        static_cast<Card>(4 * rank2 + s));
                    if (w <= 0.0f) continue;
                    std::string tmp{rank_char_of(rank1), suit_char_of(static_cast<uint8_t>(s)),
                                    rank_char_of(rank2), suit_char_of(static_cast<uint8_t>(s))};
                    append_weight(tmp, w);
                    out.push_back(tmp);
                }
            }

            if (!same_weight(offsuit_indices(rank1, rank2))) {
                for (int s1 = 3; s1 >= 0; --s1) {
                    for (int s2 = 3; s2 >= 0; --s2) {
                        if (s1 == s2) continue;
                        const float w = get_weight_by_cards(static_cast<Card>(4 * rank1 + s1),
                                                            static_cast<Card>(4 * rank2 + s2));
                        if (w <= 0.0f) continue;
                        std::string tmp{
                            rank_char_of(rank1), suit_char_of(static_cast<uint8_t>(s1)),
                            rank_char_of(rank2), suit_char_of(static_cast<uint8_t>(s2))};
                        append_weight(tmp, w);
                        out.push_back(tmp);
                    }
                }
            }
        }
    }

    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) joined.push_back(',');
        joined += out[i];
    }
    return joined;
}

}  // namespace pfs
