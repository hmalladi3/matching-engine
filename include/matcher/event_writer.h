#pragma once

#include "matcher/buffered_writer.h"
#include "matcher/event_sink.h"

namespace matcher {

// The production EventSink: formats each event as a CSV line directly into
// the output buffer. No temporaries, no allocation.
class EventWriter {
public:
    // Longest line: "4," + 20 digits + "," + 20 digits + "\n" = 44 bytes.
    static constexpr std::size_t kMaxLineBytes = 64;

    explicit EventWriter(BufferedWriter& out) noexcept : out_(out) {}

    void on_trade(const Trade& trade) noexcept;
    void on_fully_filled(const OrderFullyFilled& event) noexcept;
    void on_partially_filled(const OrderPartiallyFilled& event) noexcept;

private:
    BufferedWriter& out_;
};
static_assert(EventSink<EventWriter>);

}  // namespace matcher
