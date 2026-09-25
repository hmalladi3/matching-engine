// Replaces global operator new/delete to (a) count heap allocations and (b)
// inject allocation failure. Lives in its own binary because the replacement
// is process-wide.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <variant>
#include <vector>

#include "matcher/app.h"
#include "matcher/error_reporter.h"
#include "matcher/event_writer.h"
#include "matcher/line_reader.h"
#include "matcher/matching_engine.h"
#include "matcher/request_parser.h"
#include "support/fake_io.h"
#include "support/request_generator.h"

namespace {
bool g_counting = false;
bool g_failing = false;
std::size_t g_allocations = 0;

void* allocate(std::size_t size) {
    if (g_failing) throw std::bad_alloc();
    if (g_counting) ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}

// Counts allocations made while alive.
struct CountAllocations {
    CountAllocations() {
        g_allocations = 0;
        g_counting = true;
    }
    ~CountAllocations() { g_counting = false; }
    std::size_t count() const { return g_allocations; }
};

// Makes every allocation fail while alive.
struct FailAllocations {
    FailAllocations() { g_failing = true; }
    ~FailAllocations() { g_failing = false; }
};
}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace matcher {
namespace {

// Accepts and discards all output without allocating.
class DiscardWriter final : public ByteWriter {
public:
    long write(const char*, std::size_t size, int&) noexcept override { return static_cast<long>(size); }
};

// Records output into a fixed array (no allocation).
class FixedWriter final : public ByteWriter {
public:
    long write(const char* data, std::size_t size, int&) noexcept override {
        const std::size_t n = std::min(size, sizeof(buf_) - len_);
        std::memcpy(buf_ + len_, data, n);
        len_ += n;
        return static_cast<long>(size);
    }
    std::string_view text() const { return {buf_, len_}; }

private:
    char buf_[4096] = {};
    std::size_t len_ = 0;
};

std::string to_csv(const test::Request& request) {
    if (const auto* a = std::get_if<AddOrder>(&request))
        return "0," + std::to_string(a->id) + "," + std::to_string(static_cast<int>(a->side)) + "," +
               std::to_string(a->qty) + "," + to_string(a->price) + "\n";
    return "1," + std::to_string(std::get<CancelOrder>(request).id) + "\n";
}

// The whole hot path (read → parse → match → format, plus diagnostics) with
// default reservations: no heap allocation after warm-up.
// @spec BOOK-MEM-002, PROTO-READ-006, DLV-TEST-006
TEST(NoAlloc, SteadyStatePipelineNeverAllocates) {
    // Prebuild the input (allocates; not counted). Tight keeps levels and live
    // orders far inside the default reservation.
    std::string input;
    test::RequestGenerator generator(test::Profile::Tight, 42);
    for (int i = 0; i < 1'000'000; ++i) input += to_csv(generator.next());
    input += "BADMESSAGE\n0,1,0,0,1\n1,999999999999\n";  // diagnostics path too

    test::ScriptedReader source(input, 1 << 16);
    LineReader reader(source);
    DiscardWriter out_sink, err_sink;
    BufferedWriter out(out_sink), err(err_sink);
    EventWriter events(out);
    ErrorReporter errors(err);
    MatchingEngine<EventWriter> engine(events);  // default BookConfig

    std::uint64_t lines = 0;
    auto process_one = [&]() -> bool {
        const LineReader::Result r = reader.next();
        if (r.status != LineReader::Status::Line) return false;
        ++lines;
        const ParseResult parsed = parse_request(r.text);
        if (const auto* add = std::get_if<AddOrder>(&parsed)) {
            if (const Reject rej = engine.add(*add); rej != Reject::None)
                errors.reject(reader.line_number(), rej, add->id, r.text);
        } else if (const auto* cancel = std::get_if<CancelOrder>(&parsed)) {
            if (const Reject rej = engine.cancel(*cancel); rej != Reject::None)
                errors.reject(reader.line_number(), rej, cancel->id, r.text);
        } else if (const auto* error = std::get_if<ParseError>(&parsed)) {
            errors.parse_error(reader.line_number(), *error, clean_line(r.text));
        }
        return true;
    };

    for (int i = 0; i < 10'000; ++i) ASSERT_TRUE(process_one());  // warm-up
    std::size_t allocations = 0;
    {
        CountAllocations counter;
        while (process_one()) {
        }
        out.flush();
        err.flush();
        allocations = counter.count();
    }
    EXPECT_EQ(lines, 1'000'003u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_GT(errors.count(), 3u);
}

// Proves the counter works: exceeding the reservation must allocate.
// @spec BOOK-MEM-003
TEST(NoAlloc, GrowthBeyondTheReservationIsCounted) {
    NullSink sink;
    MatchingEngine<NullSink> engine(sink, BookConfig{16, 4, kMaxNodes});
    std::size_t allocations = 0;
    {
        CountAllocations counter;
        for (OrderId id = 1; id <= 1000; ++id)
            ASSERT_EQ(engine.add({id, Side::Buy, 1, Price::from_units(static_cast<std::int64_t>(id))}), Reject::None);
        allocations = counter.count();
    }
    EXPECT_GT(allocations, 0u);
}

// @spec MATCH-REJ-004, BOOK-MEM-004
TEST(NoAlloc, AllocationFailureDuringGrowthRejectsWithoutSideEffects) {
    NullSink sink;
    MatchingEngine<NullSink> engine(sink, BookConfig{2, 1, kMaxNodes});
    ASSERT_EQ(engine.add({1, Side::Sell, 5, Price::from_units(10)}), Reject::None);
    const auto before = engine.book().snapshot(Side::Sell);
    Reject verdict = Reject::None;
    {
        FailAllocations fail;
        // Needs a new level and nodes beyond the reservation.
        verdict = engine.add({2, Side::Sell, 5, Price::from_units(11)});
    }
    EXPECT_EQ(verdict, Reject::CapacityExceeded);
    EXPECT_EQ(engine.book().snapshot(Side::Sell), before);
    engine.book().check_invariants();
    EXPECT_EQ(engine.add({2, Side::Sell, 5, Price::from_units(11)}), Reject::None)
        << "recovers once memory is available";
}

// @spec PROTO-APP-006, PROTO-APP-003
TEST(NoAlloc, StartupAllocationFailureExitsWithStatusOne) {
    test::ScriptedReader in("0,1,0,1,1\n");
    FixedWriter out, err;
    const std::string_view args[] = {"--reserve", "1000"};
    int code = -1;
    {
        FailAllocations fail;
        code = run_app(args, in, out, err);
    }
    EXPECT_EQ(code, kExitIoFailure);
    EXPECT_EQ(err.text(), "cannot reserve memory for 1000 orders\n");
    EXPECT_EQ(out.text(), "");
    EXPECT_EQ(in.consumed(), 0u) << "must not read input";
}

}  // namespace
}  // namespace matcher
