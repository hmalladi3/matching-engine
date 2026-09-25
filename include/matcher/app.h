#pragma once

#include <span>
#include <string_view>

#include "matcher/io.h"

namespace matcher {

enum ExitCode : int {
    kExitOk = 0,         // all input processed, all output written
    kExitIoFailure = 1,  // stdin read error, stdout write failure, startup allocation failure
    kExitUsage = 2,      // invalid command line
};

// The whole program minus process setup: parses `args` (argv without argv[0]),
// reads requests from `in`, writes events to `out` and diagnostics to `err`.
// Runs in-process so tests can drive it end to end without spawning.
// @spec PROTO-APP-003, PROTO-APP-004, PROTO-APP-005, PROTO-APP-006,
//       OUT-FLUSH-001, OUT-FLUSH-003, OUT-ERR-003, OUT-ERR-005
int run_app(std::span<const std::string_view> args, ByteReader& in, ByteWriter& out, ByteWriter& err) noexcept;

}  // namespace matcher
