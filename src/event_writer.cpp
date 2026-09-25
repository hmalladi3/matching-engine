#include "matcher/event_writer.h"

#include <charconv>

namespace matcher {

namespace {

constexpr std::size_t kMaxU64Digits = 20;

char* put_u64(char* out, std::uint64_t value) noexcept { return std::to_chars(out, out + kMaxU64Digits, value).ptr; }

}  // namespace

// Each callback formats one line straight into the output buffer.

void EventWriter::on_trade(const Trade& trade) noexcept {
    char* p = out_.reserve(kMaxLineBytes);
    *p++ = '2';
    *p++ = ',';
    p = put_u64(p, trade.qty);
    *p++ = ',';
    p = format_price(trade.price, p);
    *p++ = '\n';
    out_.commit(p);
}

void EventWriter::on_fully_filled(const OrderFullyFilled& event) noexcept {
    char* p = out_.reserve(kMaxLineBytes);
    *p++ = '3';
    *p++ = ',';
    p = put_u64(p, event.id);
    *p++ = '\n';
    out_.commit(p);
}

void EventWriter::on_partially_filled(const OrderPartiallyFilled& event) noexcept {
    char* p = out_.reserve(kMaxLineBytes);
    *p++ = '4';
    *p++ = ',';
    p = put_u64(p, event.id);
    *p++ = ',';
    p = put_u64(p, event.remaining);
    *p++ = '\n';
    out_.commit(p);
}

}  // namespace matcher
