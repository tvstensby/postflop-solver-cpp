#include "harness.hpp"

#include <pfs/range.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace pfs;

namespace {
Range parse_ok(const std::string& s) {
    Result<Range> r = Range::parse(s);
    if (!r.is_ok()) {
        ::pfs::test::report_failure(__FILE__, __LINE__, "parse failed for \"" + s + "\": " + r.error());
        return Range();
    }
    return r.value();
}
}  // namespace

// Port of range.rs::range_regex -- the same 13 cases, run against the
// hand-written scanner that replaces RANGE_REGEX.
PFS_TEST(range, scanner_accepts_and_rejects) {
    struct Case {
        const char* input;
        bool ok;
        const char* range;
        const char* weight;  // nullptr => no weight group
    };
    const Case cases[] = {
        {"AK", true, "AK", nullptr},
        {"ak", true, "ak", nullptr},
        {"K9s:.67", true, "K9s", ".67"},
        {"88+:1.", true, "88+", "1."},
        {"98s-65s:0.25", true, "98s-65s", "0.25"},
        {"AcKh", true, "AcKh", nullptr},
        {"8h8s+:.67", true, "8h8s+", ".67"},
        {"9d8d-6d5d:0.25", true, "9d8d-6d5d", "0.25"},
        {"AKQ", false, nullptr, nullptr},
        {"AK+-AJ", false, nullptr, nullptr},
        {"K9s.67", false, nullptr, nullptr},
        {"88+:2.0", false, nullptr, nullptr},
        {"98s-21s", false, nullptr, nullptr},
    };

    for (const Case& c : cases) {
        const Result<Range> r = Range::parse(c.input);
        if (c.ok) {
            // Accepted by the pattern. "98s-21s" and "88+:2.0" are pattern-level
            // and semantic rejections respectively, so both must fail overall.
            CHECK(r.is_ok());
        } else {
            CHECK(!r.is_ok());
        }
    }

    // Weight is parsed, not just matched.
    CHECK_NEAR(parse_ok("K9s:.67").get_weight_suited(11, 7), 0.67f, 1e-6);
    CHECK_NEAR(parse_ok("88+:1.").get_weight_pair(6), 1.0f, 1e-6);
    CHECK_NEAR(parse_ok("98s-65s:0.25").get_weight_suited(7, 6), 0.25f, 1e-6);
}

// Port of range.rs::trim_regex.
PFS_TEST(range, whitespace_is_stripped_around_separators) {
    // Verified through parse equivalence, since trim_range_string is internal.
    CHECK(parse_ok("  AK  ") == parse_ok("AK"));
    CHECK(parse_ok("K9s: .67") == parse_ok("K9s:.67"));
    CHECK(parse_ok("88+, AQ+") == parse_ok("88+,AQ+"));
    CHECK(parse_ok("98s - 65s: 0.25") == parse_ok("98s-65s:0.25"));
    // Whitespace inside a combo is NOT removed, so this stays invalid.
    CHECK(!Range::parse("A K").is_ok());
}

// Port of range.rs::range_from_str, assertion for assertion.
PFS_TEST(range, from_str_equivalences) {
    CHECK(parse_ok("88+") == parse_ok("AA,KK,QQ,JJ,TT,99,88"));
    CHECK(parse_ok("8s8h+") == parse_ok("AhAs,KhKs,QhQs,JhJs,ThTs,9h9s,8h8s"));
    CHECK(parse_ok("98s+") == parse_ok("AKs,KQs,QJs,JTs,T9s,98s"));
    CHECK(parse_ok("A8o+") == parse_ok("AKo,AQo,AJo,ATo,A9o,A8o"));
    CHECK(parse_ok("88-55") == parse_ok("88,77,66,55"));
    CHECK(parse_ok("98s-65s") == parse_ok("98s,87s,76s,65s"));
    CHECK(parse_ok("AQo-86o") == parse_ok("AQo,KJo,QTo,J9o,T8o,97o,86o"));
    CHECK(parse_ok("K5-K2") == parse_ok("K5,K4,K3,K2"));
    CHECK(parse_ok("AhAs-QhQs,JJ") == parse_ok("JJ,AhAs,KhKs,QhQs"));

    CHECK(Range::parse("").is_ok());          // empty range is valid
    CHECK(Range::parse("AK,").is_ok());       // one trailing comma allowed
    CHECK(!Range::parse("AK,,").is_ok());     // two are not

    CHECK(!Range::parse("89").is_ok());       // first rank must be >= second
    CHECK(!Range::parse("AAo").is_ok());      // pair with suitedness
    CHECK(!Range::parse("AQo:1.1").is_ok());  // weight out of range

    CHECK(!Range::parse("AQo-AQo").is_ok());
    CHECK(!Range::parse("AQo-86s").is_ok());  // suitedness mismatch
    CHECK(!Range::parse("AQo-KQo").is_ok());
    CHECK(!Range::parse("K2-K5").is_ok());    // must descend
    CHECK(!Range::parse("AhAs-QsQh").is_ok());

    const Range data = parse_ok("85s:0.5");
    CHECK_NEAR(data.get_weight_suited(3, 6), 0.5f, 0.0);
    CHECK_NEAR(data.get_weight_suited(6, 3), 0.5f, 0.0);
    CHECK_NEAR(data.get_weight_offsuit(3, 6), 0.0f, 0.0);
    CHECK_NEAR(data.get_weight_offsuit(6, 3), 0.0f, 0.0);
}

// Port of range.rs::range_to_string -- the 10 golden pairs.
PFS_TEST(range, to_string_golden) {
    const std::pair<const char*, const char*> cases[] = {
        {"AA,KK", "KK+"},
        {"KK,QQ", "KK-QQ"},
        {"66-22,TT+", "TT+,66-22"},
        {"AA:0.5, KK:1.0, QQ:1.0, JJ:0.5", "AA:0.5,KK-QQ,JJ:0.5"},
        {"AA,AK,AQ", "AA,AQ+"},
        {"AK,AQ,AJs", "AJs+,AQo+"},
        {"KQ,KT,K9,K8,K6,K5", "KQ,KT-K8,K6-K5"},
        {"AhAs-QhQs,JJ", "JJ,AsAh,KsKh,QsQh"},
        {"KJs+,KQo,KsJh", "KJs+,KQo,KsJh"},
        {"KcQh,KJ", "KJ,KcQh"},
    };
    for (const auto& c : cases) {
        const std::string got = parse_ok(c.first).to_string();
        if (got != c.second)
            ::pfs::test::report_failure(__FILE__, __LINE__, std::string("to_string(\"") + c.first +
                                                                "\") = \"" + got +
                                                                "\", expected \"" + c.second + "\"");
    }
}

PFS_TEST(range, basic_operations) {
    const Range full = Range::ones();
    CHECK(!full.is_empty());
    CHECK(full.is_valid());
    CHECK(full.is_suit_symmetric());
    for (uint8_t s1 = 0; s1 < 4; ++s1)
        for (uint8_t s2 = 0; s2 < 4; ++s2) CHECK(full.is_suit_isomorphic(s1, s2));

    Range empty;
    CHECK(empty.is_empty());
    CHECK_NEAR(empty.get_weight_pair(12), 0.0f, 0.0);

    // A suit-specific range is not suit-symmetric.
    const Range specific = parse_ok("AsAh");
    CHECK(!specific.is_suit_symmetric());

    // The doc example from the Range struct.
    const Range r = parse_ok("QQ+,AKs");
    CHECK_NEAR(r.get_weight_pair(10), 1.0f, 0.0);   // QQ
    CHECK_NEAR(r.get_weight_offsuit(12, 11), 0.0f, 0.0);  // AKo not in range
    CHECK_NEAR(r.get_weight_suited(12, 11), 1.0f, 0.0);   // AKs is

    Range c = parse_ok("AA");
    c.clear();
    CHECK(c.is_empty());
}

// Rust inverts through a decimal round-trip so that 0.9 becomes exactly 0.1
// rather than 0.100000024. printf("%g") would not reproduce this.
PFS_TEST(range, invert_round_trips_through_shortest_decimal) {
    Range r;
    r.set_weight_pair(12, 0.9f);
    r.invert();
    CHECK_EQ(r.get_weight_pair(12), 0.1f);

    Range r2;
    r2.set_weight_pair(12, 0.25f);
    r2.invert();
    CHECK_EQ(r2.get_weight_pair(12), 0.75f);

    // A full range inverts to empty and back.
    Range ones = Range::ones();
    ones.invert();
    CHECK(ones.is_empty());
    ones.invert();
    CHECK(ones == Range::ones());
}

PFS_TEST(range, shortest_float_formatting) {
    CHECK_EQ(f32_to_shortest(1.0f), std::string("1"));
    CHECK_EQ(f32_to_shortest(0.5f), std::string("0.5"));
    CHECK_EQ(f32_to_shortest(0.25f), std::string("0.25"));
    CHECK_EQ(f32_to_shortest(0.67f), std::string("0.67"));
    CHECK_EQ(f32_to_shortest(0.1f), std::string("0.1"));
    CHECK_EQ(f32_to_shortest(0.0f), std::string("0"));
}

PFS_TEST(range, get_hands_weights_filters_dead_cards) {
    const Range r = parse_ok("AA");
    std::vector<Hole> hands;
    std::vector<float> weights;

    r.get_hands_weights(0, hands, weights);
    CHECK_EQ(hands.size(), 6u);  // C(4,2) ace combos
    CHECK_EQ(weights.size(), 6u);
    // Lexicographic order: (48,49) is AcAd.
    CHECK_EQ(hands[0].first, 48);
    CHECK_EQ(hands[0].second, 49);

    // Kill the ace of clubs; the three combos containing it must go.
    r.get_hands_weights(uint64_t{1} << 48, hands, weights);
    CHECK_EQ(hands.size(), 3u);

    const Range rt = parse_ok("AA:0.5");
    rt.get_hands_weights(0, hands, weights);
    CHECK_NEAR(weights[0], 0.5f, 0.0);

    // Round-trip via from_hands_weights.
    const Result<Range> back = Range::from_hands_weights(hands, weights);
    CHECK(back.is_ok());
    CHECK(back.value() == rt);
}

PFS_TEST(range, raw_data_round_trip) {
    const Range r = parse_ok("88+,AQs");
    const Result<Range> back = Range::from_raw_data(r.raw_data());
    CHECK(back.is_ok());
    CHECK(back.value() == r);

    // Wrong length and out-of-range weights are rejected.
    const std::vector<float> short_data(10, 0.5f);
    CHECK(!Range::from_raw_data(short_data).is_ok());
    std::vector<float> bad(kNumHandIndices, 0.0f);
    bad[0] = 1.5f;
    CHECK(!Range::from_raw_data(bad).is_ok());
}

// The two ranges used by examples/basic.rs and most of the ported game tests.
PFS_TEST(range, example_ranges_parse) {
    CHECK(Range::parse("66+,A8s+,A5s-A4s,AJo+,K9s+,KQo,QTs+,JTs,96s+,85s+,75s+,65s,54s").is_ok());
    CHECK(Range::parse("QQ-22,AQs-A2s,ATo+,K5s+,KJo+,Q8s+,J8s+,T7s+,96s+,86s+,75s+,64s+,53s+")
              .is_ok());
    CHECK(Range::parse("KK-22,K9-K2,Q8-Q2,J8-J2,T8-T2,92+,82+,72+,62+").is_ok());
    CHECK(Range::parse("AsAh,QsQh").is_ok());
    CHECK(Range::parse("TT+,AKo,AQs+").is_ok());
}
