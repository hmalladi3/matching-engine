#include "matcher/event_writer.h"

namespace matcher {

void EventWriter::on_trade(const Trade&) noexcept {}  // Phase 5 stub
void EventWriter::on_fully_filled(const OrderFullyFilled&) noexcept {}
void EventWriter::on_partially_filled(const OrderPartiallyFilled&) noexcept {}

}  // namespace matcher
