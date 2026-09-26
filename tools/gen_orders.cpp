// Writes a seeded, reproducible request stream in the assignment's CSV format.
//
// Usage:
//   gen_orders --profile tight|deep|cancel_heavy|sweep|id_reuse|extreme|mixed --count N [--seed S]
//   gen_orders --scenario huge_book [--orders N] [--levels L]
//
// The same arguments produce byte-identical output on every platform.
// @spec DLV-TEST-001, DLV-STRESS-002
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

#include "support/request_generator.h"

namespace {

using namespace matcher;

void print_add(OrderId id, Side side, Quantity qty, Price price) {
    char text[Price::kMaxFormattedLen + 1];
    *format_price(price, text) = '\0';
    std::printf("0,%llu,%d,%llu,%s\n", static_cast<unsigned long long>(id), static_cast<int>(side),
                static_cast<unsigned long long>(qty), text);
}

void print_cancel(OrderId id) { std::printf("1,%llu\n", static_cast<unsigned long long>(id)); }

// A book of `orders` resting orders (half per side, quantity 1) spread over
// `levels` price levels per side; then every other order is cancelled, one
// aggressive order sweeps each side, and two probes confirm the book is
// empty. Writes the exact number of trades to expect to stderr.
int huge_book(unsigned long long orders, unsigned long long levels) {
    const unsigned long long per_level = orders / (2 * levels);
    if (levels == 0 || per_level == 0) {
        std::fprintf(stderr, "huge_book: need orders >= 2 * levels\n");
        return 2;
    }
    constexpr std::int64_t kMid = 1'000'000;
    const auto id_of = [&](unsigned long long k, unsigned long long level, Side side) -> OrderId {
        return 1 + 2 * (k * levels + level) + static_cast<unsigned long long>(side);
    };
    const auto price_of = [&](unsigned long long level, Side side) {
        const auto offset = static_cast<std::int64_t>(level) + 1;
        return Price::from_units(side == Side::Buy ? kMid - offset : kMid + offset);
    };

    // 1. Rest every order; round-robin over levels so each level's FIFO has depth.
    for (unsigned long long k = 0; k < per_level; ++k)
        for (unsigned long long level = 0; level < levels; ++level)
            for (Side side : {Side::Buy, Side::Sell}) print_add(id_of(k, level, side), side, 1, price_of(level, side));

    // 2. Cancel every other order in each FIFO (odd k).
    unsigned long long remaining_per_side = 0;
    for (unsigned long long k = 0; k < per_level; ++k) {
        for (unsigned long long level = 0; level < levels; ++level) {
            if (k % 2 == 1) {
                print_cancel(id_of(k, level, Side::Buy));
                print_cancel(id_of(k, level, Side::Sell));
            } else {
                ++remaining_per_side;
            }
        }
    }

    // 3. One aggressive order sweeps each side completely (every order has qty 1).
    const OrderId next = 2 * per_level * levels + 1;
    print_add(next, Side::Sell, remaining_per_side, Price::from_units(1));
    print_add(next + 1, Side::Buy, remaining_per_side, Price::from_units(kMid * 10));

    // 4. Probes: with an empty book each rests without trading and is then cancelled.
    print_add(next + 2, Side::Sell, 1, Price::from_raw(-Price::kMaxRaw));
    print_cancel(next + 2);
    print_add(next + 3, Side::Buy, 1, Price::from_raw(Price::kMaxRaw));
    print_cancel(next + 3);

    std::fprintf(stderr, "expected_trades=%llu\n", 2 * remaining_per_side);
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: gen_orders --profile tight|deep|cancel_heavy|sweep|id_reuse|extreme|mixed --count N "
                 "[--seed S]\n"
                 "       gen_orders --scenario huge_book [--orders N] [--levels L]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view profile_name = "mixed";
    std::string_view scenario;
    unsigned long long count = 1'000'000, seed = 1, orders = 5'000'000, levels = 100'000;
    if (argc % 2 == 0) return usage();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--profile")
            profile_name = value;
        else if (flag == "--count")
            count = std::stoull(value);
        else if (flag == "--seed")
            seed = std::stoull(value);
        else if (flag == "--scenario")
            scenario = value;
        else if (flag == "--orders")
            orders = std::stoull(value);
        else if (flag == "--levels")
            levels = std::stoull(value);
        else
            return usage();
    }
    if (scenario == "huge_book") return huge_book(orders, levels);
    if (!scenario.empty()) return usage();

    const auto profile = test::profile_from_name(profile_name);
    if (!profile) return usage();
    test::RequestGenerator generator(*profile, seed);
    for (unsigned long long i = 0; i < count; ++i) {
        const test::Request request = generator.next();
        if (const auto* a = std::get_if<AddOrder>(&request))
            print_add(a->id, a->side, a->qty, a->price);
        else
            print_cancel(std::get<CancelOrder>(request).id);
    }
    return 0;
}
