// Price parsing never crashes, and every accepted price round-trips exactly
// through format -> parse.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "matcher/price.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace matcher;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    PriceError why = PriceError::None;
    const auto price = parse_price(text, why);
    if (!price) {
        if (why == PriceError::None) std::abort();  // a failure must say why
        return 0;
    }
    char buf[Price::kMaxFormattedLen];
    const char* end = format_price(*price, buf);
    PriceError why2 = PriceError::None;
    const auto again = parse_price(std::string_view(buf, static_cast<std::size_t>(end - buf)), why2);
    if (!again || *again != *price) std::abort();
    return 0;
}
