#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "matcher/io.h"

namespace matcher {

// Splits a byte stream into lines using one fixed buffer. Returned views are
// valid until the next call to next().
class LineReader {
public:
    static constexpr std::size_t kDefaultBufferBytes = std::size_t{1} << 20;
    static constexpr std::size_t kMaxLineBytes = 4096;
    static constexpr std::size_t kTooLongPrefixBytes = 80;

    enum class Status { Line, LineTooLong, EndOfInput, ReadError };

    struct Result {
        Status status;
        // Line: the line without '\n' and one trailing '\r'.
        // LineTooLong: the line's first (up to) kTooLongPrefixBytes raw bytes.
        std::string_view text;
        int error = 0;  // ReadError: errno
    };

    // Precondition: buffer_bytes > max_line_bytes.
    explicit LineReader(ByteReader& source, std::size_t buffer_bytes = kDefaultBufferBytes,
                        std::size_t max_line_bytes = kMaxLineBytes);

    // @spec PROTO-READ-001, PROTO-READ-002, PROTO-READ-003, PROTO-READ-005
    Result next() noexcept;

    // 1-based number of the line most recently returned (Line or LineTooLong).
    std::uint64_t line_number() const noexcept { return line_number_; }

    // True if next() can return without calling read(), i.e. a complete line
    // is buffered or input has ended. The app flushes output when this is
    // false, just before it would block (OUT-FLUSH-001).
    bool has_buffered_line() const noexcept;

private:
    bool fill() noexcept;  // one read(); false on error (EINTR retried)
    Result line(const char* start, std::size_t length) noexcept;
    void start_prefix(std::string_view bytes) noexcept;
    void skip_rest_of_line() noexcept;

    ByteReader& source_;
    std::unique_ptr<char[]> buffer_;
    std::size_t capacity_;
    std::size_t max_line_;
    std::size_t begin_ = 0;  // start of unconsumed bytes
    std::size_t end_ = 0;    // one past the last valid byte
    std::uint64_t line_number_ = 0;
    bool eof_ = false;
    int error_ = 0;  // sticky read error
    char too_long_prefix_[kTooLongPrefixBytes] = {};
    std::size_t prefix_len_ = 0;
};

}  // namespace matcher
