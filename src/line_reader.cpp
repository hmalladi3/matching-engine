#include "matcher/line_reader.h"

#include <cerrno>
#include <unistd.h>

namespace matcher {

long FdReader::read(char* buffer, std::size_t capacity, int& err) noexcept {
    const ssize_t n = ::read(fd_, buffer, capacity);
    if (n < 0) err = errno;
    return static_cast<long>(n);
}

LineReader::LineReader(ByteReader& source, std::size_t buffer_bytes, std::size_t max_line_bytes)
    : source_(source), buffer_(new char[buffer_bytes]), capacity_(buffer_bytes),
      max_line_(max_line_bytes) {}  // Phase 5 stub

LineReader::Result LineReader::next() noexcept { return {Status::EndOfInput, {}, 0}; }
bool LineReader::has_buffered_line() const noexcept { return true; }

}  // namespace matcher
