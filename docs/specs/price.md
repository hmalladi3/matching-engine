# Price Specs

Source LLD: `docs/llds/price.md`. A price is stored as a signed 64-bit integer equal to the value × 10^8. The valid range is ±92233720368.54775807.

## Parsing

- [x] **PRICE-PARSE-001**: The price parser shall accept text matching `'-'? DIGIT+ ('.' DIGIT+)?` whose value is exactly representable with 8 decimal places and lies within ±92233720368.54775807.
- [x] **PRICE-PARSE-002**: The price parser shall accept leading zeros in the integer part and zero digits after the 8th decimal place.
- [x] **PRICE-PARSE-003**: If a price has a non-zero digit after the 8th decimal place, then the price parser shall reject it with `TooPrecise`.
- [x] **PRICE-PARSE-004**: If a price's magnitude exceeds 92233720368.54775807, then the price parser shall reject it with `OutOfRange`, without integer overflow.
- [x] **PRICE-PARSE-005**: If a price does not match the grammar (including a leading `+`, a missing integer or fraction digit, an exponent, hex, `inf`/`nan`, embedded whitespace, or empty text), then the price parser shall reject it with `Malformed`.
- [x] **PRICE-PARSE-006**: When parsing `-0` or any negative zero value, the price parser shall produce the same value as `0`.
- [x] **PRICE-PARSE-007**: The price parser shall accept zero and negative prices.

## Formatting and Comparison

- [x] **PRICE-FMT-001**: The price formatter shall write the shortest exact decimal form: a `-` only for negative values, the integer part always, and fractional digits only when non-zero, with trailing zeros removed and no exponent.
- [x] **PRICE-FMT-002**: For every in-range price p, parsing the formatted text of p shall produce p.
- [x] **PRICE-CMP-001**: Price comparison shall be exact integer comparison of the fixed-point values.
