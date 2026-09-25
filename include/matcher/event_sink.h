#pragma once

#include "matcher/types.h"

namespace matcher {

// Receives the engine's output events, synchronously and in emission order.
//
// Callbacks must be noexcept: an exception in the middle of a sweep would leave
// the book half-updated. Sinks that can fail (e.g. stdout) record the failure
// internally and let the caller check it between requests.
// @spec MATCH-SAFE-001
template <class S>
concept EventSink = requires(S sink, const Trade& t, const OrderFullyFilled& f,
                             const OrderPartiallyFilled& p) {
    { sink.on_trade(t) } noexcept;
    { sink.on_fully_filled(f) } noexcept;
    { sink.on_partially_filled(p) } noexcept;
};

// Discards everything; used by the benchmark to time the engine alone.
struct NullSink {
    void on_trade(const Trade&) noexcept {}
    void on_fully_filled(const OrderFullyFilled&) noexcept {}
    void on_partially_filled(const OrderPartiallyFilled&) noexcept {}
};
static_assert(EventSink<NullSink>);

}  // namespace matcher
