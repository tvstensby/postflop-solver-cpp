#include "harness.hpp"

#include "hand.hpp"

#include <pfs/card.hpp>

#include <cstdlib>
#include <vector>

using namespace pfs;

// Port of card.rs::test_card_pair_index -- exhaustive over all 1326 pairs, both
// directions. Cheap, and it is the guard on index_to_card_pair's sqrt/ceil
// closed form, so it should run in every configuration.
PFS_TEST(card, pair_index_round_trips) {
    size_t expected = 0;
    for (Card card1 = 0; card1 < 52; ++card1) {
        for (Card card2 = static_cast<Card>(card1 + 1); card2 < 52; ++card2) {
            const size_t index = card_pair_to_index(card1, card2);
            CHECK_EQ(index, expected);
            // Argument order must not matter.
            CHECK_EQ(card_pair_to_index(card2, card1), expected);

            const Hole back = index_to_card_pair(index);
            CHECK_EQ(back.first, card1);
            CHECK_EQ(back.second, card2);
            ++expected;
        }
    }
    CHECK_EQ(expected, 1326u);
}

PFS_TEST(card, string_conversions) {
    // The doc examples from range.rs.
    CHECK_EQ(card_to_string(0).value(), std::string("2c"));
    CHECK_EQ(card_to_string(5).value(), std::string("3d"));
    CHECK_EQ(card_to_string(10).value(), std::string("4h"));
    CHECK_EQ(card_to_string(51).value(), std::string("As"));
    CHECK(!card_to_string(52).is_ok());

    CHECK_EQ(card_from_str("2c").value(), 0);
    CHECK_EQ(card_from_str("3d").value(), 5);
    CHECK_EQ(card_from_str("4h").value(), 10);
    CHECK_EQ(card_from_str("As").value(), 51);
    // Ranks are case-insensitive, suits are not.
    CHECK_EQ(card_from_str("as").value(), 51);
    CHECK(!card_from_str("AS").is_ok());
    CHECK(!card_from_str("A").is_ok());
    CHECK(!card_from_str("Asx").is_ok());

    // Output is sorted descending by card id.
    CHECK_EQ(hole_to_string(Hole{Card{0}, Card{5}}).value(), std::string("3d2c"));
    CHECK_EQ(hole_to_string(Hole{Card{10}, Card{51}}).value(), std::string("As4h"));
    CHECK(!hole_to_string(Hole{Card{52}, Card{53}}).is_ok());

    const std::vector<Hole> holes{{Card{0}, Card{5}}, {Card{10}, Card{51}}};
    const auto strs = holes_to_strings(holes);
    CHECK(strs.is_ok());
    CHECK_EQ(strs.value()[0], std::string("3d2c"));
    CHECK_EQ(strs.value()[1], std::string("As4h"));
    CHECK(!holes_to_strings({{Card{52}, Card{53}}}).is_ok());
}

PFS_TEST(card, flop_from_str_sorts_and_rejects) {
    const auto a = flop_from_str("2c3d4h");
    CHECK(a.is_ok());
    CHECK_EQ(a.value()[0], 0);
    CHECK_EQ(a.value()[1], 5);
    CHECK_EQ(a.value()[2], 10);

    // Optional spaces, and the result is sorted ascending.
    const auto b = flop_from_str("As Ah Ks");
    CHECK(b.is_ok());
    CHECK_EQ(b.value()[0], 47);
    CHECK_EQ(b.value()[1], 50);
    CHECK_EQ(b.value()[2], 51);

    CHECK(!flop_from_str("2c3d4h5s").is_ok());  // too many
    CHECK(!flop_from_str("2c3d").is_ok());      // too few
    CHECK(!flop_from_str("2c2c3d").is_ok());    // duplicates

    // The board used by most of the ported game tests.
    const auto c = flop_from_str("Td9d6h");
    CHECK(c.is_ok());
    CHECK_EQ(card_to_string(c.value()[0]).value(), std::string("6h"));
    CHECK_EQ(card_to_string(c.value()[1]).value(), std::string("9d"));
    CHECK_EQ(card_to_string(c.value()[2]).value(), std::string("Td"));
}

PFS_TEST(hand, table_is_ascending_and_unique) {
    for (size_t i = 1; i < kHandTableSize; ++i) CHECK(kHandTable[i - 1] < kHandTable[i]);
}

PFS_TEST(hand, known_hands_rank_correctly) {
    auto h = [](std::initializer_list<const char*> cs) {
        Hand hand;
        for (const char* c : cs) hand = hand.add_card(card_from_str(c).value());
        return hand;
    };
    auto category = [](const Hand& hand) { return hand.evaluate_internal() >> 26; };

    // Royal flush vs a lesser straight flush.
    const Hand royal = h({"As", "Ks", "Qs", "Js", "Ts", "2c", "3d"});
    const Hand sf = h({"9s", "8s", "7s", "6s", "5s", "2c", "3d"});
    CHECK_EQ(category(royal), 8u);
    CHECK_EQ(category(sf), 8u);
    CHECK(royal.evaluate() > sf.evaluate());

    CHECK_EQ(category(h({"As", "Ac", "Ad", "Ah", "Ks", "2c", "3d"})), 7u);  // quads
    CHECK_EQ(category(h({"As", "Ac", "Ad", "Ks", "Kc", "2c", "3d"})), 6u);  // full house
    CHECK_EQ(category(h({"As", "Ks", "9s", "5s", "3s", "2c", "4d"})), 5u);  // flush
    CHECK_EQ(category(h({"As", "Kc", "Qd", "Jh", "Ts", "2c", "3d"})), 4u);  // straight
    CHECK_EQ(category(h({"As", "Ac", "Ad", "Ks", "Qc", "2c", "4d"})), 3u);  // trips
    CHECK_EQ(category(h({"As", "Ac", "Ks", "Kc", "Qd", "2c", "4d"})), 2u);  // two pair
    CHECK_EQ(category(h({"As", "Ac", "Ks", "Qc", "9d", "2c", "4d"})), 1u);  // one pair
    CHECK_EQ(category(h({"As", "Kc", "Qs", "9c", "7d", "3c", "2d"})), 0u);  // high card

    // The wheel: A2345 is a straight, and the weakest one.
    const Hand wheel = h({"As", "2c", "3d", "4h", "5s", "9c", "Td"});
    CHECK_EQ(category(wheel), 4u);
    const Hand six_high = h({"2c", "3d", "4h", "5s", "6c", "9c", "Td"});
    CHECK(six_high.evaluate() > wheel.evaluate());

    // A steel wheel is a straight flush, not just a flush.
    CHECK_EQ(category(h({"As", "2s", "3s", "4s", "5s", "9c", "Td"})), 8u);
}

// Port of hand.rs::test_all_hands: all C(52,7) = 133,784,560 hands, asserting
// every table entry is reachable and every category count matches. Slow, so it
// is opt-in via the `slow` label (or by passing a filter).
PFS_TEST(hand_slow, exhaustive_seven_card_counts) {
    if (std::getenv("PFS_RUN_SLOW") == nullptr) {
        std::printf("    skipped (set PFS_RUN_SLOW=1 to run)\n");
        return;
    }

    std::vector<bool> appeared(kHandTableSize, false);
    long long counter[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (size_t i = 0; i < 52; ++i) {
        const Hand h1 = Hand().add_card(i);
        for (size_t j = i + 1; j < 52; ++j) {
            const Hand h2 = h1.add_card(j);
            for (size_t k = j + 1; k < 52; ++k) {
                const Hand h3 = h2.add_card(k);
                for (size_t m = k + 1; m < 52; ++m) {
                    const Hand h4 = h3.add_card(m);
                    for (size_t n = m + 1; n < 52; ++n) {
                        const Hand h5 = h4.add_card(n);
                        for (size_t p = n + 1; p < 52; ++p) {
                            const Hand h6 = h5.add_card(p);
                            for (size_t q = p + 1; q < 52; ++q) {
                                const int32_t raw = h6.add_card(q).evaluate_internal();
                                const auto it =
                                    std::lower_bound(kHandTable.begin(), kHandTable.end(), raw);
                                if (it == kHandTable.end() || *it != raw) {
                                    CHECK(false);
                                    return;
                                }
                                appeared[static_cast<size_t>(it - kHandTable.begin())] = true;
                                counter[raw >> 26] += 1;
                            }
                        }
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < kHandTableSize; ++i) CHECK(appeared[i]);
    CHECK_EQ(counter[8], 41584LL);      // straight flush
    CHECK_EQ(counter[7], 224848LL);     // four of a kind
    CHECK_EQ(counter[6], 3473184LL);    // full house
    CHECK_EQ(counter[5], 4047644LL);    // flush
    CHECK_EQ(counter[4], 6180020LL);    // straight
    CHECK_EQ(counter[3], 6461620LL);    // three of a kind
    CHECK_EQ(counter[2], 31433400LL);   // two pair
    CHECK_EQ(counter[1], 58627800LL);   // one pair
    CHECK_EQ(counter[0], 23294460LL);   // high card
}
