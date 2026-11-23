# Build a static site faster — fast drop-in for makefun-site 1.0.0

[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/rank.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/ChristaNavisa/site-generator-speed-01-run-20261122/reward.svg)](https://make.fun/competitions/site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0, entered in
[Build a static site faster, 22 November 2026](https://make.fun/competitions/site-generator-speed-01-run-20261122)
on make.fun. `solve.js` (dependency-free ES module) implements the same build —
front matter, template dialect, Markdown subset, slug rule and report — and
returns the identical output tree for every case, byte for byte; `main.ts`
reads every case from `/input/cases.json`, answers each with `run`, and writes
`/output/answers.json`.

## How it is faster

- Every template is compiled once per case into straight-line code over one
  shared scope object (includes become calls, loops become loops), instead of
  walking an AST with a scope chain per node. If code generation from strings
  is unavailable in the sandbox, the same parsed trees are rendered by an
  interpreter with identical semantics.
- HTML escaping is bulk: plain runs are escaped in one pass, with a fast path
  when a string holds nothing to escape, instead of escaping character by
  character.
- Inline Markdown is scanned with `indexOf`-driven searches for code spans,
  strong, emphasis and links, instead of stepping through every character.
- Slugs take an ASCII fast path (NFKD is the identity on ASCII) and are
  memoised within a case; each page's card is computed once and reused.
- Front matter is scanned line by line without splitting and re-joining the
  whole body; word counting is a single pass with no allocation.

## License

MIT. See [LICENSE](LICENSE).
