#include "matcher/error_reporter.h"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "matcher/line_reader.h"

namespace matcher {

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026 "…"

// Formats one diagnostic directly into the output buffer. String literals are
// copied with compile-time sizes (inlined), so a diagnostic costs one buffer
// reservation instead of a library call per fragment.
class Line {
public:
    explicit Line(char* out) noexcept : out_(out) {}

    template <std::size_t N>
    void put(const char (&literal)[N]) noexcept {
        std::memcpy(out_, literal, N - 1);
        out_ += N - 1;
    }
    void put(std::string_view text) noexcept {
        std::memcpy(out_, text.data(), text.size());
        out_ += text.size();
    }
    void number(std::uint64_t value) noexcept { out_ = std::to_chars(out_, out_ + 20, value).ptr; }

    // Printable ASCII passes through; every other byte becomes \xHH, so binary
    // input cannot corrupt a terminal.
    void escaped(std::string_view text) noexcept {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (const char c : text) {
            if (c >= 0x20 && c <= 0x7E) {
                *out_++ = c;
            } else {
                const auto byte = static_cast<unsigned char>(c);
                *out_++ = '\\';
                *out_++ = 'x';
                *out_++ = kHex[byte >> 4];
                *out_++ = kHex[byte & 0xF];
            }
        }
    }
    void excerpt(std::string_view text, bool always_ellipsis) noexcept {
        escaped(text.substr(0, ErrorReporter::kExcerptBytes));
        if (always_ellipsis || text.size() > ErrorReporter::kExcerptBytes) put(kEllipsis);
    }
    void prefix(std::uint64_t line) noexcept {
        put("line ");
        number(line);
        put(": ");
    }
    void int_field_reason(std::string_view name, const ParseError& error) noexcept {
        switch (error.int_detail) {
            case ParseError::IntDetail::Malformed:
                put("invalid ");
                put(name);
                put(" '");
                excerpt(error.field, false);
                put("'");
                return;
            case ParseError::IntDetail::NotPositive:
                put(name);
                put(" must be positive");
                return;
            case ParseError::IntDetail::OutOfRange:
                put(name);
                put(" out of range");
                return;
        }
    }
    char* end() const noexcept { return out_; }

private:
    char* out_;
};

}  // namespace

std::string_view to_string(Reject reject) noexcept {
    switch (reject) {
        case Reject::None: return "none";
        case Reject::InvalidOrderId: return "invalid orderid";
        case Reject::InvalidQuantity: return "invalid quantity";
        case Reject::DuplicateOrderId: return "duplicate orderid";
        case Reject::UnknownOrderId: return "unknown orderid";
        case Reject::CapacityExceeded: return "capacity exceeded";
    }
    return "unknown";
}

// Formats straight into the output buffer when it can hold a whole diagnostic
// (the production writer holds 64 KiB); otherwise into `local`, then appends.
char* ErrorReporter::begin(char* local) noexcept {
    return err_.capacity() >= kMaxDiagnosticBytes ? err_.reserve(kMaxDiagnosticBytes) : local;
}

void ErrorReporter::finish(char* local, char* end) noexcept {
    if (err_.capacity() >= kMaxDiagnosticBytes)
        err_.commit(end);
    else
        err_.append({local, static_cast<std::size_t>(end - local)});
}

void ErrorReporter::parse_error(std::uint64_t line, const ParseError& error, std::string_view cleaned_line) noexcept {
    char local[kMaxDiagnosticBytes];
    Line out(begin(local));
    out.prefix(line);
    switch (error.kind) {
        case ParseError::Kind::UnknownMessageType: out.put("Unknown message type"); break;
        case ParseError::Kind::WrongFieldCount:
            if (error.is_add)
                out.put("AddOrderRequest expects ");
            else
                out.put("CancelOrderRequest expects ");
            out.number(error.expected_fields);
            out.put(" fields, got ");
            out.number(error.actual_fields);
            break;
        case ParseError::Kind::BadOrderId: out.int_field_reason("orderid", error); break;
        case ParseError::Kind::BadSide:
            out.put("invalid side '");
            out.excerpt(error.field, false);
            out.put("' (expected 0=Buy or 1=Sell)");
            break;
        case ParseError::Kind::BadQuantity: out.int_field_reason("quantity", error); break;
        case ParseError::Kind::BadPrice:
            out.put("invalid price '");
            out.excerpt(error.field, false);
            out.put("': ");
            out.put(describe(error.price_error));
            break;
    }
    out.put(": ");
    out.excerpt(cleaned_line, false);
    out.put("\n");
    finish(local, out.end());
    ++count_;
}

void ErrorReporter::reject(std::uint64_t line, Reject reason, OrderId id, std::string_view cleaned_line) noexcept {
    char local[kMaxDiagnosticBytes];
    Line out(begin(local));
    out.prefix(line);
    switch (reason) {
        case Reject::DuplicateOrderId:
            out.put("duplicate orderid ");
            out.number(id);
            out.put(" (an order with this id is still resting)");
            break;
        case Reject::UnknownOrderId:
            out.put("cannot cancel orderid ");
            out.number(id);
            out.put(": no resting order with this id");
            break;
        case Reject::CapacityExceeded: out.put("order rejected: order book capacity exhausted"); break;
        default:  // unreachable behind the parser
            out.put("internal error: ");
            out.put(to_string(reason));
            break;
    }
    out.put(": ");
    out.excerpt(cleaned_line, false);
    out.put("\n");
    finish(local, out.end());
    ++count_;
}

void ErrorReporter::line_too_long(std::uint64_t line, std::string_view raw_prefix) noexcept {
    char local[kMaxDiagnosticBytes];
    Line out(begin(local));
    out.prefix(line);
    out.put("line exceeds ");
    out.number(LineReader::kMaxLineBytes);
    out.put(" bytes: ");
    out.excerpt(raw_prefix, true);
    out.put("\n");
    finish(local, out.end());
    ++count_;
}

// Free-form messages have no length bound, so they go through append().
void ErrorReporter::message(std::string_view text) noexcept {
    err_.append(text);
    err_.append("\n");
    ++count_;
}

void ErrorReporter::system_error(std::string_view what, int errnum) noexcept {
    err_.append(what);
    err_.append(": ");
    err_.append(std::strerror(errnum));
    err_.append("\n");
    ++count_;
}

}  // namespace matcher
