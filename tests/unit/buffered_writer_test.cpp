#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "matcher/buffered_writer.h"
#include "support/fake_io.h"

namespace matcher {
namespace {

using test::RecordingWriter;

TEST(BufferedWriter, BuffersUntilFlush) {
    RecordingWriter sink;
    BufferedWriter out(sink, 64);
    out.append("hello ");
    out.append("world\n");
    EXPECT_EQ(sink.data(), "");
    EXPECT_EQ(out.buffered(), 12u);
    out.flush();
    EXPECT_EQ(sink.data(), "hello world\n");
    EXPECT_EQ(out.buffered(), 0u);
    EXPECT_EQ(sink.calls(), 1u);
}

TEST(BufferedWriter, FlushOfAnEmptyBufferWritesNothing) {
    RecordingWriter sink;
    BufferedWriter out(sink, 64);
    out.flush();
    EXPECT_EQ(sink.calls(), 0u);
}

// @spec OUT-FLUSH-002
TEST(BufferedWriter, ReserveFlushesWhenTheLineWouldNotFit) {
    RecordingWriter sink;
    BufferedWriter out(sink, 16);
    out.append("0123456789");
    char* p = out.reserve(8);  // 10 + 8 > 16
    EXPECT_EQ(sink.data(), "0123456789");
    std::memcpy(p, "abcdefgh", 8);
    out.commit(p + 8);
    out.flush();
    EXPECT_EQ(sink.data(), "0123456789abcdefgh");
}

TEST(BufferedWriter, AppendAcceptsDataLargerThanTheBuffer) {
    RecordingWriter sink;
    BufferedWriter out(sink, 8);
    const std::string big(1000, 'q');
    out.append("<");
    out.append(big);
    out.append(">");
    out.flush();
    EXPECT_EQ(sink.data(), "<" + big + ">");
}

// @spec OUT-ERR-001
TEST(BufferedWriter, CompletesPartialWrites) {
    RecordingWriter sink;
    sink.limit_per_call(3);
    BufferedWriter out(sink, 64);
    out.append("partial writes are normal on pipes\n");
    out.flush();
    EXPECT_EQ(sink.data(), "partial writes are normal on pipes\n");
    EXPECT_FALSE(out.failed());
}

// @spec OUT-ERR-001
TEST(BufferedWriter, RetriesEintr) {
    RecordingWriter sink;
    sink.interrupt_every_write();
    sink.limit_per_call(4);
    BufferedWriter out(sink, 64);
    out.append("interrupted\n");
    out.flush();
    EXPECT_EQ(sink.data(), "interrupted\n");
    EXPECT_FALSE(out.failed());
}

// @spec OUT-ERR-002
TEST(BufferedWriter, FirstErrorIsStickyAndLaterOutputIsDiscarded) {
    RecordingWriter sink;
    sink.fail_after(5, EPIPE);
    BufferedWriter out(sink, 8);
    out.append("12345678");
    out.flush();
    EXPECT_TRUE(out.failed());
    EXPECT_EQ(out.error(), EPIPE);
    const std::size_t calls = sink.calls();
    out.append("more output");
    char* p = out.reserve(4);  // still returns usable memory
    ASSERT_NE(p, nullptr);
    out.commit(p);
    out.flush();
    EXPECT_EQ(sink.calls(), calls) << "no writes after failure";
    EXPECT_EQ(sink.data(), "12345");
}

TEST(BufferedWriter, DestructorFlushes) {
    RecordingWriter sink;
    {
        BufferedWriter out(sink, 64);
        out.append("bye\n");
    }
    EXPECT_EQ(sink.data(), "bye\n");
}

}  // namespace
}  // namespace matcher
