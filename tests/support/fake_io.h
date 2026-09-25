#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "matcher/io.h"

namespace matcher::test {

// Delivers `data` in reads of at most `chunk` bytes. Optionally returns EINTR
// before every real read, and/or fails with `fail_errno` once `fail_at` bytes
// have been delivered.
class ScriptedReader final : public ByteReader {
public:
    explicit ScriptedReader(std::string data, std::size_t chunk = static_cast<std::size_t>(-1))
        : data_(std::move(data)), chunk_(chunk) {}

    void interrupt_every_read() { eintr_ = true; }
    void fail_at(std::size_t offset, int err) {
        fail_at_ = offset;
        fail_errno_ = err;
    }

    long read(char* buffer, std::size_t capacity, int& err) noexcept override {
        ++calls_;
        if (eintr_ && !interrupted_) {
            interrupted_ = true;
            err = EINTR;
            return -1;
        }
        interrupted_ = false;
        if (pos_ >= fail_at_) {
            err = fail_errno_;
            return -1;
        }
        const std::size_t limit = std::min({capacity, chunk_, data_.size() - pos_, fail_at_ - pos_});
        std::copy_n(data_.data() + pos_, limit, buffer);
        pos_ += limit;
        return static_cast<long>(limit);
    }

    std::size_t calls() const { return calls_; }
    std::size_t consumed() const { return pos_; }

private:
    std::string data_;
    std::size_t chunk_;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
    bool eintr_ = false;
    bool interrupted_ = false;
    std::size_t fail_at_ = static_cast<std::size_t>(-1);
    int fail_errno_ = 0;
};

// Records everything written. Can accept at most `max_per_call` bytes per
// write, return EINTR before every write, or fail with `fail_errno` once
// `fail_after` bytes have been accepted.
class RecordingWriter final : public ByteWriter {
public:
    void limit_per_call(std::size_t n) { max_per_call_ = n; }
    void interrupt_every_write() { eintr_ = true; }
    void fail_after(std::size_t bytes, int err) {
        fail_after_ = bytes;
        fail_errno_ = err;
    }

    long write(const char* data, std::size_t size, int& err) noexcept override {
        ++calls_;
        if (eintr_ && !interrupted_) {
            interrupted_ = true;
            err = EINTR;
            return -1;
        }
        interrupted_ = false;
        if (data_.size() >= fail_after_) {
            err = fail_errno_;
            return -1;
        }
        const std::size_t n = std::min({size, max_per_call_, fail_after_ - data_.size()});
        data_.append(data, n);
        return static_cast<long>(n);
    }

    const std::string& data() const { return data_; }
    std::size_t calls() const { return calls_; }

    // Splits the recorded output into lines (without '\n').
    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i < data_.size(); ++i) {
            if (data_[i] == '\n') {
                out.emplace_back(data_, start, i - start);
                start = i + 1;
            }
        }
        if (start < data_.size()) out.emplace_back(data_, start);
        return out;
    }

private:
    std::string data_;
    std::size_t calls_ = 0;
    std::size_t max_per_call_ = static_cast<std::size_t>(-1);
    bool eintr_ = false;
    bool interrupted_ = false;
    std::size_t fail_after_ = static_cast<std::size_t>(-1);
    int fail_errno_ = 0;
};

}  // namespace matcher::test
