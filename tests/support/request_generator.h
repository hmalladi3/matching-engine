#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "matcher/types.h"

namespace matcher::test {

// Small, fast, portable PRNG (xoshiro256**, seeded via splitmix64). Unlike
// <random> distributions, its output is identical on every platform and
// standard library, so a seed reproduces the same dataset everywhere.
class Rng {
public:
    explicit Rng(std::uint64_t seed);
    std::uint64_t next();
    // Uniform in [0, n). Precondition: n > 0.
    std::uint64_t below(std::uint64_t n);
    // Uniform in [lo, hi].
    std::int64_t between(std::int64_t lo, std::int64_t hi);
    // True with probability percent/100.
    bool percent(unsigned percent);

private:
    std::uint64_t s_[4];
};

// Workload shapes for differential tests, datasets and benchmarks.
enum class Profile {
    Tight,        // narrow price band around a drifting mid; heavy matching
    Deep,         // many levels, few crossings, few cancels
    CancelHeavy,  // most requests cancel recent orders
    Sweep,        // occasional large aggressive orders sweeping many levels
    IdReuse,      // tiny id space: constant duplicate rejections and reuse after fills
    Extreme,      // boundary prices (±max, 0, negatives) and quantities (1, 2^64-1)
    Mixed,        // a blend of all of the above, with the mid wandering through zero
};

inline constexpr Profile kAllProfiles[] = {Profile::Tight,   Profile::Deep,    Profile::CancelHeavy,
                                           Profile::Sweep,   Profile::IdReuse, Profile::Extreme,
                                           Profile::Mixed};

std::string_view name(Profile profile);
std::optional<Profile> profile_from_name(std::string_view name);

using Request = std::variant<AddOrder, CancelOrder>;

// Generates an endless, seeded request stream for a profile. It deliberately
// includes invalid requests (duplicate ids, cancels of unknown or already
// filled ids) so rejection paths are exercised alongside matching.
class RequestGenerator {
public:
    RequestGenerator(Profile profile, std::uint64_t seed);
    Request next();

private:
    struct Params {
        std::int64_t tick_raw;         // price increment (fixed-point raw units)
        std::int64_t start_mid;        // initial mid, in ticks
        std::int64_t band;             // passive orders rest within this many ticks of mid
        std::int64_t cross_depth;      // aggressive orders reach this many ticks through mid
        unsigned aggressive_pct;       // % of adds priced to cross
        unsigned cancel_pct;           // % of requests that are cancels
        unsigned unknown_cancel_pct;   // % of cancels naming a never-used id
        unsigned duplicate_pct;        // % of adds reusing a recent id
        unsigned sweep_pct;            // % of aggressive adds with a large quantity
        std::uint64_t max_qty;
        std::uint64_t sweep_qty;
        std::uint64_t id_space;        // 0: fresh sequential ids; else ids drawn from [1, id_space]
        unsigned drift_pct;            // % chance the mid moves one tick per request
        std::size_t max_live;          // cap on ids that may still be resting; forces cancels
    };

    static Params params_for(Profile profile);
    AddOrder make_add();
    AddOrder make_extreme_add();
    CancelOrder make_cancel();
    OrderId fresh_id();
    OrderId any_maybe_live();

    Profile profile_;
    Params params_;
    Rng rng_;
    std::int64_t mid_;
    OrderId next_id_ = 1;
    // Ids that may still be resting. The generator cannot see fills, so some
    // are already dead; cancelling those exercises UnknownOrderId. Capping
    // this set bounds the book size.
    std::vector<OrderId> maybe_live_;
};

}  // namespace matcher::test
