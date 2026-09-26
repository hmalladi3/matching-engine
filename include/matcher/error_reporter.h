#pragma once

#include <cstdint>
#include <string_view>

#include "matcher/buffered_writer.h"
#include "matcher/request_parser.h"
#include "matcher/types.h"

namespace matcher {

// Formats diagnostics as `line N: <reason>: <excerpt>` on stderr. Excerpts are
// truncated to 80 bytes plus "…" and bytes outside printable ASCII are escaped
// as \xHH, so binary input stays terminal-safe. Best-effort: write failures are
// ignored (OUT-ERR-005).
// @spec PROTO-APP-001, PROTO-APP-002, OUT-DIAG-001
class ErrorReporter {
public:
    static constexpr std::size_t kExcerptBytes = 80;

    explicit ErrorReporter(BufferedWriter& err) noexcept : err_(err) {}

    // `cleaned_line` is the line after comment removal and trimming.
    void parse_error(std::uint64_t line, const ParseError& error, std::string_view cleaned_line) noexcept;
    void reject(std::uint64_t line, Reject reason, OrderId id, std::string_view cleaned_line) noexcept;
    // `raw_prefix` is the line's first (up to) 80 raw bytes.
    void line_too_long(std::uint64_t line, std::string_view raw_prefix) noexcept;
    // A complete message without line context.
    void message(std::string_view text) noexcept;
    // "<what>: <strerror(errnum)>", e.g. "stdout write failed: Broken pipe".
    void system_error(std::string_view what, int errnum) noexcept;

    std::uint64_t count() const noexcept { return count_; }

    // Longest possible diagnostic: "line " + 20 digits + ": " + a reason quoting
    // an escaped 80-byte field (up to 4 bytes per input byte, plus "…") + ": " +
    // the escaped 80-byte excerpt + "…" + "\n". 1 KiB covers it with room to spare.
    static constexpr std::size_t kMaxDiagnosticBytes = 1024;

private:
    char* begin(char* local) noexcept;
    void finish(char* local, char* end) noexcept;

    BufferedWriter& err_;
    std::uint64_t count_ = 0;
};

}  // namespace matcher
