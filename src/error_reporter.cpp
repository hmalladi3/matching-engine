#include "matcher/error_reporter.h"

namespace matcher {

std::string_view to_string(Reject) noexcept { return "stub"; }  // Phase 5 stub

void ErrorReporter::parse_error(std::uint64_t, const ParseError&, std::string_view) noexcept {}
void ErrorReporter::reject(std::uint64_t, Reject, OrderId, std::string_view) noexcept {}
void ErrorReporter::line_too_long(std::uint64_t, std::string_view) noexcept {}
void ErrorReporter::message(std::string_view) noexcept {}
void ErrorReporter::excerpt(std::string_view, bool) noexcept {}
void ErrorReporter::escaped(std::string_view) noexcept {}
void ErrorReporter::line_prefix(std::uint64_t) noexcept {}

}  // namespace matcher
