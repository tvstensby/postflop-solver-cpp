#include <pfs/card.hpp>

#include <cmath>

namespace pfs {

namespace {

// Rust: char_to_rank. Ranks are case-insensitive.
Result<uint8_t> char_to_rank(char c) {
    switch (c) {
        case 'A': case 'a': return uint8_t{12};
        case 'K': case 'k': return uint8_t{11};
        case 'Q': case 'q': return uint8_t{10};
        case 'J': case 'j': return uint8_t{9};
        case 'T': case 't': return uint8_t{8};
        default: break;
    }
    if (c >= '2' && c <= '9') return static_cast<uint8_t>(c - '2');
    return Result<uint8_t>::err(std::string("Expected rank character: ") + c);
}

// Rust: char_to_suit. Suits are lowercase only -- deliberately asymmetric with
// ranks, and the range grammar depends on it.
Result<uint8_t> char_to_suit(char c) {
    switch (c) {
        case 'c': return uint8_t{0};
        case 'd': return uint8_t{1};
        case 'h': return uint8_t{2};
        case 's': return uint8_t{3};
        default: return Result<uint8_t>::err(std::string("Expected suit character: ") + c);
    }
}

Result<char> rank_to_char(uint8_t rank) {
    switch (rank) {
        case 12: return 'A';
        case 11: return 'K';
        case 10: return 'Q';
        case 9: return 'J';
        case 8: return 'T';
        default: break;
    }
    if (rank <= 7) return static_cast<char>(rank + '2');
    return Result<char>::err("Invalid input: " + std::to_string(rank));
}

Result<char> suit_to_char(uint8_t suit) {
    switch (suit) {
        case 0: return 'c';
        case 1: return 'd';
        case 2: return 'h';
        case 3: return 's';
        default: return Result<char>::err("Invalid input: " + std::to_string(suit));
    }
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Rust: card_from_chars -- consumes exactly two chars from the cursor.
Result<Card> card_from_cursor(const std::string& s, size_t& pos) {
    if (pos >= s.size()) return Result<Card>::err("Unexpected end");
    const char rank_char = s[pos++];
    if (pos >= s.size()) return Result<Card>::err("Unexpected end");
    const char suit_char = s[pos++];

    Result<uint8_t> rank = char_to_rank(rank_char);
    if (!rank) return Result<Card>::err(rank.error());
    Result<uint8_t> suit = char_to_suit(suit_char);
    if (!suit) return Result<Card>::err(suit.error());

    return static_cast<Card>((rank.value() << 2) | suit.value());
}

}  // namespace

Hole index_to_card_pair(size_t index) noexcept {
    // Verbatim from Rust:
    //   card1 = (103 - (103.0 * 103.0 - 8.0 * index).sqrt().ceil()) / 2
    //   card2 = index - card1 * (101 - card1) + 1
    // Exact only in double with an IEEE sqrt; see the header note.
    const uint16_t card1 = static_cast<uint16_t>(
        (103 - static_cast<uint16_t>(
                   std::ceil(std::sqrt(103.0 * 103.0 - 8.0 * static_cast<double>(index))))) /
        2);
    const uint16_t card2 =
        static_cast<uint16_t>(index) - static_cast<uint16_t>(card1 * (101 - card1) / 2) + 1;
    return {static_cast<Card>(card1), static_cast<Card>(card2)};
}

Result<std::string> card_to_string(Card card) {
    if (card >= 52) return Result<std::string>::err("Invalid card: " + std::to_string(card));
    Result<char> r = rank_to_char(static_cast<uint8_t>(card >> 2));
    if (!r) return Result<std::string>::err(r.error());
    Result<char> s = suit_to_char(static_cast<uint8_t>(card & 3));
    if (!s) return Result<std::string>::err(s.error());
    return std::string{r.value(), s.value()};
}

Result<std::string> hole_to_string(Hole hole) {
    // Output is sorted descending by card id.
    const Card hi = hole.first > hole.second ? hole.first : hole.second;
    const Card lo = hole.first > hole.second ? hole.second : hole.first;
    Result<std::string> a = card_to_string(hi);
    if (!a) return a;
    Result<std::string> b = card_to_string(lo);
    if (!b) return b;
    return a.value() + b.value();
}

Result<std::vector<std::string>> holes_to_strings(const std::vector<Hole>& holes) {
    // Rust collects into Result<Vec<_>>, which short-circuits on the first Err.
    std::vector<std::string> out;
    out.reserve(holes.size());
    for (const Hole& h : holes) {
        Result<std::string> s = hole_to_string(h);
        if (!s) return Result<std::vector<std::string>>::err(s.error());
        out.push_back(s.value());
    }
    return out;
}

Result<Card> card_from_str(const std::string& s) {
    size_t pos = 0;
    Result<Card> c = card_from_cursor(s, pos);
    if (!c) return c;
    if (pos != s.size()) return Result<Card>::err("Expected exactly two characters");
    return c;
}

Result<std::array<Card, 3>> flop_from_str(const std::string& s) {
    std::array<Card, 3> result{0, 0, 0};
    size_t pos = 0;

    for (int i = 0; i < 3; ++i) {
        if (i > 0)
            while (pos < s.size() && is_space(s[pos])) ++pos;  // Rust: skip_while(is_whitespace)
        Result<Card> c = card_from_cursor(s, pos);
        if (!c) return Result<std::array<Card, 3>>::err(c.error());
        result[static_cast<size_t>(i)] = c.value();
    }

    if (pos != s.size()) return Result<std::array<Card, 3>>::err("Expected exactly three cards");

    // Insertion sort of 3 elements, matching `result.sort_unstable()`.
    for (size_t i = 1; i < 3; ++i)
        for (size_t j = i; j > 0 && result[j - 1] > result[j]; --j) {
            const Card t = result[j - 1];
            result[j - 1] = result[j];
            result[j] = t;
        }

    if (result[0] == result[1] || result[1] == result[2])
        return Result<std::array<Card, 3>>::err("Cards must be unique");

    return result;
}

}  // namespace pfs
