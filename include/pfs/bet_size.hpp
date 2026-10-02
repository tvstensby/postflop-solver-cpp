// Port of src/bet_size.rs.
#pragma once

#include <pfs/common.hpp>
#include <pfs/result.hpp>

#include <limits>
#include <string>
#include <vector>

namespace pfs {

// Accepted bet-size strings. Each element must end in one of % x c r e a:
//
//   %      percentage of the pot            "70%"
//   x      multiple of the previous bet, raises only    "2.5x"
//   c      constant, must be an integer     "100c"
//   c + r  constant with a raise cap (FLHE), raises only "20c3r"
//   e      geometric size
//            "e"     same as 3e on the flop, 2e on the turn, 1e on the river
//            "Xe"    X streets remaining
//            "XeY%"  as Xe, capped at Y% of the pot
//   a      all-in                           "a"
//
// Input is lowercased first, so "3.5X", "123C", "100C100R", "E37.5%" and "A" all
// work. Suit-style characters are irrelevant here.
enum class BetSizeKind : uint8_t {
    // Declaration order is load-bearing: Rust derives PartialOrd, and the parsers
    // sort by it, which fixes the order actions are pushed into the tree.
    PotRelative = 0,
    PrevBetRelative = 1,
    Additive = 2,
    Geometric = 3,
    AllIn = 4,
};

struct BetSize {
    BetSizeKind kind = BetSizeKind::PotRelative;
    // PotRelative / PrevBetRelative: ratio. Geometric: max pot-relative size
    // (infinity when uncapped).
    double ratio = 0.0;
    // Additive: (amount, raise_cap) where cap 0 means uncapped.
    // Geometric: num_streets in `amount` (0 means flop 3 / turn 2 / river 1).
    int32_t amount = 0;
    int32_t cap = 0;

    static BetSize pot_relative(double r) { return {BetSizeKind::PotRelative, r, 0, 0}; }
    static BetSize prev_bet_relative(double r) { return {BetSizeKind::PrevBetRelative, r, 0, 0}; }
    static BetSize additive(int32_t add, int32_t cap) {
        return {BetSizeKind::Additive, 0.0, add, cap};
    }
    static BetSize geometric(int32_t streets, double max_ratio) {
        return {BetSizeKind::Geometric, max_ratio, streets, 0};
    }
    static BetSize all_in() { return {BetSizeKind::AllIn, 0.0, 0, 0}; }

    friend bool operator==(const BetSize& a, const BetSize& b) noexcept {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case BetSizeKind::PotRelative:
            case BetSizeKind::PrevBetRelative:
                return a.ratio == b.ratio;
            case BetSizeKind::Additive:
                return a.amount == b.amount && a.cap == b.cap;
            case BetSizeKind::Geometric:
                return a.amount == b.amount && a.ratio == b.ratio;
            case BetSizeKind::AllIn:
                return true;
        }
        return false;
    }

    // Rust's derived PartialOrd: variant declaration order first, then the
    // payload in declaration order. The parsers sort with
    // `partial_cmp().unwrap()`, so NaN payloads would panic -- the parser
    // rejects them, so this can stay a strict weak ordering.
    friend bool operator<(const BetSize& a, const BetSize& b) noexcept {
        if (a.kind != b.kind) return a.kind < b.kind;
        switch (a.kind) {
            case BetSizeKind::PotRelative:
            case BetSizeKind::PrevBetRelative:
                return a.ratio < b.ratio;
            case BetSizeKind::Additive:
                if (a.amount != b.amount) return a.amount < b.amount;
                return a.cap < b.cap;
            case BetSizeKind::Geometric:
                if (a.amount != b.amount) return a.amount < b.amount;
                return a.ratio < b.ratio;
            case BetSizeKind::AllIn:
                return false;
        }
        return false;
    }
};

inline constexpr double kNoMaxRatio = std::numeric_limits<double>::infinity();

// Bet size options for first bets and raises.
struct BetSizeOptions {
    std::vector<BetSize> bet;
    std::vector<BetSize> raise;

    // Rust: `BetSizeOptions::try_from((bet_str, raise_str))`.
    static Result<BetSizeOptions> parse(const std::string& bet_str, const std::string& raise_str);

    friend bool operator==(const BetSizeOptions& a, const BetSizeOptions& b) noexcept {
        return a.bet == b.bet && a.raise == b.raise;
    }
};

// Bet size options for donk bets.
struct DonkSizeOptions {
    std::vector<BetSize> donk;

    static Result<DonkSizeOptions> parse(const std::string& donk_str);

    friend bool operator==(const DonkSizeOptions& a, const DonkSizeOptions& b) noexcept {
        return a.donk == b.donk;
    }
};

// Internal, exposed for testing.
Result<BetSize> bet_size_from_str(const std::string& s, bool is_raise);

}  // namespace pfs
