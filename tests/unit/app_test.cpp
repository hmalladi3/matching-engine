#include <gtest/gtest.h>

#include <cerrno>
#include <functional>
#include <string>
#include <vector>

#include "matcher/app.h"
#include "support/fake_io.h"

namespace matcher {
namespace {

using test::RecordingWriter;
using test::ScriptedReader;

struct RunResult {
    int code;
    std::string out;
    std::string err;
};

RunResult run(const std::string& input, std::vector<std::string_view> args = {}, std::size_t chunk = 1 << 20) {
    ScriptedReader in(input, chunk);
    RecordingWriter out, err;
    const int code = run_app(args, in, out, err);
    return {code, out.data(), err.data()};
}

// The README's worked example (data/golden/worked_example.*).
const char* const kExampleInput =
    "0,101,1,5,100.25\n"
    "0,102,1,3,100.50\n"
    "0,103,0,4,99.75\n"
    "0,104,1,2,100.25\n"
    "0,105,0,6,99.50\n"
    "HELLO\n"
    "1,103\n"
    "0,106,0,6,100.25\n";

const char* const kExampleOutput =
    "2,5,100.25\n"
    "4,106,1\n"
    "3,101\n"
    "2,1,100.25\n"
    "3,106\n"
    "4,104,1\n";

TEST(App, WorkedExampleEndToEnd) {
    const RunResult r = run(kExampleInput);
    EXPECT_EQ(r.out, kExampleOutput);
    EXPECT_EQ(r.err, "line 6: Unknown message type: HELLO\n");
    EXPECT_EQ(r.code, kExitOk);
}

TEST(App, ByteAtATimeInputGivesIdenticalResults) {
    const RunResult r = run(kExampleInput, {}, 1);
    EXPECT_EQ(r.out, kExampleOutput);
    EXPECT_EQ(r.err, "line 6: Unknown message type: HELLO\n");
}

TEST(App, EngineRejectionsAreReportedWithLineNumbers) {
    const RunResult r =
        run("\n"            // 1: blank
            "// comment\n"  // 2: comment only
            "0,5,0,1,10\n"  // 3: rests
            "0,5,1,1,11\n"  // 4: duplicate
            "1,6\n"         // 5: unknown
            "1,5\n"         // 6: ok
            "1,5\n");       // 7: already cancelled
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err,
              "line 4: duplicate orderid 5 (an order with this id is still resting): 0,5,1,1,11\n"
              "line 5: cannot cancel orderid 6: no resting order with this id: 1,6\n"
              "line 7: cannot cancel orderid 5: no resting order with this id: 1,5\n");
    EXPECT_EQ(r.code, kExitOk);
}

TEST(App, TooLongLinesAreReportedAndProcessingContinues) {
    const std::string long_line = "0,1,0,1,1 //" + std::string(5000, 'c');  // valid message, huge comment
    const RunResult r = run("0,9,1,1,10\n" + long_line + "\n0,2,0,1,10\n");
    EXPECT_EQ(r.err, "line 2: line exceeds 4096 bytes: " + long_line.substr(0, 80) + "\xE2\x80\xA6\n");
    EXPECT_EQ(r.out, "2,1,10\n3,2\n3,9\n");
}

TEST(App, SurvivesArbitraryBinaryInput) {
    std::string garbage;
    for (int i = 0; i < 100'000; ++i) garbage.push_back(static_cast<char>((i * 7919) % 256));
    const RunResult r = run(garbage);
    EXPECT_EQ(r.code, kExitOk);
    EXPECT_FALSE(r.err.empty());
}

TEST(App, EmptyInput) {
    const RunResult r = run("");
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err, "");
    EXPECT_EQ(r.code, kExitOk);
}

TEST(App, CommandLine) {
    EXPECT_EQ(run(kExampleInput, {"--reserve", "1"}).out, kExampleOutput);
    EXPECT_EQ(run(kExampleInput, {"--reserve", "000100"}).out, kExampleOutput);
    // 2^31 itself is accepted by the parser; actually allocating it is
    // environment-dependent, so that path is tested with injected allocation
    // failure in no_alloc_test.

    const RunResult help = run("0,1,0,1,1\n", {"--help"});
    EXPECT_EQ(help.code, kExitOk);
    EXPECT_NE(help.out.find("usage: matcher"), std::string::npos);

    for (std::vector<std::string_view> bad :
         std::vector<std::vector<std::string_view>>{{"--reserve"},
                                                    {"--reserve", "0"},
                                                    {"--reserve", "-1"},
                                                    {"--reserve", "abc"},
                                                    {"--reserve", "2147483649"},
                                                    {"--reserve", "99999999999999999999999"},
                                                    {"--bogus"},
                                                    {"extra"}}) {
        const RunResult r = run("0,1,0,1,1\n", bad);
        EXPECT_EQ(r.code, kExitUsage) << bad[0];
        EXPECT_NE(r.err.find("usage: matcher"), std::string::npos) << bad[0];
        EXPECT_EQ(r.out, "");
    }
}

TEST(App, ReadErrorExitsWithStatusOne) {
    ScriptedReader in("0,1,1,1,10\n0,2,0,1,10\n", 11);
    in.fail_at(11, EIO);
    RecordingWriter out, err;
    EXPECT_EQ(run_app({}, in, out, err), kExitIoFailure);
    EXPECT_NE(err.data().find("stdin read failed"), std::string::npos);
}

TEST(App, StdoutFailureStopsAtARequestBoundaryWithStatusOne) {
    std::string input;
    for (int i = 1; i <= 50'000; i += 2) {
        input += "0," + std::to_string(i) + ",1,1,10\n";
        input += "0," + std::to_string(i + 1) + ",0,1,10\n";  // trades every pair
    }
    ScriptedReader in(input, 4096);  // small reads, so "stopped early" is observable
    RecordingWriter out, err;
    out.fail_after(100, EPIPE);
    EXPECT_EQ(run_app({}, in, out, err), kExitIoFailure);
    EXPECT_NE(err.data().find("stdout write failed: Broken pipe"), std::string::npos);
    EXPECT_LT(in.consumed(), input.size()) << "should stop early rather than drain all input";
}

TEST(App, StderrFailureIsIgnored) {
    ScriptedReader in(kExampleInput);
    RecordingWriter out, err;
    err.fail_after(0, EBADF);
    EXPECT_EQ(run_app({}, in, out, err), kExitOk);
    EXPECT_EQ(out.data(), kExampleOutput);
}

// Reader that runs a hook before every read(), so tests can observe what had
// been written by the time the app would block.
class ObservingReader final : public ByteReader {
public:
    ObservingReader(ScriptedReader& inner, std::function<void()> hook) : inner_(inner), hook_(std::move(hook)) {}
    long read(char* b, std::size_t n, int& e) noexcept override {
        hook_();
        return inner_.read(b, n, e);
    }

private:
    ScriptedReader& inner_;
    std::function<void()> hook_;
};

TEST(App, FlushesBeforeBlockingOnInput) {
    // Deliver one line per read, as an interactive terminal would.
    ScriptedReader inner("0,1,1,1,10\n0,2,0,1,10\nBAD\n", 11);
    RecordingWriter out, err;
    std::vector<std::string> seen_out, seen_err;
    ObservingReader in(inner, [&] {
        seen_out.push_back(out.data());
        seen_err.push_back(err.data());
    });
    EXPECT_EQ(run_app({}, in, out, err), kExitOk);
    // Before the read that follows line 2, line 2's trades are already visible.
    ASSERT_GE(seen_out.size(), 3u);
    EXPECT_EQ(seen_out[2], "2,1,10\n3,2\n3,1\n");
    // Before the final (EOF) read, the diagnostic is visible.
    EXPECT_EQ(seen_err.back(), "line 3: Unknown message type: BAD\n");
}

TEST(App, BatchInputIsWrittenInFewSystemCalls) {
    std::string input;
    for (int i = 1; i <= 20'000; i += 2)
        input += "0," + std::to_string(i) + ",1,1,10\n0," + std::to_string(i + 1) + ",0,1,10\n";
    ScriptedReader in(input);
    RecordingWriter out, err;
    EXPECT_EQ(run_app({}, in, out, err), kExitOk);
    EXPECT_EQ(out.lines().size(), 30'000u);
    EXPECT_LT(out.calls(), 100u);
}

// Usage text goes through the unbuffered startup writer: it must survive
// EINTR and partial writes, and give up quietly if the stream is broken.
TEST(App, UsageTextSurvivesInterruptedAndFailingWrites) {
    ScriptedReader in("");
    RecordingWriter out, err;
    out.interrupt_every_write();
    out.limit_per_call(7);
    EXPECT_EQ(run_app(std::vector<std::string_view>{"--help"}, in, out, err), kExitOk);
    EXPECT_EQ(out.data().rfind("usage: matcher", 0), 0u);
    EXPECT_NE(out.data().find("--help       show this message\n"), std::string::npos) << "usage text truncated";

    ScriptedReader in2("");
    RecordingWriter out2, err2;
    err2.fail_after(0, EBADF);
    EXPECT_EQ(run_app(std::vector<std::string_view>{"--bogus"}, in2, out2, err2), kExitUsage);
}

}  // namespace
}  // namespace matcher
