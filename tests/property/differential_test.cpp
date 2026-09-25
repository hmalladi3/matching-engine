// Differential testing: the real engine against the naive reference engine on
// seeded random request streams, with invariant and property checks after
// every request.
// @spec DLV-TEST-002, DLV-TEST-003
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <variant>

#include "matcher/matching_engine.h"
#include "support/capture_sink.h"
#include "support/reference_engine.h"
#include "support/request_generator.h"

#ifndef MATCHER_DIFF_REQUESTS
#define MATCHER_DIFF_REQUESTS 100000
#endif

namespace matcher {
namespace {

using test::CaptureSink;
using test::Event;
using test::Profile;
using test::ReferenceEngine;
using test::RequestGenerator;

__extension__ using u128 = unsigned __int128;  // quantity sums can exceed 2^64

constexpr std::uint64_t kRequests = MATCHER_DIFF_REQUESTS;
constexpr std::uint64_t kSnapshotEvery = 128;

std::string describe(const test::Request& r) {
    if (const auto* a = std::get_if<AddOrder>(&r))
        return "add id=" + std::to_string(a->id) + " side=" + std::to_string(static_cast<int>(a->side)) +
               " qty=" + std::to_string(a->qty) + " price=" + to_string(a->price);
    return "cancel id=" + std::to_string(std::get<CancelOrder>(r).id);
}

u128 resting_quantity(const std::vector<OrderBook::LevelSnapshot>& levels) {
    u128 total = 0;
    for (const auto& l : levels)
        for (const auto& [id, qty] : l.orders) total += qty;
    return total;
}

// Structural properties of one add's event stream that must hold regardless
// of what the reference engine says: triples of (Trade, aggressor fill,
// resting fill); at least one full fill per trade; at most one partial; the
// aggressor's limit is respected; its remaining quantity strictly decreases.
void check_event_grammar(const AddOrder& add, const std::vector<Event>& events) {
    ASSERT_EQ(events.size() % 3, 0u);
    Quantity remaining = add.qty;
    for (std::size_t i = 0; i < events.size(); i += 3) {
        const auto* trade = std::get_if<Trade>(&events[i]);
        ASSERT_NE(trade, nullptr) << "event " << i << " is not a trade";
        ASSERT_GT(trade->qty, 0u);
        if (add.side == Side::Buy) {
            ASSERT_LE(trade->price, add.price) << "buy traded above its limit";
        } else {
            ASSERT_GE(trade->price, add.price) << "sell traded below its limit";
        }

        ASSERT_LE(trade->qty, remaining);
        remaining -= trade->qty;
        int full = 0, partial = 0;
        if (remaining == 0) {
            const auto* f = std::get_if<OrderFullyFilled>(&events[i + 1]);
            ASSERT_NE(f, nullptr);
            ASSERT_EQ(f->id, add.id);
            ++full;
            ASSERT_EQ(i + 3, events.size()) << "trades after the aggressor was fully filled";
        } else {
            const auto* p = std::get_if<OrderPartiallyFilled>(&events[i + 1]);
            ASSERT_NE(p, nullptr);
            ASSERT_EQ(p->id, add.id);
            ASSERT_EQ(p->remaining, remaining);
            ++partial;
        }
        if (const auto* f = std::get_if<OrderFullyFilled>(&events[i + 2])) {
            ASSERT_NE(f->id, add.id);
            ++full;
        } else {
            const auto* p = std::get_if<OrderPartiallyFilled>(&events[i + 2]);
            ASSERT_NE(p, nullptr);
            ASSERT_NE(p->id, add.id);
            ++partial;
        }
        ASSERT_GE(full, 1) << "MATCH-EVT-003: each trade fully fills at least one order";
        ASSERT_LE(partial, 1);
    }
}

// Runs `requests` requests through both engines. Returns a digest of the
// engine's output for determinism checks.
std::string run_differential(Profile profile, std::uint64_t seed, std::uint64_t requests) {
    CaptureSink sink;
    MatchingEngine<CaptureSink> engine(sink, BookConfig{64, 8, kMaxNodes});  // small: growth is exercised
    ReferenceEngine reference;
    RequestGenerator generator(profile, seed);

    u128 added = 0, traded = 0, cancelled = 0;
    std::uint64_t digest = 1469598103934665603ULL;  // FNV-1a over output lines
    std::uint64_t accepted = 0, rejected = 0, trades = 0;

    for (std::uint64_t i = 0; i < requests; ++i) {
        const test::Request request = generator.next();
        std::vector<Event> expected;
        Reject engine_verdict = Reject::None, reference_verdict = Reject::None;

        if (const auto* add = std::get_if<AddOrder>(&request)) {
            engine_verdict = engine.add(*add);
            reference_verdict = reference.add(*add, expected);
            if (engine_verdict == Reject::None) added += add->qty;
        } else {
            const auto& cancel = std::get<CancelOrder>(request);
            Quantity qty = 0;
            engine_verdict = engine.cancel(cancel);
            reference_verdict = reference.cancel(cancel, qty);
            if (engine_verdict == Reject::None) cancelled += qty;
        }
        const std::vector<Event> actual = sink.take();

        const auto context = [&] {
            return "profile=" + std::string(name(profile)) + " seed=" + std::to_string(seed) + " request#" +
                   std::to_string(i) + " " + describe(request);
        };
        if (engine_verdict != reference_verdict || test::to_lines(actual) != test::to_lines(expected)) {
            ADD_FAILURE() << "divergence at " << context();
            EXPECT_EQ(engine_verdict, reference_verdict);
            EXPECT_EQ(test::to_lines(actual), test::to_lines(expected));
            return {};
        }
        engine_verdict == Reject::None ? ++accepted : ++rejected;

        if (const auto* add = std::get_if<AddOrder>(&request)) {
            check_event_grammar(*add, actual);
            if (::testing::Test::HasFatalFailure()) {
                ADD_FAILURE() << context();
                return {};
            }
        }
        for (const Event& e : actual) {
            if (const auto* t = std::get_if<Trade>(&e)) {
                traded += t->qty;
                ++trades;
            }
            for (char c : test::to_line(e)) digest = (digest ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }

        const OrderBook& book = engine.book();
        book.check_invariants();
        if (!book.empty(Side::Buy) && !book.empty(Side::Sell) &&
            book.best_price(Side::Buy) >= book.best_price(Side::Sell)) {
            ADD_FAILURE() << "crossed book after " << context();
            return {};
        }
        if (i % kSnapshotEvery == 0 || i + 1 == requests) {
            for (Side side : {Side::Buy, Side::Sell}) {
                if (book.snapshot(side) != reference.snapshot(side)) {
                    ADD_FAILURE() << "book snapshot differs after " << context();
                    return {};
                }
            }
        }
    }

    // Conservation: every accepted unit of quantity is traded (counted on both
    // sides), still resting, or cancelled.
    const u128 resting =
        resting_quantity(engine.book().snapshot(Side::Buy)) + resting_quantity(engine.book().snapshot(Side::Sell));
    EXPECT_TRUE(added == 2 * traded + resting + cancelled) << "quantity not conserved";

    // Every stream must exercise matching and rejections, or it proves little.
    EXPECT_GT(trades, 0u);
    EXPECT_GT(rejected, 0u);
    EXPECT_GT(accepted, requests / 4);
    return std::to_string(digest) + "/" + std::to_string(trades);
}

class Differential : public ::testing::TestWithParam<std::tuple<Profile, std::uint64_t>> {};

// @spec MATCH-ADD-001, MATCH-ADD-002, MATCH-ADD-003, MATCH-ADD-004, MATCH-ADD-005, MATCH-ADD-006,
//       MATCH-ADD-007, MATCH-ADD-008, MATCH-EVT-001, MATCH-EVT-002, MATCH-EVT-003, MATCH-EVT-004,
//       MATCH-CXL-001, MATCH-REJ-001, MATCH-REJ-002, MATCH-REJ-005, BOOK-INV-001
TEST_P(Differential, MatchesReferenceEngine) {
    const auto [profile, seed] = GetParam();
    (void)run_differential(profile, seed, kRequests);
}

INSTANTIATE_TEST_SUITE_P(AllProfiles, Differential,
                         ::testing::Combine(::testing::ValuesIn(test::kAllProfiles), ::testing::Values(1u, 20260925u)),
                         [](const auto& param_info) {
                             return std::string(name(std::get<0>(param_info.param))) + "_seed" +
                                    std::to_string(std::get<1>(param_info.param));
                         });

// Same input, same output, every time.
TEST(Determinism, IdenticalInputGivesIdenticalOutput) {
    const std::string first = run_differential(Profile::Mixed, 99, 20'000);
    const std::string second = run_differential(Profile::Mixed, 99, 20'000);
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

}  // namespace
}  // namespace matcher
