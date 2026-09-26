#include "matcher/buffered_writer.h"

#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>

namespace matcher {

long FdWriter::write(const char* data, std::size_t size, int& err) noexcept {
    const ssize_t n = ::write(fd_, data, size);
    if (n < 0) err = errno;
    return static_cast<long>(n);
}

BufferedWriter::BufferedWriter(ByteWriter& sink, std::size_t buffer_bytes)
    : sink_(sink), buffer_(new char[buffer_bytes]), capacity_(buffer_bytes) {}

BufferedWriter::~BufferedWriter() { flush(); }

void BufferedWriter::append(std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        if (size_ == capacity_) flush();
        if (failed()) return;  // output is discarded after the first error
        const std::size_t n = std::min(capacity_ - size_, bytes.size());
        std::memcpy(buffer_.get() + size_, bytes.data(), n);
        size_ += n;
        bytes.remove_prefix(n);
    }
}

// @spec OUT-FLUSH-002
char* BufferedWriter::reserve(std::size_t n) noexcept {
    assert(n <= capacity_);
    if (capacity_ - size_ < n) flush();
    return buffer_.get() + size_;
}

void BufferedWriter::commit(const char* end) noexcept {
    // After a failure, flush() has emptied the buffer and keeps it empty, so
    // formatted bytes are dropped rather than accumulated.
    size_ = failed() ? 0 : static_cast<std::size_t>(end - buffer_.get());
}

// @spec OUT-ERR-001, OUT-ERR-002
void BufferedWriter::flush() noexcept {
    std::size_t written = 0;
    while (written < size_ && !failed()) {
        int err = 0;
        const long n = sink_.write(buffer_.get() + written, size_ - written, err);
        if (n > 0)
            written += static_cast<std::size_t>(n);
        else if (n < 0 && err == EINTR)
            continue;
        else
            error_ = n < 0 && err != 0 ? err : EIO;  // a zero-byte write would loop forever
    }
    size_ = 0;
}

}  // namespace matcher
