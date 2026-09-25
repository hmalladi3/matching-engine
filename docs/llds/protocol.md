# Protocol (Input)

## Context and Design Philosophy

This component turns raw bytes from stdin into typed requests (`AddOrder`, `CancelOrder`), or into a precise `ParseError` that the app reports on stderr. It is where hostile input arrives, so it is where most of "ensure no input causes the application to crash" is enforced.

Principles:
- **Total functions.** Every byte sequence, including NUL bytes, invalid UTF-8, lines of any length, and a missing final newline, results in either a request or a diagnostic. There is no undefined behavior, no unbounded memory use, and no exceptions.
- **One bad line never affects another.** Errors are reported per line, and processing continues with the next line.
- **Throughput.** Input is read with `read(2)` into a large buffer and parsed in place from `string_view`s. There is no iostream, no `scanf`, and no per-line allocation (HLD decision 7).

## LineReader

```cpp
class LineReader {
public:
    explicit LineReader(ByteReader& source, std::size_t buffer_bytes = 1 << 20,
                        std::size_t max_line = 4096);   // ByteReader: read(2) seam (FdReader in production)
    // Next complete line (without '\n' and without a trailing '\r'), or:
    //   LineTooLong  – line exceeded max_line; its remaining bytes are discarded
    //   EndOfInput   – clean EOF
    //   ReadError    – read(2) failed (errno captured)
    Result next();
    std::uint64_t line_number() const;   // 1-based, counts every '\n'-terminated or final line
    bool has_buffered_line() const;      // false when next() would have to call read() (see output.md)
};
```

- **Buffering:** a single 1 MiB buffer, allocated once. `memchr` finds `'\n'`. When a line straddles the end of the buffer, the partial line is moved to the front with `memmove` and the rest of the buffer is refilled. Reading one byte at a time is supported and tested; this is how a pipe or terminal behaves.
- **Lines longer than 4096 bytes:** the limit applies to the raw line, *including* any `//` comment, because it is a memory-safety bound applied before any parsing. Such a line is reported once as `LineTooLong` with its line number and its first 80 raw bytes, which the reader keeps for the diagnostic. The reader then discards bytes until the next `'\n'` without storing them, so memory stays bounded however long the line is. A valid message is at most about 70 bytes, so 4096 is very generous.
- **Line endings:** `\n` and `\r\n` are both accepted, and one trailing `\r` is removed. A lone `\r` anywhere else is just a byte, and the parser rejects it.
- **End of file:**
  - A final line without a trailing `'\n'` is still returned as a line.
  - An empty input produces no requests and no diagnostics.
- **Read errors:** if `read` is interrupted (`EINTR`), it is retried. Any other error returns `ReadError`. The app reports it and exits with status 1 (see `output.md`).

## RequestParser

`ParseResult parse_request(std::string_view line) noexcept`, where `ParseResult = std::variant<AddOrder, CancelOrder, ParseError>`.

**Comments:** everything from the first `//` to the end of the line is removed before parsing. The brief's own example annotates its input this way (`BADMESSAGE // An erroneous input`), so the example works if pasted verbatim. No valid message contains `/`, so this can never change the meaning of a valid line.

**Whitespace:** the "whitespace" set is ASCII space, tab, and three invisible Unicode characters that appear when text is copied from documents:
- U+00A0 no-break space (UTF-8 `C2 A0`),
- U+200B zero-width space (`E2 80 8B`), which the brief's PDF itself contains inside its example lines,
- U+FEFF byte-order mark or zero-width no-break space (`EF BB BF`), which some editors put at the start of a file.

These characters are trimmed from the start and end of the line and around each field. Inside a field they are invalid, so `10 25` is rejected, not read as `1025`.

**Field splitting:** after comment removal and trimming, the line is split on `,`, and each field is trimmed, so `0, 123, 0, 9, 1000` is accepted.

**Blank lines:** a line that is empty after comment removal and trimming, such as an empty line or a line that is only a comment, is **skipped silently**. It carries no message.

**Diagnostic excerpts** show the line *after* comment removal and trimming. So `BADMESSAGE                // An erroneous input` is reported as `Unknown message type: BADMESSAGE`, which is the brief's exact text.

**Grammar and validation order:** the first failure decides the error.

| Step | Check | Error (`ParseError::Kind`) | Diagnostic text (after `line N: `) |
|---|---|---|---|
| 1 | Field 0 is `0` or `1` | `UnknownMessageType` | `Unknown message type: <line>` (the brief's wording) |
| 2 | Field count is 5 for Add, 2 for Cancel | `WrongFieldCount` | `AddOrderRequest expects 5 fields, got 3: <line>` |
| 3 | `orderid` is an unsigned decimal integer ≤ 2^64−1 | `BadOrderId` | `invalid orderid 'abc': <line>` / `orderid out of range` |
| 4 | `orderid` > 0 | `BadOrderId` | `orderid must be positive` |
| 5 (Add) | `side` is `0` or `1` | `BadSide` | `invalid side '2' (expected 0=Buy or 1=Sell)` |
| 6 (Add) | `quantity` is an unsigned integer ≤ 2^64−1 and > 0 | `BadQuantity` | `invalid quantity '-5'` / `quantity must be positive` / `quantity out of range` |
| 7 (Add) | `price` parses (`price.md`) | `BadPrice` | `invalid price '1e3': malformed` / `…: more than 8 decimal places` / `…: out of range` |

**Integer grammar:** `DIGIT+` only. No sign, no `+`, no decimal point, no exponent, no hex. Leading zeros are allowed (`007`). The value is accumulated with an overflow check against `UINT64_MAX`.

**Message type grammar:** exactly `0` or `1`, **after** trimming. Any other text, including `00`, `2`–`4` (output message types), `-1`, and `BADMESSAGE`, is an `UnknownMessageType`.

**Diagnostic excerpts:** the offending line is echoed after the reason. It is cut to 80 bytes plus `…`, and bytes outside printable ASCII are escaped as `\xHH`. This keeps stderr readable and safe for a terminal even when the input is binary.

**Scope of the parser:** it checks only what can be decided from the line itself. State-dependent rejections, meaning a duplicate live id, an unknown cancel id, and capacity, belong to the engine (`matching-engine.md`) and use the same diagnostic format.

## App Loop (the driver)

The loop lives in `run_app(args, in, out, err)` in the library, not in `main`. That lets tests drive the whole program in-process with scripted I/O. `main` only ignores SIGPIPE and connects file descriptors 0, 1 and 2.

```
main:
  ignore SIGPIPE                                   (see output.md)
  parse CLI: [--reserve N] [--help]; bad args or N ∉ [1, 2^31] → usage on stderr, exit 2
  construct engine; allocation failure → "cannot reserve memory for N orders", exit 1
                                  (the only place bad_alloc is caught; before any input is read)
  loop:
    r = reader.next()
    EndOfInput → break;  ReadError → report, exit 1
    LineTooLong → report "line N: line exceeds 4096 bytes: <first 80 raw bytes, escaped>…"; continue
    blank line  → continue
    parse → ParseError → report; continue
    engine.add / engine.cancel → Reject ≠ None → report "line N: <reject text>"
    if out.failed() → report, exit 1
    if !reader.has_buffered_line(): flush stdout and stderr   (see output.md: flush before blocking)
  out.flush(); exit (out.failed() ? 1 : 0)
```

The exit status is **0** whenever all input was consumed and all output was written, **even if some lines were rejected**. Rejected lines are normal, expected input, and the diagnostics say what happened to them.

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Input mechanism | `read(2)` + 1 MiB buffer + `memchr` | `std::getline`; `fgets`; `mmap` of stdin | iostreams are several times slower and allocate for every line. `mmap` does not work on pipes, and the brief pipes stdin. |
| Whitespace around fields | Trim ASCII space/tab plus U+00A0, U+200B, U+FEFF | Strict (no spaces); ASCII only; full Unicode whitespace tables | Hand-written test files often contain spaces, and the brief's own PDF puts zero-width spaces in its example. Accepting them loses nothing, because whitespace can never change a value. Three explicit byte sequences are enough for what copy-paste actually produces, without a Unicode dependency. |
| `//` comments | Removed from `//` to end of line | Not supported; `#` comments | The brief's example uses `//` annotations. Supporting them makes the brief's example input work verbatim. |
| Blank lines | Skip silently | Report as errors | A blank or comment-only line carries no request, so it is neither a message nor an error. Reporting it would be noise. |
| Maximum line length | 4096 bytes, and longer lines are reported and discarded as they stream | Unlimited, growing the buffer | Unlimited would let one hostile line exhaust memory. |
| Message type `00` | Rejected | Parsed as the integer 0 | The type is a tag, not a quantity. Strict matching keeps the diagnostic clear. |
| Where rejections are reported | Parser returns a `ParseError` value; the app formats it | Parser writes to stderr itself | Keeps the parser pure and unit-testable, with one place for diagnostic formatting. |
| Leading zeros in integers | Accepted | Rejected | `007` is still a positive integer, so rejecting it would be pedantic. |

## Open Questions & Future Decisions

### Resolved
1. ✅ The diagnostic for an unknown message type uses the brief's wording, `Unknown message type: …`.
3. ✅ The brief's example input, including its `//` annotations and the zero-width spaces from the PDF, is accepted exactly as written, and the golden test uses exactly that input.
2. ✅ The exit code is 0 when some lines are rejected, and non-zero only for I/O failure or bad command-line arguments.

### Deferred
1. A binary wire protocol, such as SBE or ITCH-style fixed layouts, instead of CSV. This is a production item for `PERFORMANCE.md`.

## References

- `docs/llds/price.md`: the price grammar
- `docs/llds/matching-engine.md`: the request types and engine rejections
- `docs/llds/output.md`: flushing, `ErrorReporter`, SIGPIPE
