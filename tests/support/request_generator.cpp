#include "support/request_generator.h"

#include <array>
#include <limits>

namespace matcher::test {

namespace {

std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

constexpr std::int64_t kMaxRaw = Price::kMaxRaw;

}  // namespace

Rng::Rng(std::uint64_t seed) {
    for (std::uint64_t& s : s_) s = splitmix64(seed);
}

std::uint64_t Rng::next() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

std::uint64_t Rng::below(std::uint64_t n) { return next() % n; }

std::int64_t Rng::between(std::int64_t lo, std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1;
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + (span == 0 ? next() : below(span)));
}

bool Rng::percent(unsigned pct) { return below(100) < pct; }

std::string_view name(Profile profile) {
    switch (profile) {
        case Profile::Tight: return "tight";
        case Profile::Deep: return "deep";
        case Profile::CancelHeavy: return "cancel_heavy";
        case Profile::Sweep: return "sweep";
        case Profile::IdReuse: return "id_reuse";
        case Profile::Extreme: return "extreme";
        case Profile::Mixed: return "mixed";
    }
    return "unknown";
}

std::optional<Profile> profile_from_name(std::string_view text) {
    for (Profile p : kAllProfiles)
        if (name(p) == text) return p;
    return std::nullopt;
}

RequestGenerator::Params RequestGenerator::params_for(Profile profile) {
    //                 tick        mid   band cross aggr cxl unk dup swp max_qty sweep_qty id_space drift live
    switch (profile) {
        case Profile::Tight: return {25'000'000, 4'000, 5, 3, 30, 35, 5, 1, 0, 20, 0, 0, 20, 300};
        case Profile::Deep: return {1'000'000, 100'000, 2'000, 2, 5, 10, 5, 1, 0, 50, 0, 0, 5, 2'000};
        case Profile::CancelHeavy: return {Price::kScale, 1'000, 20, 2, 15, 60, 10, 1, 0, 10, 0, 0, 10, 300};
        case Profile::Sweep: return {Price::kScale / 100, 50'000, 50, 60, 20, 30, 5, 1, 30, 10, 2'000, 0, 10, 500};
        case Profile::IdReuse: return {Price::kScale, 500, 10, 3, 30, 30, 5, 20, 0, 10, 0, 300, 10, 200};
        case Profile::Extreme: return {Price::kScale, 0, 10, 3, 30, 30, 5, 2, 10, 5, 100, 0, 10, 300};
        case Profile::Mixed: return {1, 0, 200, 20, 25, 35, 5, 3, 5, 100, 5'000, 0, 30, 1'000};
    }
    return params_for(Profile::Tight);
}

RequestGenerator::RequestGenerator(Profile profile, std::uint64_t seed)
    : profile_(profile), params_(params_for(profile)), rng_(seed), mid_(params_.start_mid) {
    maybe_live_.reserve(params_.max_live + 1);
}

Request RequestGenerator::next() {
    if (rng_.percent(params_.drift_pct)) mid_ += rng_.percent(50) ? 1 : -1;
    const bool must_cancel = maybe_live_.size() >= params_.max_live;
    if (!maybe_live_.empty() && (must_cancel || rng_.percent(params_.cancel_pct))) return make_cancel();
    if (profile_ == Profile::Extreme && rng_.percent(40)) return make_extreme_add();
    return make_add();
}

OrderId RequestGenerator::fresh_id() {
    if (params_.id_space != 0) return 1 + rng_.below(params_.id_space);
    return next_id_++;
}

OrderId RequestGenerator::any_maybe_live() { return maybe_live_[rng_.below(maybe_live_.size())]; }

AddOrder RequestGenerator::make_add() {
    const Side side = rng_.percent(50) ? Side::Buy : Side::Sell;
    const bool aggressive = rng_.percent(params_.aggressive_pct);

    // Offset from mid in ticks: passive orders sit on their own side of the
    // mid; aggressive ones reach through it.
    std::int64_t offset = aggressive ? rng_.between(0, params_.cross_depth) : -rng_.between(1, params_.band);
    if (side == Side::Sell) offset = -offset;
    const std::int64_t price_raw = (mid_ + offset) * params_.tick_raw;

    Quantity qty = 1 + rng_.below(params_.max_qty);
    if (aggressive && params_.sweep_qty != 0 && rng_.percent(params_.sweep_pct)) qty = params_.sweep_qty;

    if (!maybe_live_.empty() && rng_.percent(params_.duplicate_pct))
        return {any_maybe_live(), side, qty, Price::from_raw(price_raw)};  // likely DuplicateOrderId
    const OrderId id = fresh_id();
    maybe_live_.push_back(id);
    return {id, side, qty, Price::from_raw(price_raw)};
}

AddOrder RequestGenerator::make_extreme_add() {
    static constexpr std::array<std::int64_t, 8> kPrices = {kMaxRaw, kMaxRaw - 1, -kMaxRaw, -kMaxRaw + 1,
                                                            0,       1,           -1,       Price::kScale};
    static constexpr std::array<Quantity, 4> kQuantities = {1, 2, std::numeric_limits<Quantity>::max(),
                                                            std::numeric_limits<Quantity>::max() - 1};

    const Side side = rng_.percent(50) ? Side::Buy : Side::Sell;
    const std::int64_t price = kPrices[rng_.below(kPrices.size())];
    const Quantity qty = kQuantities[rng_.below(kQuantities.size())];
    const OrderId id = rng_.percent(5) ? std::numeric_limits<OrderId>::max() - rng_.below(3) : fresh_id();
    maybe_live_.push_back(id);
    return {id, side, qty, Price::from_raw(price)};
}

CancelOrder RequestGenerator::make_cancel() {
    if (rng_.percent(params_.unknown_cancel_pct))
        return {std::numeric_limits<OrderId>::max() / 2 + rng_.below(1'000'000)};  // never issued
    // Swap-remove a random maybe-live id.
    const std::size_t i = rng_.below(maybe_live_.size());
    const OrderId id = maybe_live_[i];
    maybe_live_[i] = maybe_live_.back();
    maybe_live_.pop_back();
    return {id};
}

}  // namespace matcher::test
