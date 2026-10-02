#include "harness.hpp"

#include <pfs/bet_size.hpp>

#include <string>
#include <vector>

using namespace pfs;

// Port of bet_size.rs::test_bet_size_from_str.
PFS_TEST(bet_size, from_str_accepts) {
    struct Case {
        const char* input;
        BetSize expected;
    };
    const Case cases[] = {
        {"0%", BetSize::pot_relative(0.0)},
        {"75%", BetSize::pot_relative(0.75)},
        {"112.5%", BetSize::pot_relative(1.125)},
        {"1.001x", BetSize::prev_bet_relative(1.001)},
        {"3.5X", BetSize::prev_bet_relative(3.5)},
        {"0c", BetSize::additive(0, 0)},
        {"123C", BetSize::additive(123, 0)},
        {"0c1r", BetSize::additive(0, 1)},
        {"100C100R", BetSize::additive(100, 100)},
        {"e", BetSize::geometric(0, kNoMaxRatio)},
        {"E", BetSize::geometric(0, kNoMaxRatio)},
        {"2e", BetSize::geometric(2, kNoMaxRatio)},
        {"E37.5%", BetSize::geometric(0, 0.375)},
        {"100e.5%", BetSize::geometric(100, 0.005)},
        {"a", BetSize::all_in()},
        {"A", BetSize::all_in()},
    };

    for (const Case& c : cases) {
        const Result<BetSize> got = bet_size_from_str(c.input, true);
        if (!got.is_ok()) {
            ::pfs::test::report_failure(__FILE__, __LINE__,
                                        std::string("rejected \"") + c.input + "\": " + got.error());
            continue;
        }
        if (!(got.value() == c.expected))
            ::pfs::test::report_failure(__FILE__, __LINE__,
                                        std::string("wrong parse for \"") + c.input + "\"");
    }
}

// Port of the 28-case error table.
PFS_TEST(bet_size, from_str_rejects) {
    const char* bad[] = {"",      "0",       "1.23",  "%",     "+42%",   "-30%",  "x",
                         "0x",    "1x",      "c",     "12.3c", "10c10",  "42cr",  "c3r",
                         "0c0r",  "123c101r", "1c2r3", "12c3.4r", "0e",   "2.7e",  "101e",
                         "3e7",   "E%",      "1e2e3", "bet",   "1a",     "a1"};
    for (const char* s : bad) {
        if (bet_size_from_str(s, true).is_ok())
            ::pfs::test::report_failure(__FILE__, __LINE__,
                                        std::string("should have rejected \"") + s + "\"");
    }
}

PFS_TEST(bet_size, raise_only_forms_rejected_for_bets) {
    // 'x' and the 'r' cap are valid only for raises.
    CHECK(!bet_size_from_str("2.5x", false).is_ok());
    CHECK(!bet_size_from_str("20c3r", false).is_ok());
    CHECK(bet_size_from_str("2.5x", true).is_ok());
    CHECK(bet_size_from_str("20c3r", true).is_ok());
}

PFS_TEST(bet_size, geometric_check_precedes_percent) {
    // "3e200%" must be geometric, not pot-relative -- the Rust notes this
    // ordering requirement explicitly.
    const Result<BetSize> g = bet_size_from_str("3e200%", false);
    CHECK(g.is_ok());
    CHECK(g.value() == BetSize::geometric(3, 2.0));
}

// Port of bet_size.rs::test_bet_sizes_from_str.
PFS_TEST(bet_size, options_parse_and_sort) {
    {
        const Result<BetSizeOptions> o = BetSizeOptions::parse("40%, 70%", "");
        CHECK(o.is_ok());
        BetSizeOptions want;
        want.bet = {BetSize::pot_relative(0.4), BetSize::pot_relative(0.7)};
        CHECK(o.value() == want);
    }
    {
        // Note the trailing comma in the bet string, and that the raise list is
        // sorted into variant order: PotRelative < PrevBetRelative < Geometric.
        const Result<BetSizeOptions> o = BetSizeOptions::parse("50c, e, a,", "25%, 2.5x, e200%");
        CHECK(o.is_ok());
        BetSizeOptions want;
        want.bet = {BetSize::additive(50, 0), BetSize::geometric(0, kNoMaxRatio),
                    BetSize::all_in()};
        want.raise = {BetSize::pot_relative(0.25), BetSize::prev_bet_relative(2.5),
                      BetSize::geometric(0, 2.0)};
        CHECK(o.value() == want);
    }

    CHECK(!BetSizeOptions::parse("2.5x", "").is_ok());
    CHECK(!BetSizeOptions::parse(",", "").is_ok());
}

// Port of bet_size.rs::test_donk_sizes_from_str.
PFS_TEST(bet_size, donk_options_parse) {
    {
        const Result<DonkSizeOptions> o = DonkSizeOptions::parse("40%, 70%");
        CHECK(o.is_ok());
        DonkSizeOptions want;
        want.donk = {BetSize::pot_relative(0.4), BetSize::pot_relative(0.7)};
        CHECK(o.value() == want);
    }
    {
        const Result<DonkSizeOptions> o = DonkSizeOptions::parse("50c, e, a,");
        CHECK(o.is_ok());
        DonkSizeOptions want;
        want.donk = {BetSize::additive(50, 0), BetSize::geometric(0, kNoMaxRatio),
                     BetSize::all_in()};
        CHECK(o.value() == want);
    }

    CHECK(!DonkSizeOptions::parse("2.5x").is_ok());
    CHECK(!DonkSizeOptions::parse(",").is_ok());
}

// The ordering that fixes how actions are pushed into the tree: variant
// declaration order first, then payload.
PFS_TEST(bet_size, ordering_is_variant_then_payload) {
    CHECK(BetSize::pot_relative(0.9) < BetSize::prev_bet_relative(1.1));
    CHECK(BetSize::prev_bet_relative(99.0) < BetSize::additive(1, 0));
    CHECK(BetSize::additive(999, 0) < BetSize::geometric(1, 1.0));
    CHECK(BetSize::geometric(100, 9.0) < BetSize::all_in());
    // AllIn is always last and never less than itself.
    CHECK(!(BetSize::all_in() < BetSize::all_in()));
    // Within a variant, payload order.
    CHECK(BetSize::pot_relative(0.4) < BetSize::pot_relative(0.7));
    CHECK(BetSize::additive(50, 0) < BetSize::additive(50, 1));
}

// The doc example on BetSizeOptions, and the sizes used by examples/basic.rs.
PFS_TEST(bet_size, doc_examples) {
    const Result<BetSizeOptions> o = BetSizeOptions::parse("50%, 100c, 2e, a", "2.5x");
    CHECK(o.is_ok());
    BetSizeOptions want;
    want.bet = {BetSize::pot_relative(0.5), BetSize::additive(100, 0),
                BetSize::geometric(2, kNoMaxRatio), BetSize::all_in()};
    want.raise = {BetSize::prev_bet_relative(2.5)};
    CHECK(o.value() == want);

    const Result<BetSizeOptions> b = BetSizeOptions::parse("60%, e, a", "2.5x");
    CHECK(b.is_ok());
    CHECK_EQ(b.value().bet.size(), 3u);
    CHECK_EQ(b.value().raise.size(), 1u);

    CHECK(DonkSizeOptions::parse("50%").is_ok());
}
