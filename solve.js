// solve.js — a native drop-in for makefun-site 1.0.0.
//
// Written fresh for site-generator-speed-01-run-20261122 round 2. run(input)
// returns, for one case's { site, templates, files }, the same output tree
// makefun-site 1.0.0's build returns: every path it writes and that file's
// text, byte for byte. No dependencies, no I/O, no clock, no randomness: the
// answer is a function of the input alone.
//
// Speed comes from a compiled native engine (engine.node, a Node-API module
// built from engine.c): main.ts hands it the raw cases.json bytes and gets
// the answers.json bytes back with no JSON.parse / JSON.stringify round-trip
// in JavaScript at all. The engine is a single-pass C port of the reference:
// bump-arena allocation, bulk HTML escaping, SSE2 JSON escaping, template
// includes resolved once per case, and per-source cards computed once.
//
// If the native module cannot load (a different libc or architecture), the
// pure-JavaScript run() below is the fallback. It is a complete, independent
// port of the same reference semantics — slower, but byte-identical — so the
// entry still answers every case correctly.

import { createRequire } from 'node:module';

// ---------------------------------------------------------------- native ---

const require = createRequire(import.meta.url);

let native = null;
try {
  native = require('./engine.node');
} catch {
  native = null;
}

/** The raw-bytes fast path: cases.json bytes in, answers.json bytes out. */
export function buildAll(buffer) {
  if (!native || typeof native.buildAll !== 'function') {
    throw new Error('native engine unavailable');
  }
  return native.buildAll(buffer);
}

// ------------------------------------------------------------- text utils ---

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const RESERVED = ['posts', 'tags', 'page'];
const SLUG_MAX = 64;

const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June', 'July',
  'August', 'September', 'October', 'November', 'December',
];

function isJsSpace(ch) {
  if (ch === ' ' || ch === '\t' || ch === '\n' || ch === '\v' || ch === '\f' || ch === '\r') return true;
  const cp = ch.codePointAt(0);
  return (
    cp === 0x00a0 || cp === 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
    cp === 0x2028 || cp === 0x2029 || cp === 0x202f || cp === 0x205f ||
    cp === 0x3000 || cp === 0xfeff
  );
}

function jsTrim(s) {
  let a = 0;
  let b = s.length;
  while (a < b && isJsSpace(s[a])) a++;
  while (b > a && isJsSpace(s[b - 1])) b--;
  return s.slice(a, b);
}

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

/** HTML escaping: & < > " ' become entities, nothing else. Bulk runs. */
function esc(s) {
  let out = '';
  let i = 0;
  while (i < s.length) {
    let j = i;
    while (j < s.length && !ESCAPES[s[j]]) j++;
    if (j > i) out += s.slice(i, j);
    if (j >= s.length) break;
    out += ESCAPES[s[j]];
    i = j + 1;
  }
  return out;
}

const COMBINING = /\p{M}/gu;

/** The slug rule: NFKD, drop combining marks, lowercase A-Z, each run of
 *  non [a-z0-9] becomes one '-', none at either end, cut to 64 chars with a
 *  trailing '-' at the cut dropped, 'untitled' when nothing is left. */
function slugify(text) {
  const plain = text.normalize('NFKD').replace(COMBINING, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, SLUG_MAX).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  if (m === 4 || m === 6 || m === 9 || m === 11) return 30;
  return 31;
}

/** '2026-11-09' -> '9 November 2026'. */
function longDate(date) {
  const y = Number(date.slice(0, 4));
  const m = Number(date.slice(5, 7));
  const d = Number(date.slice(8, 10));
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (const ch of text) {
    if (isJsSpace(ch)) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}


// ------------------------------------------------------------ front matter ---

const FM_KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^\d{4}-\d{2}-\d{2}$/;

/** Parse one front-matter value. Returns { ok, value } or { ok:false, reason }. */
function fmValue(key, raw) {
  if (key === 'title' || key === 'summary') {
    if (raw.startsWith('"')) {
      if (raw.length < 2 || !raw.endsWith('"')) return { ok: false, reason: `key "${key}": a quoted string is not closed` };
      const inner = raw.slice(1, -1);
      let out = '';
      for (let i = 0; i < inner.length; i++) {
        const c = inner[i];
        if (c === '\\') {
          if (i + 1 >= inner.length) return { ok: false, reason: `key "${key}": a quoted string holds an escape other than \\" or \\\\` };
          const e = inner[i + 1];
          if (e === '"' || e === '\\') {
            out += e;
            i++;
          } else {
            return { ok: false, reason: `key "${key}": a quoted string holds an escape other than \\" or \\\\` };
          }
        } else if (c === '"') {
          return { ok: false, reason: `key "${key}": a quoted string holds a stray quote` };
        } else {
          out += c;
        }
      }
      return { ok: true, value: out };
    }
    if (raw.startsWith('[')) return { ok: false, reason: `key "${key}": a bare string opens on [` };
    return { ok: true, value: raw };
  }
  if (key === 'date') {
    if (!FM_DATE.test(raw)) return { ok: false, reason: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
    const y = Number(raw.slice(0, 4));
    const m = Number(raw.slice(5, 7));
    const d = Number(raw.slice(8, 10));
    if (y < 1970 || y > 2099 || m < 1 || m > 12 || d < 1 || d > daysIn(y, m)) {
      return { ok: false, reason: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
    }
    return { ok: true, value: raw };
  }
  if (key === 'tags') {
    if (!raw.startsWith('[') || !raw.endsWith(']')) return { ok: false, reason: 'key "tags": not a list [a, b]' };
    const inner = raw.slice(1, -1);
    if (jsTrim(inner) === '') return { ok: true, value: [] };
    const parts = inner.split(',');
    const tags = [];
    for (const p of parts) {
      const t = jsTrim(p);
      if (t === '' || t.includes('"') || t.includes('[') || t.includes(']')) {
        return { ok: false, reason: 'key "tags": an empty tag, or one holding " [ or ]' };
      }
      tags.push(t);
    }
    return { ok: true, value: tags };
  }
  if (key === 'draft') {
    if (raw === 'true') return { ok: true, value: true };
    if (raw === 'false') return { ok: true, value: false };
    return { ok: false, reason: 'key "draft": true or false' };
  }
  if (key === 'layout') {
    if (!/^[a-z0-9-]+$/.test(raw)) return { ok: false, reason: 'key "layout": not a template name' };
    return { ok: true, value: raw };
  }
  if (key === 'slug') {
    if (!/^[a-z0-9]+(-[a-z0-9]+)*$/.test(raw) || raw.length > 64) {
      return { ok: false, reason: 'key "slug": not a slug' };
    }
    return { ok: true, value: raw };
  }
  return { ok: false, reason: `unknown key "${key}"` };
}

/**
 * Parse a source's front matter. Returns { ok, fm, body } or
 * { ok:false, reason }. fm holds title/summary/date/tags/draft/layout/slug
 * plus has_* flags; body is the text after the closing --- line.
 */
function parseFm(text) {
  const lines = text.split('\n');
  if (lines.length === 0 || lines[0] !== '---') return { ok: false, reason: 'no front matter' };
  const fm = {
    title: '', has_title: false, summary: '', has_summary: false,
    date: '', has_date: false, tags: [], has_tags: false,
    draft: false, has_draft: false, layout: '', has_layout: false,
    slug: '', has_slug: false,
  };
  const seen = new Set();
  let i = 1;
  let closed = false;
  for (; i < lines.length; i++) {
    const line = lines[i];
    if (line === '---') {
      closed = true;
      i++;
      break;
    }
    if (/^[ \t]*$/.test(line)) continue;
    if (line.startsWith('#')) continue;
    const m = FM_LINE.exec(line);
    if (!m) return { ok: false, reason: `line ${i + 1} is not "key: value"` };
    const key = m[1];
    const raw = m[2] === undefined ? '' : m[2].replace(/[ \t]+$/, '');
    if (!FM_KEYS.includes(key)) return { ok: false, reason: `unknown key "${key}"` };
    if (seen.has(key)) return { ok: false, reason: `key "${key}" is given twice` };
    seen.add(key);
    if (raw === '') return { ok: false, reason: `key "${key}" has no value` };
    const v = fmValue(key, raw);
    if (!v.ok) return { ok: false, reason: v.reason };
    if (key === 'title') { fm.title = v.value; fm.has_title = true; }
    else if (key === 'summary') { fm.summary = v.value; fm.has_summary = true; }
    else if (key === 'date') { fm.date = v.value; fm.has_date = true; }
    else if (key === 'tags') { fm.tags = v.value; fm.has_tags = true; }
    else if (key === 'draft') { fm.draft = v.value; fm.has_draft = true; }
    else if (key === 'layout') { fm.layout = v.value; fm.has_layout = true; }
    else if (key === 'slug') { fm.slug = v.value; fm.has_slug = true; }
  }
  if (!closed) return { ok: false, reason: 'the front matter is not closed' };
  if (!fm.has_title) return { ok: false, reason: 'no title' };
  return { ok: true, fm, body: lines.slice(i).join('\n') };
}

// ---------------------------------------------------------------- markdown ---

const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);
const INLINE_STOP = new Set(['\\', '`', '*', '[', '&', '<', '>', '"', "'"]);

/** Index of the next unescaped occurrence of needle at or after from, or -1.
 *  Escape pairs are consumed left to right: a backslash only escapes the
 *  next char when that char is escapable. */
function findUnescaped(s, needle, from) {
  let i = from;
  while (i < s.length) {
    if (s[i] === '\\' && i + 1 < s.length && ESCAPABLE.has(s[i + 1])) {
      i += 2;
      continue;
    }
    if (s.startsWith(needle, i)) return i;
    i++;
  }
  return -1;
}

/** Index of the next single '*' (one not beside another '*') at/after from, or -1. */
function findSingleStar(s, from) {
  let i = from;
  while (i < s.length) {
    if (s[i] === '\\' && i + 1 < s.length && ESCAPABLE.has(s[i + 1])) {
      i += 2;
      continue;
    }
    if (s[i] === '*') {
      if (s[i + 1] === '*') {
        i += 2;
        continue;
      }
      return i;
    }
    i++;
  }
  return -1;
}

function strHasJsSpace(s) {
  for (const ch of s) if (isJsSpace(ch)) return true;
  return false;
}

/** Render inline Markdown. env.link(target) resolves a link target to a URL. */
function inlineAppend(out, s, env) {
  let i = 0;
  while (i < s.length) {
    let j = i;
    while (j < s.length && !INLINE_STOP.has(s[j])) j++;
    if (j > i) out.push(s.slice(i, j));
    if (j >= s.length) return;
    i = j;
    const c = s[i];
    if (c === '\\') {
      if (i + 1 < s.length && ESCAPABLE.has(s[i + 1])) {
        out.push(esc(s[i + 1]));
        i += 2;
      } else {
        out.push('\\');
        i++;
      }
      continue;
    }
    if (c === '`') {
      const k = s.indexOf('`', i + 1);
      if (k > i + 1) {
        out.push('<code>');
        out.push(esc(s.slice(i + 1, k)));
        out.push('</code>');
        i = k + 1;
      } else {
        out.push('`');
        i++;
      }
      continue;
    }
    if (c === '*') {
      if (i + 1 < s.length && s[i + 1] === '*') {
        const k = findUnescaped(s, '**', i + 2);
        if (k > i + 2) {
          out.push('<strong>');
          inlineAppend(out, s.slice(i + 2, k), env);
          out.push('</strong>');
          i = k + 2;
        } else {
          out.push('**');
          i += 2;
        }
        continue;
      }
      const k = findSingleStar(s, i + 1);
      if (k > i + 1) {
        out.push('<em>');
        inlineAppend(out, s.slice(i + 1, k), env);
        out.push('</em>');
        i = k + 1;
      } else {
        out.push('*');
        i++;
      }
      continue;
    }
    if (c === '[') {
      const k = findUnescaped(s, ']', i + 1);
      let m = -1;
      if (k !== -1 && k + 1 < s.length && s[k + 1] === '(') {
        const close = s.indexOf(')', k + 2);
        if (close !== -1) m = close;
      }
      const target = m === -1 ? '' : s.slice(k + 2, m);
      if (target.length > 0 && !strHasJsSpace(target)) {
        const href = env.link(target);
        out.push('<a href="');
        out.push(esc(href));
        out.push('">');
        inlineAppend(out, s.slice(i + 1, k), env);
        out.push('</a>');
        i = m + 1;
        continue;
      }
      out.push('[');
      i++;
      continue;
    }
    out.push(ESCAPES[c]);
    i++;
  }
}

function isBlank(line) {
  return /^[ \t]*$/.test(line);
}

function fenceOpen(line) {
  if (!line.startsWith('```')) return null;
  const rest = line.slice(3);
  if (!/^[A-Za-z0-9_+-]*[ \t]*$/.test(rest)) return null;
  return rest.replace(/[ \t]+$/, '');
}

function fenceClose(line) {
  return line.startsWith('```') && /^[ \t]*$/.test(line.slice(3));
}

function headingOf(line) {
  const m = /^(#{1,6})[ \t]+(.*?)[ \t]*$/.exec(line);
  if (!m || m[2] === '') return null;
  return { depth: m[1].length, text: m[2] };
}

function isHr(line) {
  return /^(---|\*\*\*)[ \t]*$/.test(line);
}

function bulletOf(line) {
  if (line.startsWith('- ') || line.startsWith('* ')) return line.slice(2);
  return null;
}

function orderedOf(line) {
  const m = /^(\d+)\. (.*)$/.exec(line);
  if (!m || m[1].length > 9) return null;
  return { num: Number(m[1]), text: m[2] };
}

/** Split a body into blocks, then render each to HTML. Returns { html, toc, excerpt }. */
function renderMd(src, env, slugSet) {
  const lines = src.split('\n');
  const out = [];
  const toc = [];
  let excerpt = '';
  let haveExcerpt = false;
  let i = 0;
  const inline = (s) => {
    const parts = [];
    inlineAppend(parts, s, env);
    return parts.join('');
  };
  while (i < lines.length) {
    const line = lines[i];
    if (isBlank(line)) {
      i++;
      continue;
    }
    const lang = fenceOpen(line);
    if (lang !== null) {
      i++;
      const body = [];
      while (i < lines.length && !fenceClose(lines[i])) body.push(lines[i++]);
      i++;
      out.push('<pre><code' + (lang !== '' ? ` class="language-${esc(lang)}"` : '') + '>');
      for (const l of body) out.push(esc(l) + '\n');
      out.push('</code></pre>\n');
      continue;
    }
    const h = headingOf(line);
    if (h) {
      const slug = slugify(h.text);
      let id = slug;
      let n = 2;
      while (slugSet.has(id)) id = `${slug}-${n++}`;
      slugSet.add(id);
      const html = inline(h.text);
      out.push(`<h${h.depth} id="${id}">${html}</h${h.depth}>\n`);
      if (h.depth === 2 || h.depth === 3) toc.push({ level: h.depth, id, html });
      i++;
      continue;
    }
    if (isHr(line)) {
      out.push('<hr>\n');
      i++;
      continue;
    }
    if (line.startsWith('>')) {
      const qlines = [];
      while (i < lines.length && lines[i].startsWith('>')) {
        let l = lines[i++];
        let st = 1;
        if (st < l.length && l[st] === ' ') st++;
        const tl = jsTrim(l.slice(st));
        if (tl !== '') qlines.push(tl);
      }
      if (qlines.length === 0) {
        out.push('<blockquote>\n</blockquote>\n');
      } else {
        out.push('<blockquote>\n<p>' + inline(qlines.join('\n')) + '</p>\n</blockquote>\n');
      }
      continue;
    }
    const bitem = bulletOf(line);
    const oitem = bitem === null ? orderedOf(line) : null;
    if (bitem !== null || oitem !== null) {
      const ordered = oitem !== null;
      const firstNum = ordered ? oitem.num : 0;
      const items = [];
      let cur = null;
      const flush = () => {
        if (cur !== null) {
          items.push(cur);
          cur = null;
        }
      };
      while (i < lines.length) {
        const li = lines[i];
        const bi = ordered ? null : bulletOf(li);
        const oi = ordered ? orderedOf(li) : null;
        if ((ordered && oi !== null) || (!ordered && bi !== null)) {
          flush();
          cur = ordered ? oi.text : bi;
          i++;
          continue;
        }
        if (li.length >= 2 && li[0] === ' ' && li[1] === ' ' && li.slice(2).trim() !== '' && cur !== null) {
          cur += '\n' + jsTrim(li);
          i++;
          continue;
        }
        break;
      }
      flush();
      if (ordered && firstNum !== 1) out.push(`<ol start="${firstNum}">\n`);
      else out.push(ordered ? '<ol>\n' : '<ul>\n');
      for (const it of items) out.push('<li>' + inline(it) + '</li>\n');
      out.push(ordered ? '</ol>\n' : '</ul>\n');
      continue;
    }
    // Paragraph: this line plus following lines up to a blank line, a fence,
    // a heading, or a thematic break.
    const plines = [jsTrim(line)];
    i++;
    while (i < lines.length) {
      const l = lines[i];
      if (isBlank(l) || fenceOpen(l) !== null || headingOf(l) !== null || isHr(l)) break;
      plines.push(jsTrim(l));
      i++;
    }
    const html = inline(plines.join('\n'));
    if (!haveExcerpt) {
      excerpt = html;
      haveExcerpt = true;
    }
    out.push('<p>' + html + '</p>\n');
  }
  return { html: out.join(''), toc, excerpt };
}


// ---------------------------------------------------------------- templates ---

const INCLUDE_DEPTH = 8;

function tplErr(name, msg) {
  return `template "${name}": ${msg}`;
}

/** Is the string a valid template path (names joined by '.')? */
function parsePath(text) {
  const parts = text.split('.');
  if (parts.length === 0) return null;
  for (const p of parts) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(p)) return null;
  }
  return parts;
}

function textOf(v) {
  if (v === undefined || v === null) return '';
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map((x) => textOf(x)).join(', ');
  return '';
}

function truthy(v) {
  if (v === undefined || v === null) return false;
  if (v === false) return false;
  if (v === '') return false;
  if (v === 0) return false;
  if (Array.isArray(v) && v.length === 0) return false;
  return true;
}

/** Look a path up: first name from the innermost loop binding, else the scope. */
function lookup(scope, path) {
  let v;
  let found = false;
  for (let s = scope.length - 1; s >= 0; s--) {
    if (Object.prototype.hasOwnProperty.call(scope[s], path[0])) {
      v = scope[s][path[0]];
      found = true;
      break;
    }
  }
  if (!found) return undefined;
  for (let k = 1; k < path.length; k++) {
    const seg = path[k];
    if (v !== null && typeof v === 'object' && !Array.isArray(v)) {
      if (!Object.prototype.hasOwnProperty.call(v, seg)) return undefined;
      v = v[seg];
    } else if (Array.isArray(v)) {
      if (seg === 'length') {
        v = v.length;
      } else if (/^(0|[1-9][0-9]*)$/.test(seg)) {
        const idx = Number(seg);
        if (idx >= v.length) return undefined;
        v = v[idx];
      } else {
        return undefined;
      }
    } else {
      return undefined;
    }
  }
  return v;
}

/**
 * Tokenize a template into nodes. A {% %} tag alone on its line (only
 * spaces/tabs beside it) takes the whole line with it, its '\n' included.
 * Returns { ok, nodes } or { ok:false, reason }.
 */
function tokenizeTpl(name, src) {
  const root = [];
  const stack = [{ kind: 'root', list: root }];
  const cur = () => stack[stack.length - 1].list;
  let i = 0;
  const n = src.length;
  const pushText = (t) => {
    if (t !== '') cur().push({ kind: 'text', text: t });
  };
  while (i < n) {
    const open2 = src.indexOf('{{', i);
    const open3 = src.indexOf('{{{', i);
    const openTag = src.indexOf('{%', i);
    let next = -1;
    let kind = null;
    // The earliest opener wins; '{{{' beats '{{' at the same spot.
    for (const [pos, k] of [[open3, 'raw'], [open2, 'var'], [openTag, 'tag']]) {
      if (pos !== -1 && (next === -1 || pos < next)) {
        next = pos;
        kind = k;
      }
    }
    if (next === -1) {
      pushText(src.slice(i));
      break;
    }
    if (kind === 'tag') {
      // Standalone line? Only spaces/tabs between the line start and the tag,
      // and only spaces/tabs + '\n' after its close.
      let ls = src.lastIndexOf('\n', next - 1);
      const lineStart = ls === -1 ? 0 : ls + 1;
      const before = src.slice(lineStart, next);
      const close = src.indexOf('%}', next + 2);
      if (close === -1) return { ok: false, reason: tplErr(name, 'a {% is not closed') };
      const after = src.slice(close + 2);
      const afterNl = after.indexOf('\n');
      const afterSeg = afterNl === -1 ? after : after.slice(0, afterNl);
      if (/^[ \t]*$/.test(before) && /^[ \t]*$/.test(afterSeg)) {
        // The tag takes its whole line, '\n' included.
        pushText(src.slice(i, lineStart));
        const inner = src.slice(next + 2, close).trim();
        const r = handleTag(name, inner, stack);
        if (!r.ok) return r;
        i = close + 2 + (afterNl === -1 ? after.length : afterNl + 1);
        continue;
      }
      pushText(src.slice(i, next));
      const inner = src.slice(next + 2, close).trim();
      const r = handleTag(name, inner, stack);
      if (!r.ok) return r;
      i = close + 2;
      continue;
    }
    pushText(src.slice(i, next));
    if (kind === 'raw') {
      const close = src.indexOf('}}}', next + 3);
      if (close === -1) return { ok: false, reason: tplErr(name, 'a {{{ is not closed') };
      const path = parsePath(src.slice(next + 3, close).trim());
      if (!path) return { ok: false, reason: tplErr(name, `a path is not one: ${JSON.stringify(src.slice(next + 3, close))}`) };
      cur().push({ kind: 'raw', path });
      i = close + 3;
    } else {
      const close = src.indexOf('}}', next + 2);
      if (close === -1) return { ok: false, reason: tplErr(name, 'a {{ is not closed') };
      const path = parsePath(src.slice(next + 2, close).trim());
      if (!path) return { ok: false, reason: tplErr(name, `a path is not one: ${JSON.stringify(src.slice(next + 2, close))}`) };
      cur().push({ kind: 'var', path });
      i = close + 2;
    }
  }
  if (stack.length > 1) {
    const top = stack[stack.length - 1];
    return { ok: false, reason: tplErr(name, top.kind === 'for' ? 'a for is not closed' : 'an if is not closed') };
  }
  return { ok: true, nodes: root };
}

function handleTag(name, inner, stack) {
  const cur = () => stack[stack.length - 1].list;
  const top = () => stack[stack.length - 1];
  if (inner.startsWith('for ')) {
    const m = /^for ([A-Za-z_][A-Za-z0-9_]*) in (.+)$/.exec(inner);
    if (!m) return { ok: false, reason: tplErr(name, `a for is not one: ${JSON.stringify(inner)}`) };
    const path = parsePath(m[2].trim());
    if (!path) return { ok: false, reason: tplErr(name, `a path is not one: ${JSON.stringify(m[2])}`) };
    const node = { kind: 'for', varname: m[1], path, body: [] };
    cur().push(node);
    stack.push({ kind: 'for', list: node.body, node });
    return { ok: true };
  }
  if (inner.startsWith('if ')) {
    const rest = inner.slice(3).trim();
    let not = false;
    let ptext = rest;
    if (rest.startsWith('not ')) {
      not = true;
      ptext = rest.slice(4).trim();
    }
    const path = parsePath(ptext);
    if (!path) return { ok: false, reason: tplErr(name, `a path is not one: ${JSON.stringify(ptext)}`) };
    const node = { kind: 'if', path, not, then: [], els: null };
    cur().push(node);
    stack.push({ kind: 'if', list: node.then, node });
    return { ok: true };
  }
  if (inner === 'else') {
    const t = top();
    if (!t || t.kind !== 'if' || t.node.els !== null) {
      return { ok: false, reason: tplErr(name, 'an else outside an if') };
    }
    t.node.els = [];
    t.list = t.node.els;
    return { ok: true };
  }
  if (inner === 'endfor' || inner === 'endif') {
    const want = inner === 'endfor' ? 'for' : 'if';
    const t = top();
    if (!t || t.kind !== want) {
      return { ok: false, reason: tplErr(name, inner === 'endfor' ? 'an endfor with no open for' : 'an endif with no open if') };
    }
    stack.pop();
    return { ok: true };
  }
  const inc = /^include "([^"]*)"$/.exec(inner);
  if (inc) {
    cur().push({ kind: 'include', name: inc[1] });
    return { ok: true };
  }
  return { ok: false, reason: tplErr(name, `{% ${inner} %} is not a tag`) };
}

function renderNodes(nodes, scope, tpls, out, depth) {
  for (const n of nodes) {
    if (n.kind === 'text') {
      out.push(n.text);
    } else if (n.kind === 'var') {
      const v = lookup(scope, n.path);
      if (v === undefined || v === null || (typeof v === 'object' && !Array.isArray(v))) continue;
      if (typeof v === 'string') out.push(esc(v));
      else out.push(textOf(v));
    } else if (n.kind === 'raw') {
      out.push(textOf(lookup(scope, n.path)));
    } else if (n.kind === 'if') {
      const tv = truthy(lookup(scope, n.path));
      const branch = tv !== n.not ? n.then : n.els;
      if (branch) renderNodes(branch, scope, tpls, out, depth);
    } else if (n.kind === 'for') {
      const xs = lookup(scope, n.path);
      if (!Array.isArray(xs)) continue;
      for (let k = 0; k < xs.length; k++) {
        const loop = { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length };
        const frame = {};
        frame[n.varname] = xs[k];
        frame.loop = loop;
        renderNodes(n.body, [...scope, frame], tpls, out, depth);
      }
    } else if (n.kind === 'include') {
      if (depth >= INCLUDE_DEPTH) continue;
      const t = tpls.get(n.name);
      if (t && t.nodes) renderNodes(t.nodes, scope, tpls, out, depth + 1);
    }
  }
}


// ------------------------------------------------------------------- build ---

function normalizeCrlf(s) {
  return s.replace(/\r\n?/g, '\n');
}

function dirOf(path) {
  const i = path.lastIndexOf('/');
  return i === -1 ? '' : path.slice(0, i);
}

function isScheme(target) {
  return /^[A-Za-z][A-Za-z0-9+.-]*:/.test(target);
}

/** Resolve a relative link target from a source directory. Returns the
 *  resolved path (never above root), or null when the target is empty. */
function resolvePath(dir, target) {
  const parts = dir === '' ? [] : dir.split('/');
  const segs = target.split('/');
  for (const seg of segs) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length > 0) parts.pop();
      continue;
    }
    parts.push(seg);
  }
  return parts.join('/');
}

function cardOf(src) {
  const w = wordsOf(src.body);
  let rm = Math.ceil(w / 200);
  if (rm < 1) rm = 1;
  return {
    title: src.fm.title,
    url: src.url,
    date: src.fm.has_date ? src.fm.date : '',
    date_long: src.fm.has_date ? longDate(src.fm.date) : '',
    summary: src.fm.has_summary ? src.fm.summary : '',
    excerpt: src.excerpt,
    reading_minutes: rm,
    tags: src.tagrefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
  };
}

function pageOf(src) {
  const card = cardOf(src);
  const newer = src.newer
    ? { title: src.newer.fm.title, url: src.newer.url, date: src.newer.fm.has_date ? src.newer.fm.date : '', date_long: src.newer.fm.has_date ? longDate(src.newer.fm.date) : '' }
    : null;
  const older = src.older
    ? { title: src.older.fm.title, url: src.older.url, date: src.older.fm.has_date ? src.older.fm.date : '', date_long: src.older.fm.has_date ? longDate(src.older.fm.date) : '' }
    : null;
  return {
    ...card,
    slug: src.slug,
    content: src.html,
    toc: src.toc.map((t) => ({ level: t.level, id: t.id, html: t.html })),
    newer,
    older,
    is_post: src.post,
  };
}

function indexUrl(base, k) {
  if (k === 1) return base + '/';
  return base + '/page/' + k + '/';
}

/** The reference build for one case's input. Returns the output tree. */
export function run(input) {
  const site = input.site;
  const base = site.base_url.replace(/\/+$/, '');
  const perPage = site.per_page;
  const recentN = site.recent;

  // Templates, sorted by name; a failed one is reported.
  const tplNames = Object.keys(input.templates).sort(byCodeUnits);
  const tpls = new Map();
  const report = [];
  for (const name of tplNames) {
    const r = tokenizeTpl(name, input.templates[name]);
    if (r.ok) tpls.set(name, { nodes: r.nodes });
    else {
      tpls.set(name, { nodes: null });
      report.push(r.reason);
    }
  }
  const tplOk = (name) => {
    const t = tpls.get(name);
    return t !== undefined && t.nodes !== null;
  };

  // 1. readSources: sort files by path; .md = source, else static.
  const files = [...input.files].sort((a, b) => byCodeUnits(a.path, b.path));
  const srcs = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) {
      srcs.push({ path: f.path, text: normalizeCrlf(f.content), kept: true });
    } else {
      statics.push(f);
    }
  }

  // 2. frontMatter: refusals in order.
  for (const s of srcs) {
    const r = parseFm(s.text);
    if (!r.ok) {
      s.kept = false;
      report.push(`refused ${s.path}: ${r.reason}`);
      continue;
    }
    s.fm = r.fm;
    s.body = r.body;
    s.post = s.path.startsWith('posts/');
    let own = null;
    if (s.path.startsWith('tags/')) own = 'tags';
    else if (s.path.startsWith('page/')) own = 'page';
    if (own !== null) {
      s.kept = false;
      report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !s.fm.has_date) {
      s.kept = false;
      report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.fm.has_layout ? s.fm.layout : s.post ? 'post' : 'page';
    s.layout = layout;
    if (!tplOk(layout)) {
      s.kept = false;
      report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    if (s.fm.has_draft && s.fm.draft) {
      s.kept = false;
      report.push(`draft ${s.path}`);
      continue;
    }
  }
  const kept = srcs.filter((s) => s.kept);

  // 3. permalinks: slug unique per directory, in source order.
  const takenByDir = new Map();
  const take = (dir, slug) => {
    let set = takenByDir.get(dir);
    if (!set) {
      set = new Set();
      takenByDir.set(dir, set);
    }
    let cand = slug;
    let n = 2;
    while (set.has(cand)) cand = `${slug}-${n++}`;
    set.add(cand);
    return cand;
  };
  for (const s of kept) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    const raw = s.fm.has_slug ? s.fm.slug : slugify(s.fm.title);
    const slug = take(dir, raw);
    s.slug = slug;
    s.outdir = dir === '' ? slug : dir + '/' + slug;
    s.url = base + '/' + s.outdir + '/';
  }

  // 4. markdown: render + link resolution. Broken links are reported once.
  const bySource = new Map(kept.map((s) => [s.path, s]));
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  const out = {};
  for (const s of kept) {
    const dir = dirOf(s.path);
    const link = (target) => {
      if (isScheme(target) || target.startsWith('/') || target.startsWith('#')) return target;
      let hash = target.indexOf('#');
      const rel = hash === -1 ? target : target.slice(0, hash);
      const frag = hash === -1 ? '' : target.slice(hash);
      if (rel !== '') {
        const path = resolvePath(dir, rel);
        const hit = bySource.get(path);
        if (hit) return hit.url + frag;
        if (staticPaths.has(path)) return base + '/' + path + frag;
      }
      broken.add(`broken-link ${s.path} -> ${target}`);
      return target;
    };
    const slugSet = new Set();
    const r = renderMd(s.body, { link }, slugSet);
    s.html = r.html;
    s.toc = r.toc;
    s.excerpt = r.excerpt;
  }

  // 5. collections: posts newest date first, ties by slug.
  const posts = kept.filter((s) => s.post);
  posts.sort((a, b) => {
    if (a.fm.date !== b.fm.date) return a.fm.date < b.fm.date ? 1 : -1;
    return byCodeUnits(a.slug, b.slug);
  });
  for (let i = 0; i < posts.length; i++) {
    posts[i].newer = i > 0 ? posts[i - 1] : null;
    posts[i].older = i + 1 < posts.length ? posts[i + 1] : null;
  }
  for (const s of kept) {
    if (!s.post) {
      s.newer = null;
      s.older = null;
    }
  }

  // 6. tags: per post's tags, each slug once, first spelling kept.
  const tagBySlug = new Map();
  for (const p of posts) {
    const seenSlug = new Set();
    for (const t of p.fm.tags) {
      const slug = slugify(t);
      if (seenSlug.has(slug)) continue;
      seenSlug.add(slug);
      if (!tagBySlug.has(slug)) tagBySlug.set(slug, { name: t, slug, url: base + '/tags/' + slug + '/', posts: [] });
      tagBySlug.get(slug).posts.push(p);
    }
  }
  const tags = [...tagBySlug.values()].sort((a, b) => {
    if (a.posts.length !== b.posts.length) return b.posts.length - a.posts.length;
    return byCodeUnits(a.slug, b.slug);
  });
  // A page's tags link to the post-tag of the same slug, or url "".
  for (const s of kept) {
    s.tagrefs = [];
    if (s.fm.has_tags) {
      const seen = new Set();
      for (const t of s.fm.tags) {
        const slug = slugify(t);
        if (seen.has(slug)) continue;
        seen.add(slug);
        const tag = tagBySlug.get(slug);
        s.tagrefs.push(tag ? { name: tag.name, slug: tag.slug, url: tag.url } : { name: t, slug, url: '' });
      }
    }
    // The card's tags use the first spelling the post itself carries.
    s.cardTags = s.tagrefs;
  }

  // 7. layouts.
  const siteVal = { title: site.title, base_url: base };
  const tagsVal = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recentVal = posts.slice(0, recentN).map((p) => cardOf(p));
  const renderTpl = (name, extra) => {
    const t = tpls.get(name);
    if (!t || !t.nodes) return null;
    const scope = [{ site: siteVal, tags: tagsVal, recent: recentVal, ...extra }];
    const parts = [];
    renderNodes(t.nodes, scope, tpls, parts, 0);
    return parts.join('');
  };
  for (const s of kept) {
    const html = renderTpl(s.layout, { page: pageOf(s) });
    if (html !== null) out[s.outdir + '/index.html'] = html;
  }
  // Index pages.
  const pages = Math.max(1, Math.ceil(posts.length / perPage));
  if (tplOk('index')) {
    for (let k = 1; k <= pages; k++) {
      const from = (k - 1) * perPage;
      const slice = posts.slice(from, from + perPage);
      const pager = {
        number: k,
        total: pages,
        url: indexUrl(base, k),
        newer_url: k === 1 ? '' : indexUrl(base, k - 1),
        older_url: k === pages ? '' : indexUrl(base, k + 1),
      };
      const html = renderTpl('index', { posts: slice.map((p) => cardOf(p)), pager });
      if (html !== null) out[(k === 1 ? 'index.html' : `page/${k}/index.html`)] = html;
    }
  }
  // Tag pages.
  if (tplOk('tag')) {
    for (const t of tags) {
      const html = renderTpl('tag', { tag: { name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cardOf(p)) } });
      if (html !== null) out[`tags/${t.slug}/index.html`] = html;
    }
  }
  // Tags index.
  if (tplOk('tags')) {
    const html = renderTpl('tags', {});
    if (html !== null) out['tags/index.html'] = html;
  }
  // Statics, unless a built page has that path.
  let nstatic = 0;
  for (const f of statics) {
    if (Object.prototype.hasOwnProperty.call(out, f.path)) {
      report.push(`shadowed ${f.path}: a built page has its path`);
      continue;
    }
    out[f.path] = f.content;
    nstatic++;
  }

  // 8. report.
  const lines = [...report, ...broken].sort(byCodeUnits);
  lines.push(`built ${kept.length - posts.length} pages, ${posts.length} posts, ${tags.length} tags and ${nstatic} static files`);
  out['build-report.txt'] = lines.map((l) => l + '\n').join('');
  return out;
}

