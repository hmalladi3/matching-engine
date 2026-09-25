#include <gtest/gtest.h>

#include <limits>

#include "matcher/event_writer.h"
#include "support/fake_io.h"
#include "support/harness.h"

namespace matcher {
namespace {

using test::px;
using test::RecordingWriter;

class EventWriterTest : public ::testing::Test {
protected:
    std::string written() {
        out_.flush();
        return sink_.data();
    }
    RecordingWriter sink_;
    BufferedWriter out_{sink_, 256};
    EventWriter writer_{out_};
};

// @spec OUT-FMT-001
TEST_F(EventWriterTest, Trade) {
    writer_.on_trade({2, px("1025")});
    EXPECT_EQ(written(), "2,2,1025\n");
}

// @spec OUT-FMT-002
TEST_F(EventWriterTest, OrderFullyFilled) {
    writer_.on_fully_filled({1000005});
    EXPECT_EQ(written(), "3,1000005\n");
}

// @spec OUT-FMT-003
TEST_F(EventWriterTest, OrderPartiallyFilled) {
    writer_.on_partially_filled({1000008, 1});
    EXPECT_EQ(written(), "4,1000008,1\n");
}

// @spec OUT-FMT-004
TEST_F(EventWriterTest, ExtremeValuesAndPriceForms) {
    constexpr auto kMaxU64 = std::numeric_limits<std::uint64_t>::max();
    writer_.on_trade({kMaxU64, Price::from_raw(-Price::kMaxRaw)});
    writer_.on_partially_filled({kMaxU64, kMaxU64});
    writer_.on_fully_filled({1});
    writer_.on_trade({1, px("1025.50")});
    writer_.on_trade({1, px("-0.00000001")});
    writer_.on_trade({1, px("0")});
    EXPECT_EQ(written(),
              "2,18446744073709551615,-92233720368.54775807\n"
              "4,18446744073709551615,18446744073709551615\n"
              "3,1\n"
              "2,1,1025.5\n"
              "2,1,-0.00000001\n"
              "2,1,0\n");
}

TEST(EventWriter, LongestLineFitsTheReservation) {
    EXPECT_GE(EventWriter::kMaxLineBytes, 2 + 20 + 1 + Price::kMaxFormattedLen + 1);
    EXPECT_GE(EventWriter::kMaxLineBytes, 2 + 20 + 1 + 20 + 1);
}

TEST(EventWriter, ManyEventsThroughASmallBuffer) {
    RecordingWriter sink;
    BufferedWriter out(sink, 64);  // exactly one maximal line
    EventWriter writer(out);
    std::string expected;
    for (OrderId id = 1; id <= 1000; ++id) {
        writer.on_fully_filled({id});
        expected += "3," + std::to_string(id) + "\n";
    }
    out.flush();
    EXPECT_EQ(sink.data(), expected);
}

}  // namespace
}  // namespace matcher
