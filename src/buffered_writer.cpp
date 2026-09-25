#include "matcher/buffered_writer.h"

#include <cerrno>
#include <unistd.h>

namespace matcher {

long FdWriter::write(const char* data, std::size_t size, int& err) noexcept {
    const ssize_t n = ::write(fd_, data, size);
    if (n < 0) err = errno;
    return static_cast<long>(n);
}

BufferedWriter::BufferedWriter(ByteWriter& sink, std::size_t buffer_bytes)
    : sink_(sink), buffer_(new char[buffer_bytes]), capacity_(buffer_bytes) {}
BufferedWriter::~BufferedWriter() = default;
void BufferedWriter::append(std::string_view) noexcept {}  // Phase 5 stub
char* BufferedWriter::reserve(std::size_t) noexcept { return buffer_.get(); }
void BufferedWriter::commit(char*) noexcept {}
void BufferedWriter::flush() noexcept {}

}  // namespace matcher
