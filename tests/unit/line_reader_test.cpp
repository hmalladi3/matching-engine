#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

#include "matcher/line_reader.h"
#include "support/fake_io.h"

namespace matcher {
namespace {

using test::ScriptedReader;
using Status = LineReader::Status;

struct Item {
    Status status;
    std::string text;
    std::uint64_t line;
    friend bool operator==(const Item&, const Item&) = default;
};

std::ostream& operator<<(std::ostream& os, const Item& i) {
    return os << "{" << static_cast<int>(i.status) << ", \"" << i.text << "\", " << i.line << "}";
}

// Drains a reader into (status, text, line number) items, stopping at the
// first EndOfInput or ReadError.
std::vector<Item> drain(LineReader& reader) {
    std::vector<Item> items;
    for (;;) {
        const LineReader::Result r = reader.next();
        items.push_back({r.status, std::string(r.text), reader.line_number()});
        if (r.status == Status::EndOfInput || r.status == Status::ReadError) return items;
        if (items.size() > 100'000) {
            ADD_FAILURE() << "reader did not terminate";
            return items;
        }
    }
}

std::vector<Item> read_all(const std::string& input, std::size_t chunk, std::size_t buffer = 64,
                           std::size_t max_line = 16) {
    ScriptedReader source(input, chunk);
    LineReader reader(source, buffer, max_line);
    return drain(reader);
}

Item line(std::string text, std::uint64_t n) { return {Status::Line, std::move(text), n}; }
Item end(std::uint64_t n) { return {Status::EndOfInput, "", n}; }

TEST(LineReader, SplitsOnNewlineAndStripsOneTrailingCr) {
    EXPECT_EQ(read_all("a\nbb\r\nccc\r\r\n\n", 1024),
              (std::vector<Item>{line("a", 1), line("bb", 2), line("ccc\r", 3), line("", 4), end(4)}));
}

TEST(LineReader, ReturnsAFinalLineWithoutNewline) {
    EXPECT_EQ(read_all("a\nlast", 1024), (std::vector<Item>{line("a", 1), line("last", 2), end(2)}));
    EXPECT_EQ(read_all("last\r", 1024), (std::vector<Item>{line("last", 1), end(1)}));
}

TEST(LineReader, EmptyInputIsJustEndOfInput) { EXPECT_EQ(read_all("", 1024), (std::vector<Item>{end(0)})); }

TEST(LineReader, ResultIsIndependentOfHowReadsAreChunked) {
    std::string input;
    for (int i = 0; i < 200; ++i) input += std::string(static_cast<std::size_t>(i % 15), 'x') + (i % 3 ? "\n" : "\r\n");
    input += "tail";
    const auto expected = read_all(input, 1 << 20);
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 16u, 17u, 63u, 64u, 4096u})
        EXPECT_EQ(read_all(input, chunk), expected) << "chunk=" << chunk;
}

TEST(LineReader, TooLongLinesAreReportedOnceAndSkipped) {
    const std::string long_line(40, 'L');  // max_line is 16 in these tests
    for (std::size_t chunk : {1u, 5u, 1024u}) {
        const auto items = read_all("ok\n" + long_line + "\nnext\n", chunk);
        ASSERT_EQ(items.size(), 4u) << "chunk=" << chunk;
        EXPECT_EQ(items[0], line("ok", 1));
        EXPECT_EQ(items[1].status, Status::LineTooLong);
        EXPECT_EQ(items[1].line, 2u);
        EXPECT_EQ(items[1].text, long_line.substr(0, 40));  // prefix is at most 80 bytes
        EXPECT_EQ(items[2], line("next", 3));
        EXPECT_EQ(items[3], end(3));
    }
}

TEST(LineReader, BoundaryLengths) {
    const std::string at_limit(16, 'a');
    const std::string over_limit(17, 'b');
    const auto items = read_all(at_limit + "\n" + over_limit + "\n", 3);
    EXPECT_EQ(items[0], line(at_limit, 1));
    EXPECT_EQ(items[1].status, Status::LineTooLong);
    // A trailing \r does not count toward the limit's content but does count as a byte.
    EXPECT_EQ(read_all(at_limit + "\r\n", 3)[0].status, Status::LineTooLong);
}

TEST(LineReader, TooLongPrefixIsCappedAt80Bytes) {
    std::string huge(1'000'000, 'z');
    huge[0] = 'A';
    ScriptedReader source(huge + "\nafter\n", 4096);
    LineReader reader(source, 8192, 4096);
    const auto items = drain(reader);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].status, Status::LineTooLong);
    EXPECT_EQ(items[0].text.size(), LineReader::kTooLongPrefixBytes);
    EXPECT_EQ(items[0].text[0], 'A');
    EXPECT_EQ(items[1], line("after", 2));
}

TEST(LineReader, TooLongFinalLineWithoutNewline) {
    const auto items = read_all("ok\n" + std::string(50, 'x'), 7);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[1].status, Status::LineTooLong);
    EXPECT_EQ(items[2].status, Status::EndOfInput);
}

TEST(LineReader, BinaryBytesAndNulsPassThrough) {
    const std::string input("a\0b\xff\n\0\n", 7);
    const auto items = read_all(input, 2);
    EXPECT_EQ(items[0], line(std::string("a\0b\xff", 4), 1));
    EXPECT_EQ(items[1], line(std::string("\0", 1), 2));
}

TEST(LineReader, RetriesEintr) {
    ScriptedReader source("x\ny\n", 1);
    source.interrupt_every_read();
    LineReader reader(source, 64, 16);
    EXPECT_EQ(drain(reader), (std::vector<Item>{line("x", 1), line("y", 2), end(2)}));
}

TEST(LineReader, ReportsReadErrorsWithErrno) {
    ScriptedReader source("x\nyyy", 2);
    source.fail_at(3, EIO);
    LineReader reader(source, 64, 16);
    EXPECT_EQ(reader.next().status, Status::Line);
    const LineReader::Result r = reader.next();
    EXPECT_EQ(r.status, Status::ReadError);
    EXPECT_EQ(r.error, EIO);
}

// The reader calls read() only when it has no complete line buffered, which
// is what lets the app flush output exactly before it might block.
TEST(LineReader, ReadsOnlyWhenNoCompleteLineIsBuffered) {
    ScriptedReader source("a\nb\nc", 4);  // first read delivers "a\nb\n"
    LineReader reader(source, 64, 16);
    ASSERT_EQ(reader.next().text, "a");
    EXPECT_EQ(source.calls(), 1u);
    ASSERT_EQ(reader.next().text, "b");  // already buffered: no read
    EXPECT_EQ(source.calls(), 1u);
    ASSERT_EQ(reader.next().text, "c");  // needs input: reads "c", then end of input
    EXPECT_EQ(source.calls(), 3u);
}

TEST(LineReader, UsesOneReadPerBufferfulOfInput) {
    std::string input;
    for (int i = 0; i < 1000; ++i) input += "0,1,0,9,1000\n";
    ScriptedReader source(input);
    LineReader reader(source, 1 << 20, 4096);
    const auto items = drain(reader);
    EXPECT_EQ(items.size(), 1001u);
    EXPECT_LE(source.calls(), 3u);
}

// A failing read that reports no errno still yields a meaningful error.
TEST(LineReader, ReadErrorWithoutErrnoIsReportedAsEio) {
    ScriptedReader source("abc", 8);
    source.fail_at(0, 0);
    LineReader reader(source, 64, 16);
    const LineReader::Result r = reader.next();
    EXPECT_EQ(r.status, Status::ReadError);
    EXPECT_EQ(r.error, EIO);
}

}  // namespace
}  // namespace matcher
