# Output Specs

Source LLD: `docs/llds/output.md`.

## Wire Format (stdout)

- [ ] **OUT-FMT-001**: When the engine emits a TradeEvent, the event writer shall write `2,<quantity>,<price>\n` to stdout.
- [ ] **OUT-FMT-002**: When the engine emits OrderFullyFilled, the event writer shall write `3,<orderid>\n` to stdout.
- [ ] **OUT-FMT-003**: When the engine emits OrderPartiallyFilled, the event writer shall write `4,<orderid>,<remaining quantity>\n` to stdout.
- [ ] **OUT-FMT-004**: The event writer shall format integers in decimal with no sign, leading zeros, or padding, format prices with the shortest exact form (PRICE-FMT-001), and write no spaces or `\r`.

## Flushing

- [ ] **OUT-FLUSH-001**: When the line reader has no complete line buffered and would have to call `read(2)`, the application shall first flush the stdout and stderr buffers.
- [ ] **OUT-FLUSH-002**: When the space left in an output buffer is less than one line, the writer shall flush that buffer before writing.
- [ ] **OUT-FLUSH-003**: When the application exits, it shall flush stdout and stderr.

## Failure Handling

- [ ] **OUT-ERR-001**: When `write(2)` writes only part of the data or fails with `EINTR`, the writer shall continue until all buffered bytes are written.
- [ ] **OUT-ERR-002**: If `write(2)` fails with any other error, then the writer shall record the error, discard all further output without throwing, and report `failed()` as true from then on.
- [ ] **OUT-ERR-003**: When the stdout writer has failed, the application shall stop at the next request boundary, write `stdout write failed: <reason>` to stderr on a best-effort basis, and exit with status 1.
- [ ] **OUT-ERR-005**: If writing to stderr fails, then the application shall ignore the failure, continue processing, and leave the exit status unchanged, because diagnostics are best-effort.
- [ ] **OUT-ERR-004**: The application shall ignore SIGPIPE so that a closed stdout pipe surfaces as an `EPIPE` write error instead of terminating the process.

## Diagnostics (stderr)

- [ ] **OUT-DIAG-001**: When the engine rejects a request, the error reporter shall describe it as `duplicate orderid <id> (an order with this id is still resting)`, `cannot cancel orderid <id>: no resting order with this id`, or `order rejected: order book capacity exhausted` respectively.
