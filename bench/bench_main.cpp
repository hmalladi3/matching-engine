// Latency and throughput benchmark for the matching engine.
//
// Per-operation scenarios time one engine call at a time against a book of a
// given size, then undo the operation untimed so the book's shape stays
// stable across samples. Throughput scenarios time whole batches, which is
// accurate even where the clock is coarse.
//
// Usage: matcher_bench [--quick] [--sizes 1000,10000,...] [--repeat R] [--seed S]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bench_config.h"
#include "matcher/app.h"
#include "matcher/buffered_writer.h"
#include "matcher/event_writer.h"
#include "matcher/matching_engine.h"
#include "matcher/request_parser.h"
#include "support/request_generator.h"
#include "timer.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace matcher::bench {
namespace {

// Keeps the optimizer from discarding a computed value.
template <class T>
void keep(const T& value) {
    asm volatile("" : : "g"(&value) : "memory");
}

// ---- statistics -------------------------------------------------------------------

struct Stats {
    double p50 = 0, p99 = 0, p999 = 0, max = 0, mean = 0, ops_per_sec = 0;
};

Stats summarize(std::vector<double>& ns) {
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) {
        return ns[std::min(ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(ns.size())))];
    };
    double sum = 0;
    for (double v : ns) sum += v;
    Stats s;
    s.p50 = pct(0.50);
    s.p99 = pct(0.99);
    s.p999 = pct(0.999);
    s.max = ns.back();
    s.mean = sum / static_cast<double>(ns.size());
    s.ops_per_sec = 1e9 / s.mean;
    return s;
}

// Median of each statistic across repeated runs (robust to one noisy run).
Stats median_of(std::vector<Stats> runs) {
    auto med = [&](double Stats::*field) {
        std::vector<double> v;
        for (const Stats& s : runs) v.push_back(s.*field);
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    return {med(&Stats::p50), med(&Stats::p99),  med(&Stats::p999),
            med(&Stats::max), med(&Stats::mean), med(&Stats::ops_per_sec)};
}

void print_row(std::string_view scenario, const std::string& book, const Stats& s) {
    std::printf("| %-30.*s | %-22s | %7.0f | %7.0f | %8.0f | %9.0f | %7.1f | %11.0f |\n",
                static_cast<int>(scenario.size()), scenario.data(), book.c_str(), s.p50, s.p99, s.p999, s.max, s.mean,
                s.ops_per_sec);
    std::fflush(stdout);
}

void print_header(std::string_view title) {
    std::printf("\n### %.*s\n\n", static_cast<int>(title.size()), title.data());
    std::printf("| %-30s | %-22s | %7s | %7s | %8s | %9s | %7s | %11s |\n", "scenario", "book", "p50 ns", "p99 ns",
                "p99.9 ns", "max ns", "mean ns", "ops/s");
    std::printf("|%s|%s|%s|%s|%s|%s|%s|%s|\n", std::string(32, '-').c_str(), std::string(24, '-').c_str(),
                std::string(9, '-').c_str(), std::string(9, '-').c_str(), std::string(10, '-').c_str(),
                std::string(11, '-').c_str(), std::string(9, '-').c_str(), std::string(13, '-').c_str());
}

// ---- a book of a known shape --------------------------------------------------------

// Records what the engine reports so undo steps can restore the book.
struct BenchSink {
    std::vector<OrderId>* filled = nullptr;  // resting orders fully filled
    std::uint64_t events = 0;
    void on_trade(const Trade&) noexcept { ++events; }
    void on_fully_filled(const OrderFullyFilled& f) noexcept {
        ++events;
        if (filled) filled->push_back(f.id);
    }
    void on_partially_filled(const OrderPartiallyFilled&) noexcept { ++events; }
};

constexpr std::int64_t kMid = 1'000'000;
constexpr Quantity kQty = 10;

// A two-sided book: `orders` resting orders split evenly across `levels`
// price levels per side, one tick apart around a one-tick-wide gap at kMid.
// Level 0 is the best level on each side.
class ShapedBook {
public:
    ShapedBook(std::size_t orders, std::size_t levels)
        : levels_(levels),
          per_level_(std::max<std::size_t>(1, orders / (2 * levels))),
          engine_(sink_, BookConfig{.reserve_orders = orders * 2 + 1024, .reserve_levels = levels * 2 + 64}) {
        // Ids follow a fixed pattern (see id_at) so any id's level is computable.
        for (std::size_t k = 0; k < per_level_; ++k)
            for (std::size_t l = 0; l < levels_; ++l)
                for (Side side : {Side::Buy, Side::Sell})
                    place(AddOrder{id_at(side, l, k), side, kQty, price_of(side, l)});
        next_id_ = id_at(Side::Sell, levels_ - 1, per_level_ - 1) + 1;
        first_fresh_id_ = next_id_;
    }

    static Price price_of(Side side, std::size_t level) {
        const auto offset = static_cast<std::int64_t>(level) + 1;
        return Price::from_units(side == Side::Buy ? kMid - offset : kMid + offset);
    }

    // The k-th original order at `level` on `side` (ids never change level).
    OrderId id_at(Side side, std::size_t level, std::size_t k) const {
        return 1 + ((k % per_level_) * levels_ + level) * 2 + static_cast<std::size_t>(side);
    }
    // True for the orders the book was built with (not aggressors added later).
    bool is_original(OrderId id) const { return id < first_fresh_id_; }
    Price original_price(OrderId id) const {
        const Side side = (id - 1) % 2 == 0 ? Side::Buy : Side::Sell;
        return price_of(side, ((id - 1) / 2) % levels_);
    }

    void place(const AddOrder& a) {
        if (engine_.add(a) != Reject::None) std::abort();
    }
    OrderId fresh_id() { return next_id_++; }

    std::size_t levels() const { return levels_; }
    std::size_t per_level() const { return per_level_; }
    MatchingEngine<BenchSink>& engine() { return engine_; }
    BenchSink& sink() { return sink_; }

private:
    std::size_t levels_;
    std::size_t per_level_;
    BenchSink sink_;
    MatchingEngine<BenchSink> engine_;
    OrderId next_id_ = 1;
    OrderId first_fresh_id_ = 1;
};

// Runs `iterations` of: untimed setup, timed op, untimed undo.
template <class Setup, class Op, class Undo>
std::vector<double> sample(const Timer& timer, std::size_t iterations, Setup&& setup, Op&& op, Undo&& undo) {
    std::vector<double> ns;
    ns.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        setup(i);
        const std::uint64_t t0 = Timer::now();
        op(i);
        const std::uint64_t t1 = Timer::now();
        undo(i);
        ns.push_back(timer.to_ns(t1 - t0));
    }
    return ns;
}

struct Options {
    std::vector<std::size_t> sizes = {1'000, 10'000, 100'000, 1'000'000};
    std::size_t repeat = 5;
    std::size_t iterations = 100'000;
    std::uint64_t seed = 20260925;
    bool quick = false;
};

// ---- per-operation scenarios ----------------------------------------------------------

void run_book_scenarios(const Timer& timer, const Options& opt) {
    print_header("Per-operation latency (engine only, one request per sample)");
    for (std::size_t n : opt.sizes) {
        const std::size_t levels = std::clamp<std::size_t>(n / 20, 1, 1000);  // ~10 orders per level per side
        const std::string book = std::to_string(n) + " / " + std::to_string(levels) + " lvls";
        auto run = [&](std::string_view name, std::size_t iterations, auto&& body) {
            std::vector<Stats> runs;
            for (std::size_t r = 0; r < opt.repeat; ++r) {
                ShapedBook b(n, levels);
                test::Rng rng(opt.seed + r);
                std::vector<double> ns = body(b, rng, iterations);
                runs.push_back(summarize(ns));
            }
            print_row(name, book, median_of(runs));
        };
        const std::size_t iters = opt.iterations;

        run("add, rests at best (no match)", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            OrderId id = 0;
            return sample(
                timer, it, [&](std::size_t) { id = b.fresh_id(); },
                [&](std::size_t) { keep(b.engine().add({id, Side::Buy, kQty, ShapedBook::price_of(Side::Buy, 0)})); },
                [&](std::size_t) { keep(b.engine().cancel({id})); });
        });
        run("add, new best level (no match)", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            OrderId id = 0;
            return sample(
                timer, it, [&](std::size_t) { id = b.fresh_id(); },
                [&](std::size_t) { keep(b.engine().add({id, Side::Buy, kQty, Price::from_units(kMid)})); },
                [&](std::size_t) { keep(b.engine().cancel({id})); });
        });
        run("add, new level deepest", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            OrderId id = 0;
            const Price deepest = ShapedBook::price_of(Side::Buy, b.levels());
            return sample(
                timer, it, [&](std::size_t) { id = b.fresh_id(); },
                [&](std::size_t) { keep(b.engine().add({id, Side::Buy, kQty, deepest})); },
                [&](std::size_t) { keep(b.engine().cancel({id})); });
        });
        run("add, fully fills 1 order", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            std::vector<OrderId> filled;
            filled.reserve(4);
            b.sink().filled = &filled;
            return sample(
                timer, it, [&](std::size_t) { filled.clear(); },
                [&](std::size_t) {
                    keep(b.engine().add({b.fresh_id(), Side::Buy, kQty, ShapedBook::price_of(Side::Sell, 0)}));
                },
                [&](std::size_t) {
                    for (OrderId id : filled)
                        if (b.is_original(id)) b.place({id, Side::Sell, kQty, b.original_price(id)});
                });
        });
        for (std::size_t k : {std::size_t{1}, std::size_t{10}, std::size_t{100}}) {
            if (k > levels) continue;
            const std::size_t sweep_iters = std::max<std::size_t>(iters / (k * 10), 1000);
            run("add, sweeps " + std::to_string(k) + " level(s)", sweep_iters,
                [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
                    std::vector<OrderId> filled;
                    filled.reserve(k * b.per_level() + 4);
                    b.sink().filled = &filled;
                    const Quantity qty = static_cast<Quantity>(k * b.per_level()) * kQty;
                    const Price limit = ShapedBook::price_of(Side::Sell, k - 1);
                    return sample(
                        timer, it, [&](std::size_t) { filled.clear(); },
                        [&](std::size_t) { keep(b.engine().add({b.fresh_id(), Side::Buy, qty, limit})); },
                        [&](std::size_t) {
                            // Restore level by level, preserving the original layout.
                            for (OrderId id : filled)
                                if (b.is_original(id)) b.place({id, Side::Sell, kQty, b.original_price(id)});
                        });
                });
        }
        auto cancel_at = [&](std::string_view name, auto level_of) {
            run(name, iters, [&](ShapedBook& b, test::Rng& rng, std::size_t it) {
                OrderId victim = 0;
                Price price;
                return sample(
                    timer, it,
                    [&](std::size_t) {
                        const std::size_t level = level_of(b, rng);
                        victim = b.id_at(Side::Buy, level, static_cast<std::size_t>(rng.below(b.per_level())));
                        price = ShapedBook::price_of(Side::Buy, level);
                    },
                    [&](std::size_t) { keep(b.engine().cancel({victim})); },
                    [&](std::size_t) { b.place({victim, Side::Buy, kQty, price}); });
            });
        };
        cancel_at("cancel, at best level", [](ShapedBook&, test::Rng&) { return std::size_t{0}; });
        cancel_at("cancel, deepest level", [](ShapedBook& b, test::Rng&) { return b.levels() - 1; });
        cancel_at("cancel, random level",
                  [](ShapedBook& b, test::Rng& rng) { return static_cast<std::size_t>(rng.below(b.levels())); });

        run("cancel, empties best level", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            OrderId id = 0;
            return sample(
                timer, it,
                [&](std::size_t) {
                    id = b.fresh_id();
                    b.place({id, Side::Buy, kQty, Price::from_units(kMid)});
                },
                [&](std::size_t) { keep(b.engine().cancel({id})); }, [](std::size_t) {});
        });
        run("cancel, empties deepest level", iters, [&](ShapedBook& b, test::Rng& /*rng*/, std::size_t it) {
            OrderId id = 0;
            const Price deepest = ShapedBook::price_of(Side::Buy, b.levels());
            return sample(
                timer, it,
                [&](std::size_t) {
                    id = b.fresh_id();
                    b.place({id, Side::Buy, kQty, deepest});
                },
                [&](std::size_t) { keep(b.engine().cancel({id})); }, [](std::size_t) {});
        });
    }
}

// Worst case for the sorted-vector design: many price levels, inserting and
// removing at the far end (a memmove of every level).
void run_deep_book_scenarios(const Timer& timer, const Options& opt) {
    print_header("Worst case: level insert/erase at the far end of a deep book (1 order per level)");
    for (std::size_t levels : {std::size_t{100}, std::size_t{1'000}, std::size_t{10'000}, std::size_t{100'000}}) {
        if (opt.quick && levels > 10'000) continue;
        std::vector<Stats> add_runs, cancel_runs;
        for (std::size_t r = 0; r < opt.repeat; ++r) {
            ShapedBook b(levels * 2, levels);
            const Price deepest = ShapedBook::price_of(Side::Buy, levels);
            std::vector<OrderId> ids;
            std::vector<double> add_ns, cancel_ns;
            const std::size_t iters = std::min<std::size_t>(opt.iterations, 20'000);
            for (std::size_t i = 0; i < iters; ++i) {
                const OrderId id = b.fresh_id();
                std::uint64_t t0 = Timer::now();
                keep(b.engine().add({id, Side::Buy, kQty, deepest}));
                std::uint64_t t1 = Timer::now();
                add_ns.push_back(timer.to_ns(t1 - t0));
                t0 = Timer::now();
                keep(b.engine().cancel({id}));
                t1 = Timer::now();
                cancel_ns.push_back(timer.to_ns(t1 - t0));
            }
            add_runs.push_back(summarize(add_ns));
            cancel_runs.push_back(summarize(cancel_ns));
        }
        print_row("add, new level deepest", std::to_string(levels) + " lvls", median_of(add_runs));
        print_row("cancel, empties deepest level", std::to_string(levels) + " lvls", median_of(cancel_runs));
    }
}

// Growth past the reservation: the one place the hot path allocates.
void run_growth_scenario(const Timer& timer, const Options& opt) {
    print_header("Growth past a tiny reservation (1M resting orders added to an engine reserved for 16)");
    std::vector<Stats> runs;
    for (std::size_t r = 0; r < opt.repeat; ++r) {
        BenchSink sink;
        MatchingEngine<BenchSink> engine(sink, BookConfig{.reserve_orders = 16, .reserve_levels = 4});
        std::vector<double> ns;
        const std::size_t count = opt.quick ? 100'000 : 1'000'000;
        ns.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto price = Price::from_units(static_cast<std::int64_t>(1000 + i % 1000));
            const std::uint64_t t0 = Timer::now();
            keep(engine.add({i + 1, Side::Buy, 1, price}));
            ns.push_back(timer.to_ns(Timer::now() - t0));
        }
        runs.push_back(summarize(ns));
    }
    print_row("add with growth", "0 -> 1M", median_of(runs));
}

// ---- throughput ---------------------------------------------------------------------

std::string to_csv(const test::Request& request) {
    if (const auto* a = std::get_if<AddOrder>(&request))
        return "0," + std::to_string(a->id) + "," + std::to_string(static_cast<int>(a->side)) + "," +
               std::to_string(a->qty) + "," + to_string(a->price) + "\n";
    return "1," + std::to_string(std::get<CancelOrder>(request).id) + "\n";
}

class MemoryReader final : public ByteReader {
public:
    explicit MemoryReader(std::string_view data) : data_(data) {}
    long read(char* buffer, std::size_t capacity, int&) noexcept override {
        const std::size_t n = std::min(capacity, data_.size() - pos_);
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<long>(n);
    }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

class CountingWriter final : public ByteWriter {
public:
    long write(const char*, std::size_t size, int&) noexcept override {
        bytes += size;
        return static_cast<long>(size);
    }
    std::size_t bytes = 0;
};

void run_throughput(const Timer& timer, const Options& opt) {
    std::printf("\n### Throughput (batch timing; mean ns per message)\n\n");
    std::printf("| %-44s | %10s | %12s | %10s |\n", "scenario", "messages", "msgs/s", "ns/msg");
    std::printf("|%s|%s|%s|%s|\n", std::string(46, '-').c_str(), std::string(12, '-').c_str(),
                std::string(14, '-').c_str(), std::string(12, '-').c_str());

    const std::size_t count = opt.quick ? 500'000 : 5'000'000;
    for (test::Profile profile : {test::Profile::Tight, test::Profile::Mixed}) {
        std::vector<test::Request> requests;
        std::string csv;
        std::vector<std::string_view> lines;
        {
            test::RequestGenerator gen(profile, opt.seed);
            requests.reserve(count);
            for (std::size_t i = 0; i < count; ++i) requests.push_back(gen.next());
            for (const auto& r : requests) csv += to_csv(r);
            std::size_t start = 0;
            for (std::size_t i = 0; i < csv.size(); ++i)
                if (csv[i] == '\n') {
                    lines.emplace_back(csv.data() + start, i - start);
                    start = i + 1;
                }
        }
        const std::string tag = " (" + std::string(name(profile)) + ")";
        auto report = [&](const std::string& scenario, std::uint64_t ticks) {
            const double ns = timer.to_ns(ticks) / static_cast<double>(count);
            std::printf("| %-44s | %10zu | %12.0f | %10.1f |\n", scenario.c_str(), count, 1e9 / ns, ns);
            std::fflush(stdout);
        };
        auto best_of = [&](auto&& body) {
            std::uint64_t best = UINT64_MAX;
            for (std::size_t r = 0; r < opt.repeat; ++r) best = std::min(best, body());
            return best;
        };

        report("parse only" + tag, best_of([&] {
                   const std::uint64_t t0 = Timer::now();
                   for (std::string_view line : lines) keep(parse_request(line));
                   return Timer::now() - t0;
               }));
        report("engine only, pre-parsed" + tag, best_of([&] {
                   BenchSink sink;
                   MatchingEngine<BenchSink> engine(sink);
                   const std::uint64_t t0 = Timer::now();
                   for (const auto& r : requests) {
                       if (const auto* a = std::get_if<AddOrder>(&r))
                           keep(engine.add(*a));
                       else
                           keep(engine.cancel(std::get<CancelOrder>(r)));
                   }
                   return Timer::now() - t0;
               }));
        report("format only (1 trade + 2 fills per msg)" + tag, best_of([&] {
                   CountingWriter sink;
                   BufferedWriter out(sink);
                   EventWriter writer(out);
                   const std::uint64_t t0 = Timer::now();
                   for (std::size_t i = 0; i < count; ++i) {
                       writer.on_trade({i + 1, Price::from_raw(static_cast<std::int64_t>(i) * 25'000'000)});
                       writer.on_partially_filled({i + 7, i + 3});
                       writer.on_fully_filled({i + 9});
                   }
                   out.flush();
                   return Timer::now() - t0;
               }));
        report("end to end: run_app, in memory" + tag, best_of([&] {
                   MemoryReader in(csv);
                   CountingWriter out, err;
                   const std::uint64_t t0 = Timer::now();
                   keep(run_app({}, in, out, err));
                   return Timer::now() - t0;
               }));
    }
}

// Realistic mixed flow: per-request latency over a generated stream.
void run_workload_latency(const Timer& timer, const Options& opt) {
    print_header("Per-request latency on generated order flow (engine only)");
    for (test::Profile profile : {test::Profile::Tight, test::Profile::Mixed, test::Profile::Sweep}) {
        std::vector<Stats> runs;
        const std::size_t count = opt.quick ? 200'000 : 2'000'000;
        test::RequestGenerator gen(profile, opt.seed);
        std::vector<test::Request> requests;
        requests.reserve(count);
        for (std::size_t i = 0; i < count; ++i) requests.push_back(gen.next());
        for (std::size_t r = 0; r < opt.repeat; ++r) {
            BenchSink sink;
            MatchingEngine<BenchSink> engine(sink);
            std::vector<double> ns;
            ns.reserve(count);
            for (const auto& req : requests) {
                const std::uint64_t t0 = Timer::now();
                if (const auto* a = std::get_if<AddOrder>(&req))
                    keep(engine.add(*a));
                else
                    keep(engine.cancel(std::get<CancelOrder>(req)));
                ns.push_back(timer.to_ns(Timer::now() - t0));
            }
            runs.push_back(summarize(ns));
        }
        print_row(std::string(name(profile)) + " profile", "generated", median_of(runs));
    }
}

std::string cpu_model() {
#if defined(__APPLE__)
    char buf[256] = {};
    std::size_t len = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string implementer, part;
    for (std::string line; std::getline(cpuinfo, line);) {
        const std::string value = line.find(':') == std::string::npos ? "" : line.substr(line.find(':') + 2);
        if (line.rfind("model name", 0) == 0) return value;
        if (line.rfind("CPU implementer", 0) == 0) implementer = value;
        if (line.rfind("CPU part", 0) == 0) part = value;
    }
    // Arm Linux has no model name; report the implementer/part IDs instead.
    if (!implementer.empty()) return "arm64 (implementer " + implementer + ", part " + part + ")";
#endif
    return "unknown";
}

Options parse_options(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string_view(argv[++i]) : std::string_view(); };
        if (arg == "--quick") {
            opt.quick = true;
            opt.repeat = 3;
            opt.iterations = 20'000;
            opt.sizes = {1'000, 100'000};
        } else if (arg == "--repeat") {
            opt.repeat = std::stoul(std::string(next()));
        } else if (arg == "--seed") {
            opt.seed = std::stoull(std::string(next()));
        } else if (arg == "--sizes") {
            opt.sizes.clear();
            std::string list(next());
            for (std::size_t pos = 0; pos < list.size();) {
                const std::size_t comma = std::min(list.find(',', pos), list.size());
                opt.sizes.push_back(std::stoul(list.substr(pos, comma - pos)));
                pos = comma + 1;
            }
        } else {
            std::fprintf(stderr, "usage: matcher_bench [--quick] [--sizes a,b,...] [--repeat R] [--seed S]\n");
            std::exit(2);
        }
    }
    return opt;
}

}  // namespace
}  // namespace matcher::bench

int main(int argc, char** argv) {
    using namespace matcher::bench;
    const Options opt = parse_options(argc, argv);
    const Timer timer;

    std::printf("## Benchmark results\n\n");
    std::printf("- CPU: %s\n", cpu_model().c_str());
    std::printf("- Compiler: %s\n", __VERSION__);
    std::printf("- Flags: %s\n", MATCHER_BENCH_FLAGS);
    std::printf("- Timer: %s, %.3f ns/tick, resolution ~%.1f ns\n", std::string(Timer::source()).c_str(),
                timer.ns_per_tick(), timer.resolution_ns());
    std::printf("- Seed: %llu, repeats: %zu (median of runs reported)%s\n", static_cast<unsigned long long>(opt.seed),
                opt.repeat, opt.quick ? ", QUICK MODE" : "");
    std::printf(
        "- Latency percentiles include ~one timer read of overhead; values below the timer\n"
        "  resolution are quantized. Batch throughput numbers are not affected.\n");

    run_book_scenarios(timer, opt);
    run_deep_book_scenarios(timer, opt);
    run_growth_scenario(timer, opt);
    run_workload_latency(timer, opt);
    run_throughput(timer, opt);
    return 0;
}
