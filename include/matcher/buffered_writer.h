#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "matcher/io.h"

namespace matcher {

// Fixed-size output buffer over a ByteWriter. Never throws and never grows.
// The first write error is sticky: later output is discarded and failed()
// stays true, so callers check once per request instead of per byte.
class BufferedWriter {
public:
    static constexpr std::size_t kDefaultBytes = std::size_t{1} << 16;

    explicit BufferedWriter(ByteWriter& sink, std::size_t buffer_bytes = kDefaultBytes);
    ~BufferedWriter();
    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    // Appends bytes, flushing as needed; any length is accepted.
    void append(std::string_view bytes) noexcept;

    // Returns space for at least `n` bytes (flushing first if needed) to
    // format into directly; finish with commit(). Precondition: n <= capacity.
    // @spec OUT-FLUSH-002
    [[nodiscard]] char* reserve(std::size_t n) noexcept;
    void commit(char* end) noexcept;

    // Writes everything buffered, handling partial writes and EINTR.
    // @spec OUT-ERR-001, OUT-ERR-002
    void flush() noexcept;

    bool failed() const noexcept { return error_ != 0; }
    int error() const noexcept { return error_; }
    std::size_t buffered() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    ByteWriter& sink_;
    std::unique_ptr<char[]> buffer_;
    std::size_t capacity_;
    std::size_t size_ = 0;
    int error_ = 0;
};

}  // namespace matcher
