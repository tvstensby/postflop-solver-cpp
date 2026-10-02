// Port of src/range.rs.
#pragma once

#include <pfs/card.hpp>
#include <pfs/common.hpp>
#include <pfs/result.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace pfs {

inline constexpr size_t kNumHandIndices = 52 * 51 / 2;  // 1326

// A player's range: one weight per hole-card combination, indexed by
// card_pair_to_index. 5304 bytes, and `Copy` in Rust -- pass by const& here.
//
// Accepted string format (PioSOLVER-like), parsed by a hand-written scanner
// rather than std::regex (which has no named groups and is slow):
//
//   - comma-separated groups, one optional trailing comma allowed
//   - each group may carry an optional `:weight` in [0, 1]
//   - group forms: singleton (AA, AKs, AKo, AsAh), plus (TT+, ATs+, T9o+),
//     dash (QQ-88, A9s-A6s, 98o-65o)
//   - ranks are case-insensitive; suit characters are lowercase only
//   - whitespace around `-`, `:` and `,` is stripped before parsing
//
// Groups are applied RIGHT TO LEFT and set_weight overwrites, so where two
// groups overlap the earlier one in the string wins.
class Range {
public:
    Range() = default;

    static Range ones();
    static Result<Range> from_raw_data(std::span<const float> data);
    static Result<Range> from_hands_weights(const std::vector<Hole>& hands,
                                            std::span<const float> weights);
    static Result<Range> parse(const std::string& s);

    // Bypasses the pattern check; for input already known to be well-formed and
    // whitespace-free.
    static Result<Range> from_sanitized_str(const std::string& s);

    std::span<const float> raw_data() const noexcept { return data_; }

    // Hands with strictly positive weight that do not collide with the dead-card
    // mask, in lexicographic order.
    void get_hands_weights(uint64_t dead_cards_mask, std::vector<Hole>& hands,
                           std::vector<float>& weights) const;

    void clear() noexcept { data_.fill(0.0f); }
    bool is_empty() const noexcept;
    void invert();

    float get_weight_by_cards(Card card1, Card card2) const noexcept {
        return data_[card_pair_to_index(card1, card2)];
    }
    float get_weight_pair(uint8_t rank) const;
    float get_weight_suited(uint8_t rank1, uint8_t rank2) const;
    float get_weight_offsuit(uint8_t rank1, uint8_t rank2) const;

    void set_weight_by_cards(Card card1, Card card2, float weight) noexcept {
        data_[card_pair_to_index(card1, card2)] = weight;
    }
    void set_weight_pair(uint8_t rank, float weight);
    void set_weight_suited(uint8_t rank1, uint8_t rank2, float weight);
    void set_weight_offsuit(uint8_t rank1, uint8_t rank2, float weight);

    std::string to_string() const;

    // Internal: access by raw hand index, used by the parser and formatter.
    void set_weight_at(size_t index, float weight) noexcept { data_[index] = weight; }
    float get_weight_at(size_t index) const noexcept { return data_[index]; }

    // Internal (Rust: pub(crate)).
    bool is_valid() const noexcept;
    bool is_suit_symmetric() const;
    bool is_suit_isomorphic(uint8_t suit1, uint8_t suit2) const;

    friend bool operator==(const Range& a, const Range& b) noexcept {
        return a.data_ == b.data_;
    }

private:
    std::array<float, kNumHandIndices> data_{};
};

static_assert(sizeof(Range) == kNumHandIndices * sizeof(float));

// Shortest decimal representation that round-trips, matching Rust's
// `f32::to_string` / Display. Needed by Range::invert (so 0.9 inverts to exactly
// 0.1, not 0.100000024) and by to_string's `:weight` suffix. printf("%g") does
// NOT produce this.
std::string f32_to_shortest(float v);

}  // namespace pfs
