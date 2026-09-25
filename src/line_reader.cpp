#include "matcher/line_reader.h"

#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>

namespace matcher {

long FdReader::read(char* buffer, std::size_t capacity, int& err) noexcept {
    const ssize_t n = ::read(fd_, buffer, capacity);
    if (n < 0) err = errno;
    return static_cast<long>(n);
}

// @spec PROTO-READ-006
LineReader::LineReader(ByteReader& source, std::size_t buffer_bytes, std::size_t max_line_bytes)
    : source_(source), buffer_(new char[buffer_bytes]), capacity_(buffer_bytes), max_line_(max_line_bytes) {
    assert(buffer_bytes > max_line_bytes);
}

bool LineReader::fill() noexcept {
    for (;;) {
        int err = 0;
        const long n = source_.read(buffer_.get() + end_, capacity_ - end_, err);
        if (n > 0) {
            end_ += static_cast<std::size_t>(n);
            return true;
        }
        if (n == 0) {
            eof_ = true;
            return true;
        }
        if (err == EINTR) continue;
        error_ = err != 0 ? err : EIO;
        return false;
    }
}

void LineReader::start_prefix(std::string_view bytes) noexcept {
    prefix_len_ = std::min(bytes.size(), kTooLongPrefixBytes);
    std::memcpy(too_long_prefix_, bytes.data(), prefix_len_);
}

void LineReader::skip_rest_of_line() noexcept {
    // Discards bytes up to and including the next '\n' without storing them,
    // topping up the (up to 80-byte) prefix kept for the diagnostic.
    for (;;) {
        const char* start = buffer_.get() + begin_;
        const std::size_t available = end_ - begin_;
        const auto* newline = static_cast<const char*>(std::memchr(start, '\n', available));
        const std::size_t upto = newline ? static_cast<std::size_t>(newline - start) : available;
        const std::size_t take = std::min(kTooLongPrefixBytes - prefix_len_, upto);
        std::memcpy(too_long_prefix_ + prefix_len_, start, take);
        prefix_len_ += take;
        if (newline) {
            begin_ += upto + 1;
            return;
        }
        begin_ = end_ = 0;
        if (eof_ || !fill()) return;
    }
}

LineReader::Result LineReader::line(const char* start, std::size_t length) noexcept {
    ++line_number_;
    if (length > max_line_) {
        start_prefix({start, length});
        return {Status::LineTooLong, {too_long_prefix_, prefix_len_}, 0};
    }
    if (length > 0 && start[length - 1] == '\r') --length;  // CRLF
    return {Status::Line, {start, length}, 0};
}

// @spec PROTO-READ-001, PROTO-READ-002, PROTO-READ-003, PROTO-READ-004, PROTO-READ-005, PROTO-READ-007
LineReader::Result LineReader::next() noexcept {
    for (;;) {
        if (error_ != 0) return {Status::ReadError, {}, error_};

        const char* start = buffer_.get() + begin_;
        const std::size_t available = end_ - begin_;
        if (const auto* newline = static_cast<const char*>(std::memchr(start, '\n', available))) {
            const auto length = static_cast<std::size_t>(newline - start);
            begin_ += length + 1;
            return line(start, length);
        }
        if (eof_) {
            if (available == 0) return {Status::EndOfInput, {}, 0};
            begin_ = end_;
            return line(start, available);  // final line without '\n'
        }
        if (available > max_line_) {
            // Too long and no newline yet: report now and skip the rest of the
            // line as it streams in, so memory stays bounded.
            ++line_number_;
            start_prefix({start, available});
            begin_ = end_;
            skip_rest_of_line();
            return {Status::LineTooLong, {too_long_prefix_, prefix_len_}, 0};
        }
        // Need more bytes: move the partial line to the front and refill.
        if (begin_ > 0) {
            std::memmove(buffer_.get(), start, available);
            begin_ = 0;
            end_ = available;
        }
        (void)fill();
    }
}

// @spec OUT-FLUSH-001
bool LineReader::has_buffered_line() const noexcept {
    const std::size_t available = end_ - begin_;
    return eof_ || error_ != 0 || available > max_line_ ||
           std::memchr(buffer_.get() + begin_, '\n', available) != nullptr;
}

}  // namespace matcher
