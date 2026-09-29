#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include "matcher/price.h"
#include "support/request_generator.h"

namespace matcher {
namespace {

constexpr std::int64_t kMax = Price::kMaxRaw;

std::optional<Price> parse(std::string_view text) {
    PriceError why = PriceError::None;
    return parse_price(text, why);
}

PriceError error_of(std::string_view text) {
    PriceError why = PriceError::None;
    EXPECT_FALSE(parse_price(text, why).has_value()) << text;
    return why;
}

struct Accepted {
    const char* text;
    std::int64_t raw;
};

TEST(PriceParse, AcceptsTheGrammarExactly) {
    const Accepted cases[] = {
        {"1025", 1025 * Price::kScale},
        {"0", 0},
        {"-0", 0},
        {"-0.000", 0},
        {"1025.5", 102'550'000'000},
        {"1025.50", 102'550'000'000},
        {"0.00390625", 390'625},     // 1/256: Treasury futures tick
        {"0.0000005", 50},           // JPY FX futures tick
        {"0.00000001", 1},           // smallest representable step
        {"-37.63", -3'763'000'000},  // WTI, April 2020
        {"-0.5", -50'000'000},
        {"007", 7 * Price::kScale},  // leading zeros
        {"00.10", 10'000'000},
        {"1.000000000000", Price::kScale},  // zeros past the 8th place are exact
        {"1.12345678", 112'345'678},
        {"92233720368.54775807", kMax},  // largest magnitude
        {"-92233720368.54775807", -kMax},
        {"92233720368", 92'233'720'368 * Price::kScale},
    };
    for (const Accepted& c : cases) {
        auto p = parse(c.text);
        ASSERT_TRUE(p.has_value()) << c.text;
        EXPECT_EQ(p->raw(), c.raw) << c.text;
    }
}

TEST(PriceParse, RejectsNonZeroDigitsBeyondEightDecimals) {
    EXPECT_EQ(error_of("1025.000000001"), PriceError::TooPrecise);
    EXPECT_EQ(error_of("0.000000005"), PriceError::TooPrecise);
    EXPECT_EQ(error_of("-1.123456789"), PriceError::TooPrecise);
    EXPECT_EQ(error_of("1.000000000000000000000000001"), PriceError::TooPrecise);
}

TEST(PriceParse, RejectsOutOfRangeWithoutOverflow) {
    EXPECT_EQ(error_of("92233720368.54775808"), PriceError::OutOfRange);
    EXPECT_EQ(error_of("-92233720368.54775808"), PriceError::OutOfRange);  // INT64_MIN is excluded
    EXPECT_EQ(error_of("92233720369"), PriceError::OutOfRange);
    EXPECT_EQ(error_of("100000000000"), PriceError::OutOfRange);
    EXPECT_EQ(error_of("18446744073709551616"), PriceError::OutOfRange);  // 2^64
    EXPECT_EQ(error_of("99999999999999999999999999999999999999"), PriceError::OutOfRange);
    EXPECT_EQ(error_of(std::string(5000, '9')), PriceError::OutOfRange);
}

TEST(PriceParse, RejectsEverythingOutsideTheGrammar) {
    const char* cases[] = {
        "",    "-",   "+5", ".5",    "5.",  "-.5",  "1e3", "1E3", "0x10", "inf", "-inf", "nan", "1,5", " 5",    "5 ",
        "1 0", "--5", "5-", "1.2.3", "abc", "1..2", "١٢",  "\t5", "5\n",  "½",   "+-5",  "-+5", "0b1", "1_000", "1'000",
    };
    for (const char* text : cases) EXPECT_EQ(error_of(text), PriceError::Malformed) << '"' << text << '"';
    EXPECT_EQ(error_of(std::string_view("5\0", 2)), PriceError::Malformed);
}

TEST(PriceParse, NegativeZeroEqualsZero) {
    EXPECT_EQ(parse("-0"), parse("0"));
    EXPECT_EQ(parse("-0.00000000"), Price{});
}

TEST(PriceFormat, WritesShortestExactDecimal) {
    const std::pair<std::int64_t, const char*> cases[] = {
        {0, "0"},
        {1025 * Price::kScale, "1025"},
        {102'550'000'000, "1025.5"},
        {390'625, "0.00390625"},
        {1, "0.00000001"},
        {-1, "-0.00000001"},
        {-50'000'000, "-0.5"},
        {-3'763'000'000, "-37.63"},
        {kMax, "92233720368.54775807"},
        {-kMax, "-92233720368.54775807"},
        {10 * Price::kScale, "10"},
        {100'000'000'000, "1000"},
    };
    for (const auto& [raw, text] : cases) {
        EXPECT_EQ(to_string(Price::from_raw(raw)), text) << raw;
    }
}

TEST(PriceFormat, NeverExceedsMaxFormattedLength) {
    char buf[Price::kMaxFormattedLen + 8];
    for (std::int64_t raw : {kMax, -kMax, -kMax + 1, std::int64_t{-1}}) {
        char* end = format_price(Price::from_raw(raw), buf);
        EXPECT_LE(static_cast<std::size_t>(end - buf), Price::kMaxFormattedLen);
    }
}

TEST(PriceFormat, RoundTripsEveryValue) {
    test::Rng rng(20260925);
    auto check = [](std::int64_t raw) {
        const Price p = Price::from_raw(raw);
        const std::string text = to_string(p);
        auto back = parse(text);
        ASSERT_TRUE(back.has_value()) << text;
        EXPECT_EQ(*back, p) << text;
    };
    for (std::int64_t raw : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, kMax, -kMax, Price::kScale,
                             -Price::kScale, Price::kScale - 1, Price::kScale + 1})
        check(raw);
    for (int i = 0; i < 200'000; ++i) {
        // Mix full-range values with values that have few significant decimals.
        const std::int64_t raw =
            (i % 2 == 0) ? rng.between(-1'000'000, 1'000'000) * Price::kScale + rng.between(-99, 99) * 1'000'000
                         : rng.between(-kMax, kMax);
        check(raw);
    }
}

TEST(PriceCompare, IsExactIntegerComparison) {
    EXPECT_LT(Price::from_raw(-1), Price::from_raw(0));
    EXPECT_LT(Price::from_raw(kMax - 1), Price::from_raw(kMax));
    EXPECT_EQ(*parse("1025.50"), *parse("1025.5"));
    EXPECT_LT(*parse("1025.49999999"), *parse("1025.5"));
    EXPECT_GT(*parse("-37.62"), *parse("-37.63"));
    // The classic floating-point trap is exact here.
    EXPECT_EQ(Price::from_raw(parse("0.1")->raw() + parse("0.2")->raw()), *parse("0.3"));
}

TEST(PriceDescribe, HasTextForEveryError) {
    EXPECT_EQ(describe(PriceError::Malformed), "malformed");
    EXPECT_EQ(describe(PriceError::TooPrecise), "more than 8 decimal places");
    EXPECT_EQ(describe(PriceError::OutOfRange), "out of range");
}

}  // namespace
}  // namespace matcher
