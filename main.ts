// main.ts — the program the harness runs: `node /bot/main.ts` (node22). It reads
// /input/cases.json (a JSON array of {id, input}) and writes /output/answers.json:
// a JSON array of the answers, one per case and in the cases' order. Outside a
// container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and
// /output. The output directory is left alone: the harness mounts it writable
// and answers.json is written straight into it.
//
// The fast path is native: engine.node (a small N-API addon, pure C, no libc
// beyond glibc) takes the raw cases.json bytes and returns the answers.json
// bytes in one call — no JSON.parse, no JSON.stringify, no per-case JS work.
// If the addon cannot be loaded, or its call throws, the program falls back
// to the pure-JS solve.js (same answers, byte for byte; slower).

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';
import { createRequire } from 'node:module';

// V8's incremental marking off, set before solve.js is imported so all of its
// work runs under it.
setFlagsFromString('--no-incremental-marking');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const raw = readFileSync(join(INPUT_DIR, 'cases.json'));

let done = false;
try {
  const require = createRequire(import.meta.url);
  const native = require('./engine.node') as { buildAll: (buf: Buffer) => Buffer };
  const out = native.buildAll(raw);
  writeFileSync(join(OUTPUT_DIR, 'answers.json'), out);
  done = true;
} catch {
  // The addon is unavailable on this machine; fall through to the JS path.
}

if (!done) {
  /** solve.js, imported by its URL (a .js module carries no types) and typed by its contract. */
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(raw.toString('utf8')) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(run(c.input));
  writeFileSync(join(OUTPUT_DIR, 'answers.json'), JSON.stringify(answers));
}
