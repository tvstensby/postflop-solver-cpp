// Port of the tests in src/bunching.rs.
//
// The independent-range cases produce integer-valued results below 2^53, so the
// comparisons are exact even though the atomic accumulation in phases 1 and 2 is
// order-dependent.
#include "harness.hpp"

#include <pfs/bunching.hpp>

#include <cstdlib>
#include <vector>

using namespace pfs;

PFS_TEST(bunching, next_combination_sequence) {
    const uint64_t seq[15] = {0b001111, 0b010111, 0b011011, 0b011101, 0b011110,
                              0b100111, 0b101011, 0b101101, 0b101110, 0b110011,
                              0b110101, 0b110110, 0b111001, 0b111010, 0b111100};
    uint64_t mask = 0b001111;
    for (size_t i = 0; i < 15; ++i) {
        CHECK_EQ(mask, seq[i]);
        mask = next_combination(mask);
    }
}

PFS_TEST(bunching, compress_mask_deletes_flop_positions) {
    const uint64_t x = (uint64_t{1} << 14) | (uint64_t{1} << 16) | (uint64_t{1} << 24) |
                       (uint64_t{1} << 26);
    const uint64_t y = compress_mask(x, {5, 15, 25});
    const uint64_t want = (uint64_t{1} << 13) | (uint64_t{1} << 14) | (uint64_t{1} << 22) |
                          (uint64_t{1} << 23);
    CHECK_EQ(y, want);
}

PFS_TEST(bunching, comb_table_matches_binomials) {
    // Row i holds C(n, i+1).
    CHECK_EQ(kCombTable[0][1], 1u);
    CHECK_EQ(kCombTable[0][48], 48u);
    CHECK_EQ(kCombTable[1][2], 1u);
    CHECK_EQ(kCombTable[1][48], 1128u);   // C(48, 2)
    CHECK_EQ(kCombTable[3][48], 194580u); // C(48, 4)
    CHECK_EQ(kCombTable[5][48], 12271512u);
    // The constants must agree with the table.
    CHECK_EQ(kCombTable[1][49 - 1] + kCombTable[0][48], 1176u);  // C(49,2)
}

namespace {
// Exhaustive round-trip over all C(49, k) masks for a given k.
void check_comb(size_t k, size_t count) {
    uint64_t mask = (uint64_t{1} << k) - 1;
    for (size_t i = 0; i < count; ++i) {
        if (mask_to_index(mask, k) != i || index_to_mask(i, k) != mask) {
            ::pfs::test::report_failure(__FILE__, __LINE__,
                                        "combinadic mismatch at k=" + std::to_string(k) +
                                            " i=" + std::to_string(i));
            return;
        }
        mask = next_combination(mask);
    }
}
}  // namespace

PFS_TEST(bunching, comb_4_round_trips) { check_comb(4, COMB_49_4); }
PFS_TEST(bunching, comb_5_round_trips) { check_comb(5, COMB_49_5); }
PFS_TEST(bunching, comb_8_prefix_round_trips) { check_comb(8, 10000); }

PFS_TEST(bunching_slow, comb_6_round_trips) {
    if (std::getenv("PFS_RUN_SLOW") == nullptr) {
        std::printf("    skipped (set PFS_RUN_SLOW=1 to run)\n");
        return;
    }
    check_comb(6, COMB_49_6);
}

PFS_TEST(bunching, create_validation) {
    const std::array<Card, 3> flop = flop_from_str("2s2h2d").value();
    const Range full = Range::ones();

    CHECK(BunchingData::create({full}, flop).is_ok());
    // Empty ranges are dropped, not rejected -- but all-empty is an error.
    CHECK(!BunchingData::create({Range()}, flop).is_ok());
    CHECK(!BunchingData::create({}, flop).is_ok());
    // At most four folded players (6-max).
    CHECK(BunchingData::create({full, full, full, full}, flop).is_ok());
    CHECK(!BunchingData::create({full, full, full, full, full}, flop).is_ok());
    // Ranges must be suit-symmetric.
    CHECK(!BunchingData::create({Range::parse("AsAh").value()}, flop).is_ok());
    CHECK(BunchingData::create({Range::parse("AA").value()}, flop).is_ok());
    // Duplicate flop cards are rejected.
    CHECK(!BunchingData::create({full}, {Card{5}, Card{5}, Card{7}}).is_ok());

    // The flop is sorted on construction.
    Result<BunchingData> d = BunchingData::create({full}, {Card{20}, Card{5}, Card{40}});
    CHECK(d.is_ok());
    if (d.is_ok()) {
        CHECK_EQ(d.value().flop()[0], 5);
        CHECK_EQ(d.value().flop()[1], 20);
        CHECK_EQ(d.value().flop()[2], 40);
    }
}

// Port of bunching.rs::test_bunching_independent_1. A single folded player holding
// any two of the 48 non-flop cards: every count is a plain combination, so the
// expected values are exact integers.
PFS_TEST(bunching, independent_one_player) {
    const char* range1 = "33+,A3+,K3+,Q3+,J3+,T3+,93+,83+,73+,63+,53+,43+,33";
    const std::array<Card, 3> flop = flop_from_str("2s2h2d").value();

    Result<BunchingData> created =
        BunchingData::create({Range::parse(range1).value()}, flop);
    CHECK(created.is_ok());
    if (!created.is_ok()) return;
    BunchingData& b = created.value();

    b.phase1_prepare();
    while (b.progress_percent() < 100) b.phase1_proceed_by_percent();
    b.phase2_prepare();
    while (b.progress_percent() < 100) b.phase2_proceed_by_percent();
    b.phase3_prepare();
    while (b.progress_percent() < 100) b.phase3_proceed_by_percent();

    CHECK(b.is_ready());
    CHECK_EQ(b.phase(), 3);

    // 49 cards remain after the flop, and the range holds no deuces, so 2c (the
    // only remaining deuce) is never usable by the folded player. Hence:
    //   2c among the dead cards  -> all 45 remaining are usable   -> C(45, 2)
    //   2c not dead              -> one of the 45 is unusable     -> C(44, 2)
    const uint64_t flop_mask = (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) |
                               (uint64_t{1} << flop[2]);
    const Card two_c = card_from_str("2c").value();

    // Sample a range of 4-card masks rather than all 211,876: enough to catch a
    // systematic error, cheap enough to run by default.
    size_t checked = 0;
    for (Card a = 0; a < 52 && checked < 400; ++a) {
        if ((flop_mask >> a) & 1) continue;
        for (Card c = static_cast<Card>(a + 1); c < 52 && checked < 400; ++c) {
            if ((flop_mask >> c) & 1) continue;
            for (Card d = static_cast<Card>(c + 1); d < 52 && checked < 400; ++d) {
                if ((flop_mask >> d) & 1) continue;
                for (Card e = static_cast<Card>(d + 1); e < 52 && checked < 400; ++e) {
                    if ((flop_mask >> e) & 1) continue;
                    const uint64_t mask = (uint64_t{1} << a) | (uint64_t{1} << c) |
                                          (uint64_t{1} << d) | (uint64_t{1} << e);
                    const bool has_2c = ((mask >> two_c) & 1) != 0;
                    const float want = has_2c ? 45.0f * 44.0f / 2.0f : 44.0f * 43.0f / 2.0f;
                    CHECK_EQ(b.result_4cards(mask), want);
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 0);

    // A 6-card query, the size the solver actually uses on the river. 43 cards
    // remain, and the same 2c reasoning applies.
    std::vector<Card> live;
    for (Card x = 0; x < 52; ++x)
        if (!((flop_mask >> x) & 1) && x != two_c) live.push_back(x);
    uint64_t mask6 = 0;
    for (size_t i = 0; i < 6; ++i) mask6 |= uint64_t{1} << live[i];
    CHECK_EQ(b.result_6cards(mask6), 42.0f * 41.0f / 2.0f);

    uint64_t mask6_with_2c = uint64_t{1} << two_c;
    for (size_t i = 0; i < 5; ++i) mask6_with_2c |= uint64_t{1} << live[i];
    CHECK_EQ(b.result_6cards(mask6_with_2c), 43.0f * 42.0f / 2.0f);
}

// Two independent folded players: the count becomes an ordered product of
// combinations, still exact.
PFS_TEST(bunching, independent_two_players) {
    const Range full = Range::ones();
    const std::array<Card, 3> flop = flop_from_str("2s2h2d").value();

    Result<BunchingData> created = BunchingData::create({full, full}, flop);
    CHECK(created.is_ok());
    if (!created.is_ok()) return;
    BunchingData& b = created.value();
    b.process();
    CHECK(b.is_ready());

    // With 4 dead cards, 45 remain. Player A takes 2 of 45, player B 2 of the
    // remaining 43.
    const uint64_t flop_mask = (uint64_t{1} << flop[0]) | (uint64_t{1} << flop[1]) |
                               (uint64_t{1} << flop[2]);
    std::vector<Card> live;
    for (Card x = 0; x < 52; ++x)
        if (!((flop_mask >> x) & 1)) live.push_back(x);

    uint64_t mask4 = 0;
    for (size_t i = 0; i < 4; ++i) mask4 |= uint64_t{1} << live[i];
    const double a = 45.0 * 44.0 / 2.0;
    const double c = 43.0 * 42.0 / 2.0;
    CHECK_NEAR(b.result_4cards(mask4), static_cast<float>(a * c), 1.0);
}

PFS_TEST(bunching, stepped_api_rejects_out_of_order_calls) {
    const std::array<Card, 3> flop = flop_from_str("2s2h2d").value();
    Result<BunchingData> created = BunchingData::create({Range::ones()}, flop);
    CHECK(created.is_ok());
    if (!created.is_ok()) return;
    BunchingData& b = created.value();

    // Phase 2 before phase 1 is a programming error.
    CHECK_THROWS(b.phase2_prepare());
    CHECK_THROWS(b.phase3_prepare());
    CHECK_THROWS(b.phase1_proceed_by_percent());

    b.phase1_prepare();
    CHECK_THROWS(b.phase1_prepare());   // twice
    CHECK_THROWS(b.phase2_prepare());   // phase 1 not finished
    CHECK(!b.is_ready());
}
