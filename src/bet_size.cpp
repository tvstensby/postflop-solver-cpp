#include <pfs/bet_size.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pfs {

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string trim(const std::string& s) {
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
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

// Rust: parse_float -- rejects any '+', '-' or ASCII letter before parsing, so
// "+42%", "-30%" and "1e2e3" all fail. Note the empty string also fails.
bool parse_float(const std::string& s, double& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c == '+' || c == '-') return false;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return false;
    }
    const char* first = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(first, &end);
    if (end != first + s.size()) return false;
    out = v;
    return true;
}

bool ends_with(const std::string& s, char c) { return !s.empty() && s.back() == c; }

}  // namespace

// The order of these checks matters and is subtle: the '%' test must come AFTER
// the 'e' test so that "3e200%" parses as geometric rather than pot-relative.
Result<BetSize> bet_size_from_str(const std::string& s, bool is_raise) {
    const std::string low = to_lower(s);
    const std::string err_msg = "Invalid bet size: " + s;

    // --- previous-bet relative: "2.5x" -----------------------------------
    if (ends_with(low, 'x')) {
        if (!is_raise)
            return Result<BetSize>::err("Relative size to the previous bet is not allowed: " + s);
        double f = 0.0;
        if (!parse_float(low.substr(0, low.size() - 1), f))
            return Result<BetSize>::err(err_msg);
        if (f <= 1.0) return Result<BetSize>::err("Multiplier must be greater than 1.0: " + s);
        return BetSize::prev_bet_relative(f);
    }

    // --- additive: "100c", "20c3r" ---------------------------------------
    if (low.find('c') != std::string::npos) {
        const std::vector<std::string> parts = split(low, 'c');
        if (parts.size() < 2) return Result<BetSize>::err(err_msg);
        if (parts.size() > 2) return Result<BetSize>::err(err_msg);  // a third 'c'

        double add = 0.0;
        if (!parse_float(parts[0], add)) return Result<BetSize>::err(err_msg);
        if (std::trunc(add) != add)
            return Result<BetSize>::err("Additional size must be an integer: " + s);
        if (add > static_cast<double>(INT32_MAX))
            return Result<BetSize>::err("Additional size must be less than 2^31: " + s);

        int32_t cap = 0;
        if (!parts[1].empty()) {
            if (!is_raise) return Result<BetSize>::err("Raise cap is not allowed: " + s);
            if (!ends_with(parts[1], 'r')) return Result<BetSize>::err(err_msg);
            double f = 0.0;
            if (!parse_float(parts[1].substr(0, parts[1].size() - 1), f))
                return Result<BetSize>::err(err_msg);
            if (std::trunc(f) != f || f == 0.0)
                return Result<BetSize>::err("Raise cap must be a positive integer: " + s);
            if (f > 100.0)
                return Result<BetSize>::err("Raise cap must be less than or equal to 100: " + s);
            cap = static_cast<int32_t>(f);
        }
        return BetSize::additive(static_cast<int32_t>(add), cap);
    }

    // --- geometric: "e", "2e", "3e200%" ----------------------------------
    if (low.find('e') != std::string::npos) {
        const std::vector<std::string> parts = split(low, 'e');
        if (parts.size() < 2) return Result<BetSize>::err(err_msg);
        if (parts.size() > 2) return Result<BetSize>::err(err_msg);  // "1e2e3"

        int32_t num_streets = 0;
        if (!parts[0].empty()) {
            double f = 0.0;
            if (!parse_float(parts[0], f)) return Result<BetSize>::err(err_msg);
            if (std::trunc(f) != f || f == 0.0)
                return Result<BetSize>::err("Number of streets must be a positive integer: " + s);
            if (f > 100.0)
                return Result<BetSize>::err(
                    "Number of streets must be less than or equal to 100: " + s);
            num_streets = static_cast<int32_t>(f);
        }

        double max_pot_rel = kNoMaxRatio;
        if (!parts[1].empty()) {
            if (!ends_with(parts[1], '%')) return Result<BetSize>::err(err_msg);
            double f = 0.0;
            if (!parse_float(parts[1].substr(0, parts[1].size() - 1), f))
                return Result<BetSize>::err(err_msg);
            max_pot_rel = f / 100.0;
        }
        return BetSize::geometric(num_streets, max_pot_rel);
    }

    // --- pot relative: "70%" (must be after the 'e' check) ---------------
    if (ends_with(low, '%')) {
        double f = 0.0;
        if (!parse_float(low.substr(0, low.size() - 1), f))
            return Result<BetSize>::err(err_msg);
        return BetSize::pot_relative(f / 100.0);
    }

    // --- all-in ----------------------------------------------------------
    if (low == "a") return BetSize::all_in();

    return Result<BetSize>::err(err_msg);
}

namespace {

// Splits, trims, drops a single trailing empty element, and parses each.
Result<std::vector<BetSize>> parse_list(const std::string& str, bool is_raise) {
    std::vector<std::string> parts = split(str, ',');
    for (std::string& p : parts) p = trim(p);
    if (!parts.empty() && parts.back().empty()) parts.pop_back();

    std::vector<BetSize> out;
    out.reserve(parts.size());
    for (const std::string& p : parts) {
        Result<BetSize> b = bet_size_from_str(p, is_raise);
        if (!b) return Result<std::vector<BetSize>>::err(b.error());
        out.push_back(b.value());
    }
    // Rust: sort_unstable_by(partial_cmp) -- variant order, then payload.
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

Result<BetSizeOptions> BetSizeOptions::parse(const std::string& bet_str,
                                            const std::string& raise_str) {
    Result<std::vector<BetSize>> bet = parse_list(bet_str, false);
    if (!bet) return Result<BetSizeOptions>::err(bet.error());
    Result<std::vector<BetSize>> raise = parse_list(raise_str, true);
    if (!raise) return Result<BetSizeOptions>::err(raise.error());

    BetSizeOptions out;
    out.bet = bet.value();
    out.raise = raise.value();
    return out;
}

Result<DonkSizeOptions> DonkSizeOptions::parse(const std::string& donk_str) {
    Result<std::vector<BetSize>> donk = parse_list(donk_str, false);
    if (!donk) return Result<DonkSizeOptions>::err(donk.error());
    DonkSizeOptions out;
    out.donk = donk.value();
    return out;
}

}  // namespace pfs
