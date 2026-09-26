# Output

## Context and Design Philosophy

This component turns engine events into the brief's CSV output messages on stdout, and rejections into diagnostics on stderr. It has to be fast, since formatting is part of every trade, and it must never throw, because sink callbacks are `noexcept` (`matching-engine.md`). It also has to deliver output **promptly**: a consumer at the other end of a pipe must not wait indefinitely for output that is sitting in a buffer.

## Wire Format

| Event | Line | Example |
|---|---|---|
| `Trade` | `2,<qty>,<price>` | `2,2,1025` |
| `OrderFullyFilled` | `3,<orderid>` | `3,1000005` |
| `OrderPartiallyFilled` | `4,<orderid>,<remaining qty>` | `4,1000008,1` |

- Each line ends with `\n`. There are no spaces or `\r`.
- Integers are unsigned decimal numbers with no leading zeros.
- The price is the shortest exact decimal (`price.md`).

## BufferedWriter

A fixed-size output buffer over a `ByteWriter`. `ByteWriter` is a minimal interface with the same contract as `write(2)`. In production it is `FdWriter(1)` or `FdWriter(2)`; tests substitute scripted writers that simulate partial writes, `EINTR` and `EPIPE`. That costs one virtual call per system call, never one per message. `EventWriter` (stdout) and `ErrorReporter` (stderr) each own one `BufferedWriter`.

```cpp
class BufferedWriter {
public:
    explicit BufferedWriter(ByteWriter& sink, std::size_t buffer_bytes = 1 << 16);
    void append(std::string_view) noexcept;
    char* reserve(std::size_t n) noexcept;  // direct formatting target; flushes first if needed
    void commit(char* end) noexcept;
    void flush() noexcept;
    bool failed() const noexcept;           // sticky: first write error seen
    int  error() const noexcept;            // errno of that error
};
```

- **Buffer:** one fixed buffer, 64 KiB by default, allocated in the constructor and never grown. `reserve(n)` flushes first if `n` bytes do not fit. Every caller requests at most one output line (≤ 64 bytes), so it always fits.
- **`flush()` behavior:**
  - It loops over `write(2)` until everything is written, handling partial writes.
  - It retries on `EINTR`.
  - Any other error sets a sticky `failed_` flag with the `errno`, and all further output is thrown away.
  - It never throws, and never calls `abort` or `exit`.
- **The destructor** calls `flush()`, but the app flushes explicitly and checks `failed()` before exiting. The destructor is only a safety net.

## EventWriter (the production `EventSink`)

```cpp
class EventWriter {
public:
    explicit EventWriter(BufferedWriter&);
    void on_trade(const Trade&) noexcept;
    void on_fully_filled(const OrderFullyFilled&) noexcept;
    void on_partially_filled(const OrderPartiallyFilled&) noexcept;
};
static_assert(EventSink<EventWriter>);
```

Each callback reserves 64 bytes in the buffer, then writes the line directly into it: the type digit and a comma, the integers via `std::to_chars` (C++17: no locale, no allocation, and among the fastest standard options), and the price via `format_price`. It never builds a temporary string.

## ErrorReporter

`void report(std::uint64_t line, std::string_view reason, std::string_view excerpt) noexcept` writes `line N: <reason>: <escaped excerpt>\n` to its own `BufferedWriter` on fd 2. The escaping rules are in `protocol.md`. `ErrorReporter` also formats the engine's `Reject` codes:

| Reject | Text |
|---|---|
| `DuplicateOrderId` | `duplicate orderid 123 (an order with this id is still resting)` |
| `UnknownOrderId` | `cannot cancel orderid 123: no resting order with this id` |
| `CapacityExceeded` | `order rejected: order book capacity exhausted` |
| `InvalidOrderId`, `InvalidQuantity` | Cannot occur behind the parser. They are reported as internal errors, which tests confirm is unreachable. |

## Flush Policy

The brief's consumer may be a pipe or a terminal. Output is flushed:

1. **Before the reader would block**, meaning its buffer contains no complete line and the next step is a `read(2)` that might wait. The line reader calls `read(2)` only in exactly that situation, so the app wraps stdin in a `FlushBeforeRead` reader that flushes both outputs immediately before delegating each read. There is no per-line check at all.
   - **Interactive use:** a person typing one line at a time sees each response as soon as they press Enter.
   - **Batch use:** a file with millions of lines is read 1 MiB at a time, so there is only about one `write` per 1 MiB of input.
2. **When the buffer is full.**
3. **At exit.**

stdout and stderr are flushed at the same points. Their relative order is not guaranteed, because they are separate streams, as with any Unix program.

## SIGPIPE and Exit Codes

- **`main` ignores SIGPIPE.** By default, writing to a pipe whose reader has exited kills the process with SIGPIPE, which from the outside looks like a crash. With SIGPIPE ignored, `write` returns `EPIPE` instead. The `BufferedWriter` records it, and the app loop stops at the next request boundary, reports `stdout write failed: Broken pipe` on stderr (which is best-effort, since stderr may also be closed), and exits with status 1.
- **stderr failures are ignored.** Diagnostics are best-effort. If stderr is closed or full, the app keeps processing, and the exit status is not affected. Halting the engine because nobody is reading its error log would be worse.
- **Exit codes:**
  - 0: all input was processed and all output written.
  - 1: an I/O failure (a stdin read error or a stdout write error), or failure to allocate the startup reservation.
  - 2: invalid command-line usage.

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Integer formatting | `std::to_chars` | A hand-written digit-pair table; `printf` | `to_chars` is already close to optimal. A hand-written table is a micro-gain that would have to be proven by benchmark first. `printf` parses a format string and handles locale on every call. |
| Flush trigger | Before blocking on input, plus when full, plus at exit | After every line; only when full | Flushing after every line costs one system call per output line. Flushing only when full stalls interactive users and pipe consumers indefinitely. Flushing before blocking gives batch throughput *and* prompt interactive output. |
| Write errors | Sticky flag, and the app exits 1 at a request boundary | Throw; `abort` | The sink callbacks are `noexcept`, and stopping at a request boundary means the book is never left half-updated. |
| Buffering stderr | Buffered, flushed at the same points as stdout | Unbuffered | A hostile input with millions of bad lines would otherwise make one system call per line. |

## Open Questions & Future Decisions

### Deferred
1. A binary output protocol and kernel-bypass networking (DPDK, ef_vi) for production. These go in `PERFORMANCE.md`.

## References

- `docs/llds/matching-engine.md`: event types and the `EventSink` concept
- `docs/llds/protocol.md`: the app loop, `would_block_next`, diagnostic escaping
- `docs/llds/price.md`: `format_price`
