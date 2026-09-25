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

const char* const kBriefInput =
    "0,1000000,1,1,1075\n"
    "0,1000001,0,9,1000\n"
    "0,1000002,0,30,975\n"
    "0,1000003,1,10,1050\n"
    "0,1000004,0,10,950\n"
    "BADMESSAGE\n"
    "0,1000005,1,2,1025\n"
    "0,1000006,0,1,1000\n"
    "1,1000004\n"
    "0,1000007,1,5,1025\n"
    "0,1000008,0,3,1050\n";

const char* const kBriefOutput =
    "2,2,1025\n"
    "4,1000008,1\n"
    "3,1000005\n"
    "2,1,1025\n"
    "3,1000008\n"
    "4,1000007,4\n";

// @spec PROTO-APP-001, PROTO-APP-003
TEST(App, BriefExampleEndToEnd) {
    const RunResult r = run(kBriefInput);
    EXPECT_EQ(r.out, kBriefOutput);
    EXPECT_EQ(r.err, "line 6: Unknown message type: BADMESSAGE\n");
    EXPECT_EQ(r.code, kExitOk);
}

// @spec PROTO-READ-004
TEST(App, ByteAtATimeInputGivesIdenticalResults) {
    const RunResult r = run(kBriefInput, {}, 1);
    EXPECT_EQ(r.out, kBriefOutput);
    EXPECT_EQ(r.err, "line 6: Unknown message type: BADMESSAGE\n");
}

// @spec PROTO-APP-001, PROTO-READ-007, OUT-DIAG-001
TEST(App, EngineRejectionsAreReportedWithLineNumbers) {
    const RunResult r = run(
        "\n"                      // 1: blank
        "// comment\n"            // 2: comment only
        "0,5,0,1,10\n"            // 3: rests
        "0,5,1,1,11\n"            // 4: duplicate
        "1,6\n"                   // 5: unknown
        "1,5\n"                   // 6: ok
        "1,5\n");                 // 7: already cancelled
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err,
              "line 4: duplicate orderid 5 (an order with this id is still resting): 0,5,1,1,11\n"
              "line 5: cannot cancel orderid 6: no resting order with this id: 1,6\n"
              "line 7: cannot cancel orderid 5: no resting order with this id: 1,5\n");
    EXPECT_EQ(r.code, kExitOk);
}

// @spec PROTO-READ-003, PROTO-APP-002
TEST(App, TooLongLinesAreReportedAndProcessingContinues) {
    const std::string long_line = "0,1,0,1,1 //" + std::string(5000, 'c');  // valid message, huge comment
    const RunResult r = run("0,9,1,1,10\n" + long_line + "\n0,2,0,1,10\n");
    EXPECT_EQ(r.err, "line 2: line exceeds 4096 bytes: " + long_line.substr(0, 80) + "\xE2\x80\xA6\n");
    EXPECT_EQ(r.out, "2,1,10\n3,2\n3,9\n");
}

// @spec PROTO-APP-005
TEST(App, SurvivesArbitraryBinaryInput) {
    std::string garbage;
    for (int i = 0; i < 100'000; ++i) garbage.push_back(static_cast<char>((i * 7919) % 256));
    const RunResult r = run(garbage);
    EXPECT_EQ(r.code, kExitOk);
    EXPECT_FALSE(r.err.empty());
}

// @spec PROTO-APP-005
TEST(App, EmptyInput) {
    const RunResult r = run("");
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err, "");
    EXPECT_EQ(r.code, kExitOk);
}

// @spec PROTO-APP-004
TEST(App, CommandLine) {
    EXPECT_EQ(run(kBriefInput, {"--reserve", "1"}).out, kBriefOutput);
    EXPECT_EQ(run(kBriefInput, {"--reserve", "000100"}).out, kBriefOutput);
    // 2^31 itself is accepted by the parser; actually allocating it is
    // environment-dependent, so that path is tested with injected allocation
    // failure in no_alloc_test (PROTO-APP-006).

    const RunResult help = run("0,1,0,1,1\n", {"--help"});
    EXPECT_EQ(help.code, kExitOk);
    EXPECT_NE(help.out.find("usage: matcher"), std::string::npos);

    for (std::vector<std::string_view> bad : std::vector<std::vector<std::string_view>>{
             {"--reserve"}, {"--reserve", "0"}, {"--reserve", "-1"}, {"--reserve", "abc"},
             {"--reserve", "2147483649"}, {"--reserve", "99999999999999999999999"}, {"--bogus"}, {"extra"}}) {
        const RunResult r = run("0,1,0,1,1\n", bad);
        EXPECT_EQ(r.code, kExitUsage) << bad[0];
        EXPECT_NE(r.err.find("usage: matcher"), std::string::npos) << bad[0];
        EXPECT_EQ(r.out, "");
    }
}

// @spec PROTO-APP-003
TEST(App, ReadErrorExitsWithStatusOne) {
    ScriptedReader in("0,1,1,1,10\n0,2,0,1,10\n", 11);
    in.fail_at(11, EIO);
    RecordingWriter out, err;
    EXPECT_EQ(run_app({}, in, out, err), kExitIoFailure);
    EXPECT_NE(err.data().find("stdin read failed"), std::string::npos);
}

// @spec OUT-ERR-003
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

// @spec OUT-ERR-005
TEST(App, StderrFailureIsIgnored) {
    ScriptedReader in(kBriefInput);
    RecordingWriter out, err;
    err.fail_after(0, EBADF);
    EXPECT_EQ(run_app({}, in, out, err), kExitOk);
    EXPECT_EQ(out.data(), kBriefOutput);
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

// @spec OUT-FLUSH-001, OUT-FLUSH-003
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

// @spec OUT-FLUSH-001
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

}  // namespace
}  // namespace matcher
