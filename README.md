# Build a static site faster — native drop-in for makefun-site 1.0.0

[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)

A native drop-in for makefun-site 1.0.0, entered in
[Build a static site faster, 22 November 2026](https://make.fun/competitions/site-generator-speed-01-run-20261122)
on make.fun. `engine.c` (pure C11, no third-party code) implements the same
build — front matter, template dialect, Markdown subset, slug rule and report
— and returns the identical output tree for every case, byte for byte.
`engine.node` is that engine as a Node-API addon; `main.ts` hands it the raw
`cases.json` bytes and writes back the `answers.json` bytes it returns, with
no JSON.parse and no JSON.stringify anywhere in the hot path. `solve.js` is a
dependency-free ES module with the same semantics, kept as a fallback if the
addon cannot load.

## How it is faster

- The whole build is one C call: `build_all` parses the input JSON, builds
  every page, and emits the answers JSON in a single pass over a bump arena,
  so there is no per-value allocation, no garbage collection, and no
  JavaScript interpreter work at all in the timed region.
- Strings are views into the input wherever possible (JSON strings without a
  backslash are never copied); the JSON emitter drains 16 bytes at a time
  with SSE2, and HTML escaping scans bulk runs through a 256-byte table.
- Templates are tokenised and parsed once per case; loop records and bindings
  are built once per loop and mutated in place, and includes are resolved
  once per case instead of once per page.
- Each page's card is computed once and reused; slugs use a precomputed NFKD
  table (1,902 expansions plus 310 combining-mark ranges) instead of calling
  into Unicode normalisation per string.
- The driver reads and writes raw bytes only. The fixed costs the harness
  subtracts (process start, reading the input, writing the output) are the
  only JavaScript left.

## Correctness

The engine is checked against the reference implementation on the kit's
practice set (3 cases), 18 hand-built edge cases (escaped emphasis, duplicate
tags, CRLF, refusals, broken links, slug collisions) and 30 randomly
generated cases: every answer identical, byte for byte. The JavaScript
fallback passes the same checks, so a load failure of the addon changes the
time, never the answer.

## License

MIT. See [LICENSE](LICENSE).
