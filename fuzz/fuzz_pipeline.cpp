// Feeds arbitrary bytes through the whole program: line reader, parser,
// engine, event writer and error reporter, checking book invariants after
// every request. Any crash, sanitizer report or invariant abort is a bug.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <variant>

#include "matcher/error_reporter.h"
#include "matcher/event_writer.h"
#include "matcher/line_reader.h"
#include "matcher/matching_engine.h"
#include "matcher/request_parser.h"

namespace {

class MemoryReader final : public matcher::ByteReader {
public:
    MemoryReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    long read(char* buffer, std::size_t capacity, int&) noexcept override {
        // Odd, data-dependent chunk sizes exercise line reassembly.
        std::size_t n = std::min({capacity, size_ - pos_, std::size_t{1} + (size_ - pos_) % 97});
        std::memcpy(buffer, data_ + pos_, n);
        pos_ += n;
        return static_cast<long>(n);
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

class NullWriter final : public matcher::ByteWriter {
public:
    long write(const char*, std::size_t size, int&) noexcept override { return static_cast<long>(size); }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace matcher;
    MemoryReader source(data, size);
    LineReader reader(source, 1024, 256);  // small limits reach the too-long paths quickly
    NullWriter sink;
    BufferedWriter out(sink, 256), err(sink, 256);
    EventWriter events(out);
    ErrorReporter errors(err);
    MatchingEngine<EventWriter> engine(events, BookConfig{2, 1, 64});  // tiny cap reaches CapacityExceeded

    for (;;) {
        const LineReader::Result r = reader.next();
        if (r.status == LineReader::Status::EndOfInput || r.status == LineReader::Status::ReadError) break;
        if (r.status == LineReader::Status::LineTooLong) {
            errors.line_too_long(reader.line_number(), r.text);
            continue;
        }
        const ParseResult parsed = parse_request(r.text);
        if (const auto* add = std::get_if<AddOrder>(&parsed)) {
            if (const Reject rej = engine.add(*add); rej != Reject::None)
                errors.reject(reader.line_number(), rej, add->id, clean_line(r.text));
        } else if (const auto* cancel = std::get_if<CancelOrder>(&parsed)) {
            if (const Reject rej = engine.cancel(*cancel); rej != Reject::None)
                errors.reject(reader.line_number(), rej, cancel->id, clean_line(r.text));
        } else if (const auto* error = std::get_if<ParseError>(&parsed)) {
            errors.parse_error(reader.line_number(), *error, clean_line(r.text));
        }
        engine.book().check_invariants();
    }
    return 0;
}
