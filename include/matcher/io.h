#pragma once

#include <cstddef>

namespace matcher {

// Minimal byte-stream seams over read(2)/write(2). Production code uses the
// fd-backed implementations; tests substitute scripted ones to exercise
// chunking, EINTR, partial writes and errors. One virtual call per system
// call (i.e. per ~1 MiB of input), never per message.

class ByteReader {
public:
    virtual ~ByteReader() = default;
    // Same contract as read(2): > 0 bytes read, 0 at end of input, < 0 on
    // error with `err` set to the errno value (EINTR included).
    virtual long read(char* buffer, std::size_t capacity, int& err) noexcept = 0;
};

class ByteWriter {
public:
    virtual ~ByteWriter() = default;
    // Same contract as write(2): >= 0 bytes written (possibly fewer than
    // requested), < 0 on error with `err` set to the errno value.
    virtual long write(const char* data, std::size_t size, int& err) noexcept = 0;
};

class FdReader final : public ByteReader {
public:
    explicit FdReader(int fd) noexcept : fd_(fd) {}
    long read(char* buffer, std::size_t capacity, int& err) noexcept override;

private:
    int fd_;
};

class FdWriter final : public ByteWriter {
public:
    explicit FdWriter(int fd) noexcept : fd_(fd) {}
    long write(const char* data, std::size_t size, int& err) noexcept override;

private:
    int fd_;
};

}  // namespace matcher
