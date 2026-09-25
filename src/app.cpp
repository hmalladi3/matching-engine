#include "matcher/app.h"

namespace matcher {

int run_app(std::span<const std::string_view>, ByteReader&, ByteWriter&, ByteWriter&) noexcept {
    return kExitOk;  // Phase 5 stub
}

}  // namespace matcher
