# Build a static site faster — a streaming native engine for makefun-site 1.0.0

[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)

A native drop-in for makefun-site 1.0.0, entered in
[Build a static site faster, 22 November 2026](https://make.fun/competitions/site-generator-speed-01-run-20261122)
on make.fun. `engine.c` (pure C11, no third-party code) runs the same build —
front matter, template dialect, Markdown subset, slug rule and report — and
returns the identical output tree for every case, byte for byte.
`engine.node` exposes that engine as a Node-API addon; `main.ts` hands it
the raw `cases.json` bytes and writes back the `answers.json` bytes it
returns, with no JSON.parse and no JSON.stringify anywhere in the timed
region. `solve.js` is a dependency-free ES module with the same semantics,
kept as a fallback if the addon cannot load.

## Why it is fast

- The whole build is one C call. `build_all` streams over the input buffer:
  no JSON DOM, no per-value allocation (one bump arena per case), no garbage
  collection, and no JavaScript interpreter work at all in the timed region.
- Strings are views into the input wherever possible. A JSON string without
  a backslash is never copied; strings, lines and escapes are scanned 16
  bytes at a time with SSE2, and HTML/JSON escaping drains bulk runs without
  touching each character.
- Templates are compiled once per case into flat jump operations; rendering
  walks those operations and appends straight into the output buffer, so
  there is no per-page parse and no intermediate string building.
- Markdown is rendered in one fused pass per page, straight into the page
  buffer, with the table of contents and the excerpt recorded as spans of
  that buffer. Slugs come from a small embedded NFKD table (no Unicode
  library call per string), and word counts run through an SSE2 fast path.
- The driver reads and writes raw bytes only. The fixed costs the harness
  subtracts (process start, reading the input, writing the output) are the
  only JavaScript left.

## Correctness

The engine is checked against the reference implementation on the kit's
practice set (3 cases), 18 hand-built edge cases (escaped emphasis,
duplicate tags, CRLF, refusals, broken links, slug collisions) and randomly
generated cases: every answer identical, byte for byte. The JavaScript
fallback passes the same checks, so a load failure of the addon changes the
time, never the answer.

## Build

```sh
cc -O3 -march=x86-64 -fPIC -shared -I/usr/include/node engine.c napi.c -o engine.node
```

## License

MIT. See [LICENSE](LICENSE).
