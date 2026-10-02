#include "gto_range.hpp"

#include <pfs/card.hpp>

#include <algorithm>
#include <cstdlib>

using pfs::Result;

namespace pfs_solver {

namespace {

constexpr char kRanks[] = "23456789TJQKA";

int parse_rank(char c) {
    const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    for (int rank = 0; rank < 13; ++rank)
        if (kRanks[rank] == upper) return rank;
    return -1;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// A hand class: ranks as in pfs (0 = deuce, 12 = ace), suitedness 's', 'o' or 0
// for both.
struct HandClass {
    int high = -1;
    int low = -1;
    char suitedness = 0;
};

bool parse_class(const std::string& text, HandClass& hand) {
    if (text.size() < 2 || text.size() > 3) return false;
    const int first = parse_rank(text[0]);
    const int second = parse_rank(text[1]);
    if (first < 0 || second < 0) return false;
    hand.high = std::max(first, second);
    hand.low = std::min(first, second);
    hand.suitedness = 0;
    if (text.size() == 3) {
        if (text[2] != 's' && text[2] != 'o') return false;
        hand.suitedness = text[2];
    }
    return hand.high != hand.low || hand.suitedness == 0;
}

void set_class(pfs::Range& range, int high, int low, char suitedness, float weight) {
    const auto h = static_cast<uint8_t>(high);
    const auto l = static_cast<uint8_t>(low);
    if (high == low) {
        range.set_weight_pair(h, weight);
        return;
    }
    if (suitedness != 'o') range.set_weight_suited(h, l, weight);
    if (suitedness != 's') range.set_weight_offsuit(h, l, weight);
}

pfs::Status apply_element(pfs::Range& range, const std::string& element, float weight) {
    // A single combo, e.g. "AhKh".
    if (element.size() == 4 && parse_rank(element[1]) < 0) {
        Result<pfs::Card> card1 = pfs::card_from_str(element.substr(0, 2));
        Result<pfs::Card> card2 = pfs::card_from_str(element.substr(2, 2));
        if (!card1 || !card2 || card1.value() == card2.value())
            return pfs::Status::err("Invalid combo: " + element);
        range.set_weight_by_cards(card1.value(), card2.value(), weight);
        return pfs::Status::ok();
    }

    HandClass from;
    HandClass to;
    const size_t dash = element.find('-');
    if (dash != std::string::npos) {
        if (!parse_class(element.substr(0, dash), from) || !parse_class(element.substr(dash + 1), to))
            return pfs::Status::err("Invalid range element: " + element);
    } else if (!element.empty() && element.back() == '+') {
        if (!parse_class(element.substr(0, element.size() - 1), from))
            return pfs::Status::err("Invalid range element: " + element);
        to = from;
        if (from.high == from.low)
            to.high = to.low = 12;
        else
            to.low = from.high - 1;
    } else {
        if (!parse_class(element, from)) return pfs::Status::err("Invalid range element: " + element);
        to = from;
    }

    if (from.high == from.low) {
        if (to.high != to.low) return pfs::Status::err("Invalid range element: " + element);
        for (int rank = std::min(from.high, to.high); rank <= std::max(from.high, to.high); ++rank)
            set_class(range, rank, rank, 0, weight);
        return pfs::Status::ok();
    }
    if (from.high != to.high || from.suitedness != to.suitedness)
        return pfs::Status::err("Invalid range element: " + element);
    for (int low = std::min(from.low, to.low); low <= std::max(from.low, to.low); ++low)
        set_class(range, from.high, low, from.suitedness, weight);
    return pfs::Status::ok();
}

Result<float> parse_weight(const std::string& text) {
    char* end = nullptr;
    const double percent = std::strtod(text.c_str(), &end);
    if (text.empty() || *end != '\0' || percent < 0.0 || percent > 100.0)
        return Result<float>::err("Invalid weight: " + text);
    return static_cast<float>(percent / 100.0);
}

}  // namespace

Result<pfs::Range> parse_gto_range(const std::string& text) {
    pfs::Range range;
    float group_weight = 1.0f;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        std::string element = trim(text.substr(start, end - start));
        start = end + 1;

        if (element.size() > 1 && element[0] == '[' && element[1] != '/') {
            const size_t close = element.find(']');
            if (close == std::string::npos)
                return Result<pfs::Range>::err("Invalid weight in " + element);
            Result<float> weight = parse_weight(element.substr(1, close - 1));
            if (!weight) return Result<pfs::Range>::err(weight.error());
            group_weight = weight.value();
            element = element.substr(close + 1);
        }

        const size_t close_tag = element.find("[/");
        const bool ends_group = close_tag != std::string::npos;
        if (ends_group) element = element.substr(0, close_tag);

        if (!element.empty()) {
            pfs::Status status = apply_element(range, element, group_weight);
            if (!status) return Result<pfs::Range>::err(status.error());
        }
        if (ends_group) group_weight = 1.0f;
    }
    return range;
}

}  // namespace pfs_solver
