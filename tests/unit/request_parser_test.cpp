#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "matcher/request_parser.h"
#include "support/harness.h"
#include "support/request_generator.h"

namespace matcher {
namespace {

using Kind = ParseError::Kind;
using IntDetail = ParseError::IntDetail;
using test::px;

ParseError error_of(std::string_view line) {
    const ParseResult r = parse_request(line);
    EXPECT_TRUE(std::holds_alternative<ParseError>(r)) << '"' << line << '"';
    return std::holds_alternative<ParseError>(r) ? std::get<ParseError>(r) : ParseError{Kind::BadPrice, {}};
}

AddOrder add_of(std::string_view line) {
    const ParseResult r = parse_request(line);
    EXPECT_TRUE(std::holds_alternative<AddOrder>(r)) << '"' << line << '"';
    return std::holds_alternative<AddOrder>(r) ? std::get<AddOrder>(r) : AddOrder{};
}

TEST(RequestParser, ParsesAddAndCancel) {
    EXPECT_EQ(parse_request("0,123,0,9,1000"), ParseResult(AddOrder{123, Side::Buy, 9, px("1000")}));
    EXPECT_EQ(parse_request("0,101,1,5,100.25"), ParseResult(AddOrder{101, Side::Sell, 5, px("100.25")}));
    EXPECT_EQ(parse_request("1,123"), ParseResult(CancelOrder{123}));
}

TEST(RequestParser, StripsDoubleSlashComments) {
    EXPECT_EQ(add_of("0,104,1,2,100.25        // joins the queue behind 101"),
              (AddOrder{104, Side::Sell, 2, px("100.25")}));
    EXPECT_EQ(parse_request("1,103 // cancel"), ParseResult(CancelOrder{103}));
    EXPECT_EQ(parse_request("// just a comment"), ParseResult(BlankLine{}));
    EXPECT_EQ(parse_request("0,1,0,9,1000//x"), ParseResult(AddOrder{1, Side::Buy, 9, px("1000")}));
    EXPECT_EQ(clean_line("HELLO                   // not a request"), "HELLO");
    // A single slash is not a comment.
    EXPECT_EQ(error_of("0,1,0,9,10/0").kind, Kind::BadPrice);
}

TEST(RequestParser, TrimsAsciiAndInvisibleUnicodeWhitespace) {
    const AddOrder expected{123, Side::Buy, 9, px("1000")};
    EXPECT_EQ(add_of(" 0 , 123 ,\t0\t, 9 , 1000 "), expected);
    const std::string nbsp = "\xC2\xA0", zwsp = "\xE2\x80\x8B", bom = "\xEF\xBB\xBF";
    EXPECT_EQ(add_of(bom + "0,123,0,9,1000"), expected);
    EXPECT_EQ(add_of("0," + zwsp + "123" + zwsp + ",0,9,1000" + nbsp), expected);
    // Text copied from a document can carry zero-width spaces before a comment.
    EXPECT_EQ(parse_request("1,103   " + zwsp + "    " + zwsp + "     // cancel"), ParseResult(CancelOrder{103}));
    EXPECT_EQ(clean_line(zwsp + " x " + nbsp), "x");
}

TEST(RequestParser, WhitespaceInsideAFieldIsInvalid) {
    EXPECT_EQ(error_of("0,1 23,0,9,1000").kind, Kind::BadOrderId);
    EXPECT_EQ(error_of("0,123,0,9,10 00").kind, Kind::BadPrice);
    EXPECT_EQ(error_of("0,123,0,9,10\xE2\x80\x8B"
                       "00")
                  .kind,
              Kind::BadPrice);
    // Characters outside the whitespace set are not trimmed.
    EXPECT_EQ(error_of("0,123,0,9,1000\r").kind, Kind::BadPrice);
    EXPECT_EQ(error_of("0,123,0,9,1000\v").kind, Kind::BadPrice);
}

TEST(RequestParser, BlankLinesAreSkipped) {
    for (std::string_view line : {"", " ", "\t \t", "\xC2\xA0", "   // comment", "//"})
        EXPECT_EQ(parse_request(line), ParseResult(BlankLine{})) << '"' << line << '"';
}

TEST(RequestParser, UnknownMessageTypes) {
    for (std::string_view line : {"HELLO", "2,2,1025", "3,123", "4,123,3", "00,1,0,1,1", "-1,1", "01,1", "5", ",",
                                  "0x0,1,0,1,1", "\x01", "0.0,1,0,1,1"}) {
        EXPECT_EQ(error_of(line).kind, Kind::UnknownMessageType) << '"' << line << '"';
    }
}

TEST(RequestParser, WrongFieldCounts) {
    ParseError e = error_of("0,1,0,9");
    EXPECT_EQ(e.kind, Kind::WrongFieldCount);
    EXPECT_TRUE(e.is_add);
    EXPECT_EQ(e.expected_fields, 5u);
    EXPECT_EQ(e.actual_fields, 4u);

    e = error_of("0,1,0,9,1000,7");
    EXPECT_EQ(e.actual_fields, 6u);

    e = error_of("1,1,");  // trailing comma adds an empty field
    EXPECT_EQ(e.kind, Kind::WrongFieldCount);
    EXPECT_FALSE(e.is_add);
    EXPECT_EQ(e.expected_fields, 2u);
    EXPECT_EQ(e.actual_fields, 3u);

    EXPECT_EQ(error_of("1").actual_fields, 1u);
    EXPECT_EQ(error_of("0").actual_fields, 1u);
    EXPECT_EQ(error_of("0,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,").kind,
              Kind::WrongFieldCount);
}

TEST(RequestParser, OrderIdValidation) {
    auto check = [](std::string_view line, IntDetail detail, std::string_view field) {
        const ParseError e = error_of(line);
        EXPECT_EQ(e.kind, Kind::BadOrderId) << line;
        EXPECT_EQ(e.int_detail, detail) << line;
        EXPECT_EQ(e.field, field) << line;
    };
    check("0,abc,0,9,1000", IntDetail::Malformed, "abc");
    check("1,-5", IntDetail::Malformed, "-5");
    check("1,+5", IntDetail::Malformed, "+5");
    check("1,1.0", IntDetail::Malformed, "1.0");
    check("1,", IntDetail::Malformed, "");
    check("1,0", IntDetail::NotPositive, "0");
    check("1,000", IntDetail::NotPositive, "000");
    check("1,18446744073709551616", IntDetail::OutOfRange, "18446744073709551616");
    check("1,99999999999999999999999", IntDetail::OutOfRange, "99999999999999999999999");
    EXPECT_EQ(parse_request("1,18446744073709551615"), ParseResult(CancelOrder{18446744073709551615ULL}));
}

TEST(RequestParser, SideValidation) {
    for (std::string_view line :
         {"0,1,2,9,1000", "0,1,-1,9,1000", "0,1,00,9,1000", "0,1,,9,1000", "0,1,B,9,1000", "0,1,01,9,1000"}) {
        EXPECT_EQ(error_of(line).kind, Kind::BadSide) << line;
    }
    EXPECT_EQ(add_of("0,1,1,9,1000").side, Side::Sell);
}

TEST(RequestParser, QuantityValidation) {
    EXPECT_EQ(error_of("0,1,0,0,1000").int_detail, IntDetail::NotPositive);
    EXPECT_EQ(error_of("0,1,0,-5,1000").int_detail, IntDetail::Malformed);
    EXPECT_EQ(error_of("0,1,0,1.5,1000").int_detail, IntDetail::Malformed);
    EXPECT_EQ(error_of("0,1,0,18446744073709551616,1000").int_detail, IntDetail::OutOfRange);
    EXPECT_EQ(error_of("0,1,0,x,1000").kind, Kind::BadQuantity);
    EXPECT_EQ(add_of("0,1,0,18446744073709551615,1000").qty, 18446744073709551615ULL);
}

TEST(RequestParser, PriceErrorsCarryThePriceParsersReason) {
    EXPECT_EQ(error_of("0,1,0,9,1e3").price_error, PriceError::Malformed);
    EXPECT_EQ(error_of("0,1,0,9,1.000000001").price_error, PriceError::TooPrecise);
    EXPECT_EQ(error_of("0,1,0,9,99999999999").price_error, PriceError::OutOfRange);
    EXPECT_EQ(error_of("0,1,0,9,").kind, Kind::BadPrice);
    EXPECT_EQ(add_of("0,1,0,9,-37.63").price, px("-37.63"));
    EXPECT_EQ(add_of("0,1,0,9,0").price, px("0"));
}

TEST(RequestParser, ReportsOnlyTheFirstDefectInFieldOrder) {
    EXPECT_EQ(error_of("7,x,y").kind, Kind::UnknownMessageType);
    EXPECT_EQ(error_of("0,x,y,z").kind, Kind::WrongFieldCount);
    EXPECT_EQ(error_of("0,x,9,-1,bad").kind, Kind::BadOrderId);
    EXPECT_EQ(error_of("0,1,9,-1,bad").kind, Kind::BadSide);
    EXPECT_EQ(error_of("0,1,0,-1,bad").kind, Kind::BadQuantity);
    EXPECT_EQ(error_of("0,1,0,1,bad").kind, Kind::BadPrice);
}

TEST(RequestParser, AcceptsLeadingZerosInIntegers) {
    EXPECT_EQ(add_of("0,007,0,0009,1000"), (AddOrder{7, Side::Buy, 9, px("1000")}));
}

TEST(RequestParser, NeverReadsOutsideTheLine) {
    // A view into a larger buffer: the parser must stop at the view's end.
    const std::string backing = "1,12345";
    EXPECT_EQ(parse_request(std::string_view(backing).substr(0, 4)), ParseResult(CancelOrder{12}));
}

// Fields that end, but do not start, with whitespace take trim()'s slow path.
TEST(RequestParser, TrailingOnlyWhitespaceOfEveryKind) {
    const AddOrder expected{123, Side::Buy, 9, px("1000")};
    EXPECT_EQ(add_of("0,123\t,0,9,1000"), expected);
    EXPECT_EQ(add_of("0,123\xE2\x80\x8B,0,9,1000"), expected);
    EXPECT_EQ(add_of("0,123\xEF\xBB\xBF,0,9,1000"), expected);
    EXPECT_EQ(add_of("0,123\xC2\xA0,0,9,1000"), expected);
    EXPECT_EQ(add_of("0,123 ,0,9,1000"), expected);
}

// ParseError compares field by field (tests rely on it for exact results).
TEST(RequestParser, ParseErrorEquality) {
    const ParseError base = error_of("0,1,0,9");
    EXPECT_EQ(base, error_of("0,1,0,9"));
    ParseError other = base;
    other.actual_fields = 3;
    EXPECT_NE(base, other);
    EXPECT_NE(error_of("1,abc"), error_of("1,abd"));
    EXPECT_EQ(error_of("1,abc"), error_of("0,abc,0,1,1"));  // same kind, detail and field
    EXPECT_NE(error_of("1,abc"), error_of("0,1,abc,1,1"));  // different kind
    EXPECT_NE(error_of("0,1,0,9,1e3"), error_of("0,1,0,9,1.000000001"));
    EXPECT_NE(error_of("1,0"), error_of("1,abc"));
    EXPECT_NE(error_of("0,1,0,9"), error_of("1,1,2"));
}

// The fast path must never change a result: for every input, parse_request()
// equals the general parser. Covers clean lines, every error form, and random
// mutations of valid lines.
TEST(RequestParser, FastPathAgreesWithTheGeneralParserOnEveryInput) {
    std::vector<std::string> lines = {"0,1,0,1,1",
                                      "1,1",
                                      "0,18446744073709551615,1,18446744073709551615,-92233720368.54775807",
                                      "0,18446744073709551616,0,1,1",
                                      "0,1,0,99999999999999999999,1",
                                      "0,007,0,0009,1000",
                                      "0,0,0,1,1",
                                      "0,1,0,0,1",
                                      "1,0",
                                      "1,000",
                                      "0,1,2,1,1",
                                      "0,1,0,1,1.000000001",
                                      "0,1,0,1,1e3",
                                      "0,1,0,1,",
                                      "0,1,0,1",
                                      "0,1,0,1,1,",
                                      "1,1,",
                                      "1,",
                                      "1",
                                      "0",
                                      "",
                                      ",",
                                      "0,",
                                      "00,1,0,1,1",
                                      "01,1",
                                      "2,2,1025",
                                      "0,1,0,1,1 // c",
                                      " 0,1,0,1,1",
                                      "0 ,1,0,1,1",
                                      "0,1 ,0,1,1",
                                      "0,1,0,1,1\r",
                                      "0,1,0,1,1\t",
                                      "0,1,01,1,1",
                                      "0,1,-1,1,1",
                                      "0,-1,0,1,1",
                                      "0,+1,0,1,1",
                                      "0,1,0,+1,1",
                                      "0,1,0,1,+1",
                                      "0,1,0,1,-0",
                                      "0,1,0,1,.5",
                                      "0,1,0,1,5.",
                                      "HELLO",
                                      "1,1//x",
                                      "0,1,0,1,1//x",
                                      "\xEF\xBB\xBF"
                                      "0,1,0,1,1",
                                      "0,1,0,1,1\xC2\xA0",
                                      "0,1,1,1,0.00000001",
                                      "0,12345678901234567890,0,1,1",
                                      "0,1234567890123456789,0,1234567890123456789,92233720368"};
    test::RequestGenerator generator(test::Profile::Mixed, 17);
    for (int i = 0; i < 200'000; ++i) {
        const test::Request r = generator.next();
        if (const auto* a = std::get_if<AddOrder>(&r))
            lines.push_back("0," + std::to_string(a->id) + "," + std::to_string(static_cast<int>(a->side)) + "," +
                            std::to_string(a->qty) + "," + to_string(a->price));
        else
            lines.push_back("1," + std::to_string(std::get<CancelOrder>(r).id));
    }
    // Random mutations of valid lines: replace, insert or delete one byte, or truncate.
    test::Rng rng(99);
    static constexpr char kBytes[] = "0123456789,.-+ /\t\r\xC2\xA0x";
    const std::size_t originals = lines.size();
    for (int i = 0; i < 300'000; ++i) {
        std::string line = lines[rng.below(originals)];
        const std::size_t at = rng.below(line.size() + 1);
        const char byte = kBytes[rng.below(sizeof kBytes - 1)];
        switch (rng.below(4)) {
            case 0:
                if (at < line.size()) line[at] = byte;
                break;
            case 1: line.insert(at, 1, byte); break;
            case 2:
                if (at < line.size()) line.erase(at, 1);
                break;
            default: line.resize(at); break;
        }
        lines.push_back(std::move(line));
    }
    for (const std::string& line : lines)
        ASSERT_EQ(parse_request(line), detail::parse_request_general(line)) << '"' << line << '"';
}

// Every digit count from 1 to 21 in every numeric field, against the general
// parser: crosses each fast-path limit (10 integer and 8 fraction digits in a
// price, 19 digits in an integer) and the uint64 range. Under UBSan this also
// checks that over-long runs never overflow a signed accumulator.
TEST(RequestParser, FastPathHandlesEveryDigitCount) {
    test::Rng rng(5);
    auto digits = [&](int n, bool nonzero_first) {
        std::string d;
        for (int i = 0; i < n; ++i)
            d += static_cast<char>('0' + ((i == 0 && nonzero_first) ? 1 + rng.below(9) : rng.below(10)));
        return d;
    };
    for (int round = 0; round < 50; ++round) {
        for (int n = 1; n <= 21; ++n) {
            const std::string number = digits(n, round % 2 == 0);
            const std::string all_nines(static_cast<std::size_t>(n), '9');
            for (const std::string& value : {number, all_nines}) {
                for (const std::string& line : {"1," + value, "0," + value + ",0,1,1", "0,1,1," + value + ",1",
                                                "0,1,0,1," + value, "0,1,0,1,-" + value, "0,1,0,1,1." + value,
                                                "0,1,0,1," + value + "." + digits(1 + round % 9, false)}) {
                    ASSERT_EQ(parse_request(line), detail::parse_request_general(line)) << '"' << line << '"';
                }
            }
        }
    }
}

}  // namespace
}  // namespace matcher
