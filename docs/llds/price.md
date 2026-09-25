# Price

## Context and Design Philosophy

The brief defines price as a "decimal number" and gives no tick size, range, or sign. Price equality decides whether two orders share a level, so it must be **exact**. `Price` is a small value type that turns decimal text into an exact integer representation, compares with single integer instructions, and formats back as the shortest exact decimal.

Principles:
- **Exact or rejected, never rounded.** If an input cannot be represented exactly, it is an error with a precise reason.
- **Every value in range is a valid price.** That includes zero and negative prices (HLD Approach).
- **Zero cost on the hot path.** Parsing and formatting happen once per message. After that, everything is `int64_t` arithmetic.

## Representation

```cpp
class Price {
public:
    static constexpr int kDecimals = 8;
    static constexpr std::int64_t kScale = pow10(kDecimals);   // consteval helper
    static constexpr std::int64_t kMaxRaw = INT64_MAX;          // ±92233720368.54775807

    constexpr static Price from_raw(std::int64_t) noexcept;
    constexpr std::int64_t raw() const noexcept;
    friend constexpr auto operator<=>(Price, Price) = default;
private:
    std::int64_t raw_;   // value × 10^8
};
```

- **Symmetric range:** `|raw| ≤ INT64_MAX`. `INT64_MIN` is excluded, so negating a price can never overflow and positive and negative prices have the same limits.
- `static_assert(sizeof(Price) == 8 && std::is_trivially_copyable_v<Price>)`.

## Parsing

`std::optional<Price> parse_price(std::string_view, PriceError& why) noexcept`. It never throws and never reads outside the view. Its caller (the protocol layer) turns `why` into the diagnostic text.

**Accepted grammar:** `'-'? DIGIT+ ('.' DIGIT+)?`

| Input | Result | Rule |
|---|---|---|
| `1025`, `1025.5`, `0.00390625`, `-37.63`, `0`, `-0` | accepted | `-0` is normalized to `0` |
| `007`, `1025.50000000000` | accepted, as 7 and 1025.5 | Leading zeros are allowed. Fractional digits after the 8th are allowed **only if they are all zero**, since the value is still exact. |
| `1025.000000001` | `PriceError::TooPrecise` | A non-zero digit after the 8th place cannot be represented exactly |
| `92233720369` | `PriceError::OutOfRange` | Outside ±92233720368.54775807. Overflow is detected digit by digit, and nothing wraps. |
| `+5`, `.5`, `5.`, `1e3`, `0x10`, `inf`, `nan`, `1,5`, ` 5`, `5 `, `--5`, empty | `PriceError::Malformed` | Everything outside the grammar. Whitespace is trimmed by the protocol layer, not here (see `protocol.md`). |

**Algorithm:** a single pass. Integer digits are accumulated into `uint64_t` with an overflow check. Fractional digits are accumulated up to position 8, then scaled by `pow10(8 − n)` from a `constexpr` table. The sign and range are applied last. A price with no decimal point (the common case, as in every example in the brief) never enters the fractional branch.

## Formatting

`char* format_price(Price, char* out) noexcept` writes the **shortest exact decimal**:
- There is no exponent and no `+`.
- A `-` is written only for negative values.
- The integer part is always written, so `0.5` rather than `.5`.
- Fractional digits are written only if they are non-zero, with trailing zeros removed.

`kMaxFormattedLen = 21` bytes (`-92233720368.54775807`), so callers can size their buffers statically.

Round-trip property: for every in-range `Price p`, `parse(format(p)) == p`. This is fuzz-tested.

**Output does not echo the input text exactly.** `1025.50` comes out as `1025.5` and `007` as `7`. A price is a number, so the output uses its canonical form. Every example in the brief is already canonical.

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Representation | `int64` × 10^8 | `double`; `int64` with a per-run scale detected from input; `__int128`; decimal128 | HLD decision 4. A scale detected from input would need two passes, or a rescale mid-stream when a finer price appears. `__int128` doubles the size of every level entry and comparison, for range nobody needs. |
| Excess fractional zeros | Accepted if all zero | Reject anything past 8 fractional digits | `1.000000000` is exactly representable, and rejecting it would be pedantic. The rule is "exact or rejected". |
| Leading `+`, `.5`, `5.` | Rejected | Accepted leniently | They are unusual forms, and accepting them buys nothing. A strict grammar is easier to state, test, and fuzz. A clear error message makes the rejection cheap for the user. |
| Output form | Canonical, shortest exact | Echo the original text | Echoing would require storing the text with every order (memory and cache cost) and would make equal prices print differently. |
| `INT64_MIN` | Excluded | Allowed | Keeps the range symmetric, so negating never overflows. Losing 10^-8 of range costs nothing. |

## Open Questions & Future Decisions

### Resolved
1. ✅ 8 decimal places cover every real futures tick (HLD decision 4).
2. ✅ Zero and negative prices are valid.

### Deferred
1. A per-instrument tick size, meaning rejecting prices that are not a multiple of the tick. The brief has no instrument metadata. This belongs with stretch goal #1 (the dense ladder, which needs a tick size anyway).

## References

- `docs/high-level-design.md`: Approach, Key Design Decision 4
- `docs/llds/protocol.md`: the caller, which also handles whitespace trimming and diagnostic text
