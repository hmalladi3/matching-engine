#include "matcher/request_parser.h"

namespace matcher {

std::string_view clean_line(std::string_view line) noexcept { return line; }  // Phase 5 stub

ParseResult parse_request(std::string_view line) noexcept {
    return ParseError{ParseError::Kind::UnknownMessageType, line};  // Phase 5 stub
}

}  // namespace matcher
