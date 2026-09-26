#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "matcher/error_reporter.h"
#include "matcher/request_parser.h"
#include "support/fake_io.h"

namespace matcher {
namespace {

using test::RecordingWriter;

class ErrorReporterTest : public ::testing::Test {
protected:
    // Parses `line` (which must fail) and returns the diagnostic text.
    std::string diagnose(std::string_view line, std::uint64_t number = 1) {
        const ParseResult r = parse_request(line);
        EXPECT_TRUE(std::holds_alternative<ParseError>(r)) << line;
        if (!std::holds_alternative<ParseError>(r)) return {};
        reporter_.parse_error(number, std::get<ParseError>(r), clean_line(line));
        return take();
    }
    std::string take() {
        err_.flush();
        std::string s = sink_.data().substr(taken_);
        taken_ = sink_.data().size();
        return s;
    }
    RecordingWriter sink_;
    BufferedWriter err_{sink_, 4096};
    ErrorReporter reporter_{err_};
    std::size_t taken_ = 0;
};

// @spec PROTO-APP-001, PROTO-PARSE-004
TEST_F(ErrorReporterTest, UnknownMessageTypeUsesTheBriefsWording) {
    EXPECT_EQ(diagnose("BADMESSAGE                // An erroneous input", 6),
              "line 6: Unknown message type: BADMESSAGE\n");
}

// @spec PROTO-APP-001, PROTO-PARSE-005, PROTO-PARSE-006, PROTO-PARSE-007, PROTO-PARSE-008, PROTO-PARSE-009
TEST_F(ErrorReporterTest, EveryParseErrorHasASpecificReason) {
    EXPECT_EQ(diagnose("0,1,0,9"), "line 1: AddOrderRequest expects 5 fields, got 4: 0,1,0,9\n");
    EXPECT_EQ(diagnose("1,1,2"), "line 1: CancelOrderRequest expects 2 fields, got 3: 1,1,2\n");
    EXPECT_EQ(diagnose("1,abc"), "line 1: invalid orderid 'abc': 1,abc\n");
    EXPECT_EQ(diagnose("1,0"), "line 1: orderid must be positive: 1,0\n");
    EXPECT_EQ(diagnose("1,18446744073709551616"), "line 1: orderid out of range: 1,18446744073709551616\n");
    EXPECT_EQ(diagnose("0,1,2,9,1000"), "line 1: invalid side '2' (expected 0=Buy or 1=Sell): 0,1,2,9,1000\n");
    EXPECT_EQ(diagnose("0,1,0,-5,1000"), "line 1: invalid quantity '-5': 0,1,0,-5,1000\n");
    EXPECT_EQ(diagnose("0,1,0,0,1000"), "line 1: quantity must be positive: 0,1,0,0,1000\n");
    EXPECT_EQ(diagnose("0,1,0,99999999999999999999,1"),
              "line 1: quantity out of range: 0,1,0,99999999999999999999,1\n");
    EXPECT_EQ(diagnose("0,1,0,9,1e3"), "line 1: invalid price '1e3': malformed: 0,1,0,9,1e3\n");
    EXPECT_EQ(diagnose("0,1,0,9,1.000000001"),
              "line 1: invalid price '1.000000001': more than 8 decimal places: 0,1,0,9,1.000000001\n");
    EXPECT_EQ(diagnose("0,1,0,9,99999999999"),
              "line 1: invalid price '99999999999': out of range: 0,1,0,9,99999999999\n");
}

// @spec OUT-DIAG-001
TEST_F(ErrorReporterTest, EngineRejections) {
    reporter_.reject(3, Reject::DuplicateOrderId, 123, "0,123,0,9,1000");
    EXPECT_EQ(take(), "line 3: duplicate orderid 123 (an order with this id is still resting): 0,123,0,9,1000\n");
    reporter_.reject(4, Reject::UnknownOrderId, 77, "1,77");
    EXPECT_EQ(take(), "line 4: cannot cancel orderid 77: no resting order with this id: 1,77\n");
    reporter_.reject(5, Reject::CapacityExceeded, 9, "0,9,0,1,1");
    EXPECT_EQ(take(), "line 5: order rejected: order book capacity exhausted: 0,9,0,1,1\n");
}

// @spec PROTO-APP-002
TEST_F(ErrorReporterTest, ExcerptsAreEscapedAndTruncated) {
    EXPECT_EQ(diagnose(std::string_view("\x01\x7f\xff\0A", 5)),
              "line 1: Unknown message type: \\x01\\x7F\\xFF\\x00A\n");
    const std::string long_garbage(200, 'g');
    EXPECT_EQ(diagnose(long_garbage), "line 1: Unknown message type: " + std::string(80, 'g') + "\xE2\x80\xA6\n");
    const std::string exactly_80(80, 'h');
    EXPECT_EQ(diagnose(exactly_80), "line 1: Unknown message type: " + exactly_80 + "\n");
    // Offending fields are escaped too.
    EXPECT_EQ(diagnose("1,\x1b[31m"), "line 1: invalid orderid '\\x1B[31m': 1,\\x1B[31m\n");
}

// @spec PROTO-APP-002, PROTO-READ-003
TEST_F(ErrorReporterTest, LineTooLongShowsTheRawPrefix) {
    reporter_.line_too_long(9, std::string(80, 'x'));
    EXPECT_EQ(take(), "line 9: line exceeds 4096 bytes: " + std::string(80, 'x') + "\xE2\x80\xA6\n");
    reporter_.line_too_long(10, "a\tb");
    EXPECT_EQ(take(), "line 10: line exceeds 4096 bytes: a\\x09b\xE2\x80\xA6\n");
}

TEST_F(ErrorReporterTest, MessagesAndCount) {
    reporter_.message("stdout write failed: Broken pipe");
    EXPECT_EQ(take(), "stdout write failed: Broken pipe\n");
    reporter_.reject(1, Reject::UnknownOrderId, 1, "1,1");
    (void)take();
    EXPECT_EQ(reporter_.count(), 2u);
}

TEST(RejectText, EveryCodeHasAName) {
    EXPECT_EQ(to_string(Reject::None), "none");
    EXPECT_EQ(to_string(Reject::InvalidOrderId), "invalid orderid");
    EXPECT_EQ(to_string(Reject::InvalidQuantity), "invalid quantity");
    EXPECT_EQ(to_string(Reject::DuplicateOrderId), "duplicate orderid");
    EXPECT_EQ(to_string(Reject::UnknownOrderId), "unknown orderid");
    EXPECT_EQ(to_string(Reject::CapacityExceeded), "capacity exceeded");
}

// A writer smaller than the largest diagnostic still gets complete, correct text.
// @spec PROTO-APP-002
TEST(ErrorReporter, WorksWithAWriterSmallerThanOneDiagnostic) {
    RecordingWriter sink;
    BufferedWriter err(sink, 64);
    ErrorReporter reporter(err);
    const std::string garbage(200, '\x01');
    const ParseResult r = parse_request(garbage);
    reporter.parse_error(1, std::get<ParseError>(r), clean_line(garbage));
    reporter.reject(2, Reject::UnknownOrderId, 77, "1,77");
    err.flush();
    std::string expected = "line 1: Unknown message type: ";
    for (int i = 0; i < 80; ++i) expected += "\\x01";
    expected += "\xE2\x80\xA6\nline 2: cannot cancel orderid 77: no resting order with this id: 1,77\n";
    EXPECT_EQ(sink.data(), expected);
}

}  // namespace
}  // namespace matcher
