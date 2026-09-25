#include "matcher/app.h"

#include <cerrno>
#include <charconv>
#include <exception>
#include <memory>
#include <variant>

#include "matcher/buffered_writer.h"
#include "matcher/error_reporter.h"
#include "matcher/event_writer.h"
#include "matcher/line_reader.h"
#include "matcher/matching_engine.h"
#include "matcher/request_parser.h"

namespace matcher {

namespace {

constexpr std::uint64_t kMaxReserve = std::uint64_t{1} << 31;

constexpr std::string_view kUsage =
    "usage: matcher [--reserve N] [--help]\n"
    "\n"
    "Reads order requests (CSV) from stdin and writes trades and fills to stdout.\n"
    "Invalid requests are reported on stderr and skipped.\n"
    "\n"
    "  --reserve N  preallocate capacity for N resting orders (1..2147483648,\n"
    "               default 1048576); the book still grows beyond it if needed\n"
    "  --help       show this message\n";

// Unbuffered, allocation-free write for startup paths where the buffered
// writers may not exist (usage errors, failed allocation).
void write_all(ByteWriter& out, std::string_view text) noexcept {
    while (!text.empty()) {
        int err = 0;
        const long n = out.write(text.data(), text.size(), err);
        if (n > 0)
            text.remove_prefix(static_cast<std::size_t>(n));
        else if (!(n < 0 && err == EINTR))
            return;
    }
}

int usage_error(ByteWriter& err, std::string_view problem, std::string_view arg) noexcept {
    write_all(err, "matcher: ");
    write_all(err, problem);
    if (!arg.empty()) {
        write_all(err, " '");
        write_all(err, arg);
        write_all(err, "'");
    }
    write_all(err, "\n");
    write_all(err, kUsage);
    return kExitUsage;
}

bool parse_reserve(std::string_view text, std::size_t& value) noexcept {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) return false;
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || end != text.data() + text.size() || v < 1 || v > kMaxReserve) return false;
    value = static_cast<std::size_t>(v);
    return true;
}

// Everything the main loop needs; constructed in one place so a failed
// startup allocation can be caught and reported.
class Session {
public:
    Session(ByteReader& in, ByteWriter& out, ByteWriter& err, std::size_t reserve)
        : reader_(in),
          out_(out),
          err_(err),
          events_(out_),
          errors_(err_),
          engine_(events_, BookConfig{.reserve_orders = reserve}) {}

    int run() noexcept {
        for (;;) {
            // Flush before we might block on input, so a pipe or terminal on
            // the other end sees results promptly (OUT-FLUSH-001).
            if (!reader_.has_buffered_line()) flush();

            const LineReader::Result r = reader_.next();
            switch (r.status) {
                case LineReader::Status::EndOfInput: return finish();
                case LineReader::Status::ReadError:
                    errors_.system_error("stdin read failed", r.error);
                    flush();
                    return kExitIoFailure;
                case LineReader::Status::LineTooLong: errors_.line_too_long(reader_.line_number(), r.text); break;
                case LineReader::Status::Line: process(r.text); break;
            }
            if (out_.failed()) return stdout_failed();  // stop at a request boundary
        }
    }

private:
    void process(std::string_view line) noexcept {
        const ParseResult parsed = parse_request(line);
        if (const auto* add = std::get_if<AddOrder>(&parsed)) {
            if (const Reject reject = engine_.add(*add); reject != Reject::None)
                errors_.reject(reader_.line_number(), reject, add->id, clean_line(line));
        } else if (const auto* cancel = std::get_if<CancelOrder>(&parsed)) {
            if (const Reject reject = engine_.cancel(*cancel); reject != Reject::None)
                errors_.reject(reader_.line_number(), reject, cancel->id, clean_line(line));
        } else if (const auto* error = std::get_if<ParseError>(&parsed)) {
            errors_.parse_error(reader_.line_number(), *error, clean_line(line));
        }  // BlankLine: nothing to do
    }

    void flush() noexcept {
        out_.flush();
        err_.flush();  // stderr failures are ignored: diagnostics are best-effort (OUT-ERR-005)
    }

    int finish() noexcept {
        flush();
        return out_.failed() ? stdout_failed() : kExitOk;
    }

    int stdout_failed() noexcept {
        errors_.system_error("stdout write failed", out_.error());
        err_.flush();
        return kExitIoFailure;
    }

    LineReader reader_;
    BufferedWriter out_;
    BufferedWriter err_;
    EventWriter events_;
    ErrorReporter errors_;
    MatchingEngine<EventWriter> engine_;
};

}  // namespace

int run_app(std::span<const std::string_view> args, ByteReader& in, ByteWriter& out, ByteWriter& err) noexcept {
    std::size_t reserve = BookConfig{}.reserve_orders;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            write_all(out, kUsage);
            return kExitOk;
        }
        if (arg != "--reserve") return usage_error(err, "unknown argument", arg);
        if (++i == args.size()) return usage_error(err, "--reserve requires a value", {});
        if (!parse_reserve(args[i], reserve))
            return usage_error(err, "--reserve must be an integer from 1 to 2147483648, got", args[i]);
    }

    std::unique_ptr<Session> session;
    try {
        session = std::make_unique<Session>(in, out, err, reserve);
    } catch (const std::exception&) {  // bad_alloc / length_error
        char buf[64];
        const auto end = std::to_chars(buf, buf + sizeof buf, reserve).ptr;
        write_all(err, "cannot reserve memory for ");
        write_all(err, {buf, static_cast<std::size_t>(end - buf)});
        write_all(err, " orders\n");
        return kExitIoFailure;
    }
    return session->run();
}

}  // namespace matcher
