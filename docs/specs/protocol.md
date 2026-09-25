# Protocol (Input) Specs

Source LLD: `docs/llds/protocol.md`. In these specs, *whitespace* means ASCII space, ASCII tab, U+00A0, U+200B, and U+FEFF (as UTF-8 byte sequences).

## Line Reading

- [ ] **PROTO-READ-001**: The line reader shall split input on `\n` and remove one trailing `\r` from each line.
- [ ] **PROTO-READ-002**: When input ends without a trailing `\n`, the line reader shall return the final partial line as a line.
- [ ] **PROTO-READ-003**: If a line exceeds 4096 bytes, including any comment, then the line reader shall report it once as `LineTooLong` with its line number, discard the rest of that line without storing it, and resume at the next line.
- [ ] **PROTO-READ-007**: The line reader shall count every line toward line numbers, including blank, comment-only, and too-long lines.
- [ ] **PROTO-READ-004**: The line reader shall return identical lines and line numbers however the input is split across `read(2)` calls, including one byte at a time.
- [ ] **PROTO-READ-005**: When `read(2)` fails with `EINTR`, the line reader shall retry. If it fails with any other error, then the line reader shall return `ReadError` with the errno.
- [ ] **PROTO-READ-006**: The line reader shall use a single input buffer allocated at construction and never grown.

## Request Parsing

- [ ] **PROTO-PARSE-001**: Before parsing a line, the request parser shall remove everything from the first `//` to the end of the line.
- [ ] **PROTO-PARSE-002**: The request parser shall trim whitespace from both ends of the line and of each comma-separated field, and shall treat whitespace inside a field as invalid.
- [ ] **PROTO-PARSE-003**: When a line is empty after comment removal and trimming, the request parser shall skip it without a diagnostic.
- [ ] **PROTO-PARSE-004**: If the first field is not exactly `0` or `1`, then the request parser shall return `UnknownMessageType`, which is reported as `Unknown message type: <line>`.
- [ ] **PROTO-PARSE-005**: If an AddOrderRequest does not have exactly 5 fields or a CancelOrderRequest does not have exactly 2 fields, then the request parser shall return `WrongFieldCount` stating the expected and actual counts.
- [ ] **PROTO-PARSE-006**: If an orderid is not an unsigned decimal integer from 1 to 2^64−1, then the request parser shall return `BadOrderId`.
- [ ] **PROTO-PARSE-007**: If an AddOrderRequest's side is not exactly `0` or `1`, then the request parser shall return `BadSide`.
- [ ] **PROTO-PARSE-008**: If an AddOrderRequest's quantity is not an unsigned decimal integer from 1 to 2^64−1, then the request parser shall return `BadQuantity`.
- [ ] **PROTO-PARSE-009**: If an AddOrderRequest's price is rejected by the price parser, then the request parser shall return `BadPrice` with the price parser's reason.
- [ ] **PROTO-PARSE-010**: When a line has several defects, the request parser shall report only the first in the order message type, field count, orderid, side, quantity, price.
- [ ] **PROTO-PARSE-011**: The request parser shall accept leading zeros in integer fields.

## Application Loop

- [ ] **PROTO-APP-001**: When a line is rejected by the line reader, the parser, or the engine, the application shall write exactly one diagnostic `line N: <reason>: <excerpt>` to stderr and continue with the next line.
- [ ] **PROTO-APP-002**: When writing a diagnostic excerpt, the application shall use the line after comment removal and trimming, truncated to 80 bytes plus `…`, with bytes outside printable ASCII escaped as `\xHH`. For a line rejected as too long, it shall instead use the line's first 80 raw bytes, escaped the same way and followed by `…`.
- [ ] **PROTO-APP-003**: The application shall exit with status 0 when all input was processed and all output written (whether or not some lines were rejected), 1 on a stdin read error, stdout write failure, or startup allocation failure, and 2 on invalid command-line usage.
- [ ] **PROTO-APP-004**: The application shall accept `--reserve N` to set the order reservation and `--help` to print usage. If any other argument is given, or N is not an integer from 1 to 2^31, then the application shall print usage to stderr and exit with status 2.
- [ ] **PROTO-APP-006**: If the initial reservation cannot be allocated at startup, then the application shall write `cannot reserve memory for N orders` to stderr and exit with status 1 without reading input.
- [ ] **PROTO-APP-005**: The application shall not crash, abort, or exhibit undefined behavior for any byte sequence on stdin.
