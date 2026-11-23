// solve.js — a fast drop-in for makefun-site 1.0.0.
//
// Written fresh for site-generator-speed-01-run-20261122. run(input) returns,
// for one case's { site, templates, files }, the same output tree makefun-site
// 1.0.0's build returns: every path it writes and that file's text, byte for
// byte. No dependencies, no I/O, no clock, no randomness: the answer is a
// function of the input alone.
//
// Speed comes from: a template compiler (each template becomes straight-line
// code over one shared scope object, includes as calls), bulk HTML escaping
// instead of per-character escaping, indexOf-driven inline Markdown scanning,
// an ASCII fast path for slugs, and per-source cards computed once.

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const RESERVED = ['posts', 'tags', 'page'];
const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

// ---------------------------------------------------------------- text

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const HAS_SPECIAL = /[&<>"']/;

/** `text` with & < > " and ' escaped. Fast path when nothing needs escaping. */
function esc(text) {
  if (!HAS_SPECIAL.test(text)) return text;
  let out = '';
  let last = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    let e;
    if (c === 38) e = '&amp;';
    else if (c === 60) e = '&lt;';
    else if (c === 62) e = '&gt;';
    else if (c === 34) e = '&quot;';
    else if (c === 39) e = '&#39;';
    else continue;
    out += text.slice(last, i) + e;
    last = i + 1;
  }
  return out + text.slice(last);
}

const slugCache = new Map();
const COMBINING = /\p{M}/gu;

/** The slug of `text`: NFKD, combining marks dropped, A-Z lowered, every run
 * of characters other than a-z and 0-9 one "-", none at either end, at most 64
 * characters (a "-" left at the cut dropped), "untitled" when nothing is left. */
function slugify(text) {
  const hit = slugCache.get(text);
  if (hit !== undefined) return hit;
  const out = slugifyUncached(text);
  slugCache.set(text, out);
  return out;
}

function slugifyUncached(text) {
  // NFKD is the identity on pure ASCII, so skip the normalisation there.
  let ascii = true;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) { ascii = false; break; }
  }
  const plain = ascii ? text : text.normalize('NFKD').replace(COMBINING, '');
  let out = '';
  let gap = false;
  for (let i = 0; i < plain.length; i++) {
    const code = plain.charCodeAt(i);
    let keep = 0;
    if (code >= 65 && code <= 90) keep = code + 32;
    else if ((code >= 97 && code <= 122) || (code >= 48 && code <= 57)) keep = code;
    if (keep === 0) {
      if (out.length > 0) gap = true;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += String.fromCharCode(keep);
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

/** `slug` made unique against `taken` (and added to it). */
function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** A YYYY-MM-DD date in words, from the string alone. */
function longDate(date) {
  return `${Number(date.slice(8, 10))} ${MONTHS[Number(date.slice(5, 7)) - 1]} ${date.slice(0, 4)}`;
}

/** The words of a body: its runs of characters other than whitespace. */
function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    const ws = c === 32 || c === 9 || c === 10 || c === 11 || c === 12 || c === 13 || c === 160 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
    if (ws) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

// ---------------------------------------------------------------- front matter

const FM_KEYS = new Set(['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary']);
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const FM_NAME = /^[a-z0-9-]+$/;
const FM_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FM_TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

/** A string value: "..." with \" and \\ its only escapes, or bare. */
function stringOf(raw) {
  if (raw.charCodeAt(0) === 34) {
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const nx = raw[i + 1];
        if ((nx !== '"' && nx !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += nx;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

/** One key's value, or why it is refused. */
function valueOf(key, raw) {
  if (key === 'title' || key === 'summary') {
    const s = stringOf(raw);
    if ('error' in s) return { error: `key "${key}": ${s.error}` };
    return s;
  }
  if (key === 'date') {
    const m = FM_DATE.exec(raw);
    const y = m ? Number(m[1]) : 0;
    const mo = m ? Number(m[2]) : 0;
    const d = m ? Number(m[3]) : 0;
    if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
    return { value: raw };
  }
  if (key === 'tags') {
    if (raw.charCodeAt(0) !== 91 || raw.charCodeAt(raw.length - 1) !== 93) return { error: 'key "tags": not a list [a, b]' };
    const inner = raw.slice(1, -1);
    if (inner.trim() === '') return { value: [] };
    const tags = inner.split(',').map((t) => t.trim());
    for (const t of tags) {
      if (t === '' || !FM_TAG.test(t)) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
    }
    return { value: tags };
  }
  if (key === 'draft') {
    if (raw === 'true') return { value: true };
    if (raw === 'false') return { value: false };
    return { error: 'key "draft": true or false' };
  }
  if (key === 'layout') {
    return FM_NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
  }
  if (key === 'slug') {
    return FM_SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
  }
  return { error: `unknown key "${key}"` };
}

function isBlankLine(line) {
  if (line.length === 0) return true;
  const c0 = line.charCodeAt(0);
  if (c0 !== 32 && c0 !== 9) return false;
  for (let k = 1; k < line.length; k++) {
    const c = line.charCodeAt(k);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/**
 * A source's front matter and body: { data, body }, or { error } with the
 * first reason it is refused. `text` has "\n" line ends.
 */
function parseFrontMatter(text) {
  if (text !== '---' && !text.startsWith('---\n')) return { error: 'no front matter' };
  // Scan the front-matter lines without splitting the whole body.
  const fmLines = [];
  let pos = 4;
  let bodyStart = -1;
  let lineNo = 2;
  while (pos < text.length) {
    const nl = text.indexOf('\n', pos);
    const line = nl === -1 ? text.slice(pos) : text.slice(pos, nl);
    if (line === '---') {
      bodyStart = nl === -1 ? text.length : nl + 1;
      break;
    }
    fmLines.push(line);
    lineNo++;
    if (nl === -1) break;
    pos = nl + 1;
  }
  if (bodyStart === -1) return { error: 'the front matter is not closed' };
  const data = {};
  for (let k = 0; k < fmLines.length; k++) {
    const line = fmLines[k];
    if (isBlankLine(line) || line.charCodeAt(0) === 35) continue;
    const m = FM_LINE.exec(line);
    if (!m) return { error: `line ${k + 2} is not "key: value"` };
    const key = m[1];
    const raw = (m[2] === undefined ? '' : m[2]).replace(/[ \t]+$/, '');
    if (!FM_KEYS.has(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  return { data, body: text.slice(bodyStart) };
}

// ---------------------------------------------------------------- markdown

function trimLine(line) {
  let a = 0;
  let b = line.length;
  while (a < b && (line.charCodeAt(a) === 32 || line.charCodeAt(a) === 9)) a++;
  while (b > a && (line.charCodeAt(b - 1) === 32 || line.charCodeAt(b - 1) === 9)) b--;
  return line.slice(a, b);
}

/** A fence-open line's language, or null when the line is not a fence open. */
function fenceOpen(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let j = 3;
  while (j < line.length) {
    const c = line.charCodeAt(j);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) j++;
    else break;
  }
  for (let k = j; k < line.length; k++) {
    const c = line.charCodeAt(k);
    if (c !== 32 && c !== 9) return null;
  }
  return line.slice(3, j);
}

function isFenceClose(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let k = 3; k < line.length; k++) {
    const c = line.charCodeAt(k);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** A heading line's level and text, or null (a heading's text is never empty). */
function headingOf(line) {
  let d = 0;
  while (d < line.length && line.charCodeAt(d) === 35) d++;
  if (d === 0 || d > 6 || d >= line.length) return null;
  const c = line.charCodeAt(d);
  if (c !== 32 && c !== 9) return null;
  let a = d;
  while (a < line.length && (line.charCodeAt(a) === 32 || line.charCodeAt(a) === 9)) a++;
  let b = line.length;
  while (b > a && (line.charCodeAt(b - 1) === 32 || line.charCodeAt(b - 1) === 9)) b--;
  if (b <= a) return null;
  return { depth: d, text: line.slice(a, b) };
}

function isHr(line) {
  if (line.length < 3) return false;
  const a = line.charCodeAt(0);
  const b = line.charCodeAt(1);
  const c = line.charCodeAt(2);
  if (!((a === 45 && b === 45 && c === 45) || (a === 42 && b === 42 && c === 42))) return false;
  for (let k = 3; k < line.length; k++) {
    const ch = line.charCodeAt(k);
    if (ch !== 32 && ch !== 9) return false;
  }
  return true;
}

const CONTINUED = /^ {2,}\S/;
function isContinued(line) {
  return line.charCodeAt(0) === 32 && line.charCodeAt(1) === 32 && CONTINUED.test(line);
}

/** An ordered-list marker line: { num, end } (end = index past ". "), or null. */
function orderedStart(line) {
  const c0 = line.charCodeAt(0);
  if (c0 < 48 || c0 > 57) return null;
  let j = 0;
  while (j < line.length && line.charCodeAt(j) >= 48 && line.charCodeAt(j) <= 57) j++;
  if (j === 0 || j > 9) return null;
  if (line.charCodeAt(j) !== 46 || line.charCodeAt(j + 1) !== 32) return null;
  return { num: Number(line.slice(0, j)), end: j + 2 };
}

/** Whether `line` ends a paragraph: a blank line, a fence, a heading or a break. */
function endsParagraph(line) {
  if (isBlankLine(line)) return true;
  if (line.charCodeAt(0) === 96 && fenceOpen(line) !== null) return true;
  if (line.charCodeAt(0) === 35 && headingOf(line) !== null) return true;
  if (isHr(line)) return true;
  return false;
}

/** `src`'s block tokens, in order. */
function lex(src) {
  const lines = src.split('\n');
  const tokens = [];
  const n = lines.length;
  let i = 0;
  while (i < n) {
    const line = lines[i];
    if (isBlankLine(line)) { i++; continue; }
    const lang = fenceOpen(line);
    if (lang !== null) {
      const body = [];
      i++;
      while (i < n && !isFenceClose(lines[i])) body.push(lines[i++]);
      i++;
      tokens.push({ type: 'code', lang, lines: body });
      continue;
    }
    if (line.charCodeAt(0) === 35) {
      const heading = headingOf(line);
      if (heading !== null) {
        tokens.push({ type: 'heading', depth: heading.depth, text: heading.text });
        i++;
        continue;
      }
    }
    if (isHr(line)) { tokens.push({ type: 'hr' }); i++; continue; }
    if (line.charCodeAt(0) === 62) {
      const body = [];
      while (i < n && lines[i].charCodeAt(0) === 62) {
        const l = lines[i++];
        body.push(l.charCodeAt(1) === 32 ? l.slice(2) : l.slice(1));
      }
      const ls = [];
      for (const l of body) {
        const t = trimLine(l);
        if (t !== '') ls.push(t);
      }
      tokens.push({ type: 'blockquote', lines: ls });
      continue;
    }
    const bullet = (line.charCodeAt(0) === 45 || line.charCodeAt(0) === 42) && line.charCodeAt(1) === 32;
    const ordered = bullet ? null : orderedStart(line);
    if (bullet || ordered !== null) {
      const isOrdered = ordered !== null;
      const items = [[trimLine(isOrdered ? line.slice(ordered.end) : line.slice(2))]];
      i++;
      while (i < n) {
        const l2 = lines[i];
        if (isOrdered) {
          const o2 = orderedStart(l2);
          if (o2 !== null) { items.push([trimLine(l2.slice(o2.end))]); i++; continue; }
        } else if ((l2.charCodeAt(0) === 45 || l2.charCodeAt(0) === 42) && l2.charCodeAt(1) === 32) {
          items.push([trimLine(l2.slice(2))]);
          i++;
          continue;
        }
        if (isContinued(l2)) { items[items.length - 1].push(trimLine(l2)); i++; continue; }
        break;
      }
      tokens.push({ type: 'list', ordered: isOrdered, start: isOrdered ? ordered.num : 1, items: items.map((parts) => parts.join('\n')) });
      continue;
    }
    const body = [trimLine(line)];
    i++;
    while (i < n && !endsParagraph(lines[i])) body.push(trimLine(lines[i++]));
    tokens.push({ type: 'paragraph', text: body.join('\n') });
  }
  return tokens;
}

// --- inline

const ESCAPABLE = new Uint8Array(128);
for (const ch of ['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']) ESCAPABLE[ch.charCodeAt(0)] = 1;
const isEscCode = (c) => c < 128 && ESCAPABLE[c] === 1;

/** Whether the character at `j` is escaped: an odd run of backslashes before it. */
function isEscaped(s, j) {
  let k = j - 1;
  let n = 0;
  while (k >= 0 && s.charCodeAt(k) === 92) { n++; k--; }
  return (n & 1) === 1;
}

/** The first index from `from` where `s` holds `what` outside a backslash escape, or -1. */
function findUnescaped(s, what, from) {
  let j = s.indexOf(what, from);
  while (j !== -1 && isEscaped(s, j)) j = s.indexOf(what, j + 1);
  return j;
}

/** The first single * from `from` (one whose run is not **), outside an escape, or -1. */
function findSingleStar(s, from) {
  let j = s.indexOf('*', from);
  while (j !== -1) {
    if (isEscaped(s, j)) { j = s.indexOf('*', j + 1); continue; }
    let start = j;
    while (start > from && s.charCodeAt(start - 1) === 42 && !isEscaped(s, start - 1)) start--;
    let end = j;
    while (s.charCodeAt(end + 1) === 42) end++;
    if (((end - start + 1) & 1) === 1) return end;
    j = s.indexOf('*', end + 1);
  }
  return -1;
}

const WS_TEST = /\s/;
const HAS_MARK = /[\\`*\[]/;
const MARK_CHARS = ['\\', '`', '*', '['];

/** `s` rendered inline; `ctx` resolves a link target to its href. */
function inline(s, ctx) {
  // Fast path: no backslash, backtick, star or bracket can open a construct,
  // so the string is itself, escaped.
  if (!HAS_MARK.test(s)) return esc(s);
  // Jump from one construct to the next with native indexOf scans; the plain
  // span between them is escaped in bulk.
  let out = '';
  let run = 0;
  let i = 0;
  const n = s.length;
  while (i < n) {
    let j = -1;
    for (const ch of MARK_CHARS) {
      const k = s.indexOf(ch, i);
      if (k !== -1 && (j === -1 || k < j)) j = k;
    }
    if (j === -1) break;
    const c = s.charCodeAt(j);
    if (c === 92) {
      if (j + 1 < n && isEscCode(s.charCodeAt(j + 1))) {
        if (j > run) out += esc(s.slice(run, j));
        out += s[j + 1] === '>' ? '&gt;' : s[j + 1];
        i = j + 2;
        run = i;
        continue;
      }
      // A backslash before anything else is itself: it stays in the plain run.
      i = j + 1;
      continue;
    }
    if (c === 96) {
      const k = s.indexOf('`', j + 1);
      if (k > j + 1) {
        if (j > run) out += esc(s.slice(run, j));
        out += '<code>' + esc(s.slice(j + 1, k)) + '</code>';
        i = k + 1;
        run = i;
      } else i = j + 1;
      continue;
    }
    if (c === 42) {
      if (s.charCodeAt(j + 1) === 42) {
        const k = findUnescaped(s, '**', j + 2);
        if (k > j + 2) {
          if (j > run) out += esc(s.slice(run, j));
          out += '<strong>' + inline(s.slice(j + 2, k), ctx) + '</strong>';
          i = k + 2;
          run = i;
        } else i = j + 2;
        continue;
      }
      const k = findSingleStar(s, j + 1);
      if (k > j + 1) {
        if (j > run) out += esc(s.slice(run, j));
        out += '<em>' + inline(s.slice(j + 1, k), ctx) + '</em>';
        i = k + 1;
        run = i;
      } else i = j + 1;
      continue;
    }
    // '['
    const k = findUnescaped(s, ']', j + 1);
    const m = k !== -1 && s.charCodeAt(k + 1) === 40 ? s.indexOf(')', k + 2) : -1;
    const target = m === -1 ? '' : s.slice(k + 2, m);
    if (target !== '' && !WS_TEST.test(target)) {
      if (j > run) out += esc(s.slice(run, j));
      out += '<a href="' + esc(resolveLink(ctx, target)) + '">' + inline(s.slice(j + 1, k), ctx) + '</a>';
      i = m + 1;
      run = i;
      continue;
    }
    i = j + 1;
  }
  if (n > run) out += esc(s.slice(run, n));
  return out;
}

/** `rel` from the directory parts `dirParts`, or null when it climbs out of the root. */
function resolvePath(dirParts, rel) {
  const parts = dirParts.slice();
  const segs = rel.split('/');
  for (const seg of segs) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

function isScheme(t) {
  const c0 = t.charCodeAt(0);
  if (!((c0 >= 65 && c0 <= 90) || (c0 >= 97 && c0 <= 122))) return false;
  for (let i = 1; i < t.length; i++) {
    const c = t.charCodeAt(i);
    if (c === 58) return true;
    if (!((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 43 || c === 45 || c === 46)) return false;
  }
  return false;
}

/** A link target's href, from the source `ctx` describes; broken links are reported. */
function resolveLink(ctx, target) {
  if (isScheme(target) || target.charCodeAt(0) === 47 || target.charCodeAt(0) === 35) return target;
  const hash = target.indexOf('#');
  const rel = hash === -1 ? target : target.slice(0, hash);
  const frag = hash === -1 ? '' : target.slice(hash);
  const path = rel === '' ? null : resolvePath(ctx.dirParts, rel);
  if (path !== null) {
    const pg = ctx.bySource.get(path);
    if (pg !== undefined) return pg.url + frag;
    if (ctx.staticPaths.has(path)) return ctx.base + '/' + path + frag;
  }
  ctx.broken.add(`broken-link ${ctx.srcPath} -> ${target}`);
  return target;
}

/**
 * `src` rendered: its HTML (each block on its own lines, a "\n" after the
 * last), its table of contents and its excerpt (the first paragraph's HTML).
 */
function renderMarkdown(src, ctx) {
  const ids = new Set();
  const blocks = [];
  const toc = [];
  let excerpt = null;
  for (const t of lex(src)) {
    if (t.type === 'heading') {
      const id = uniqueSlug(slugify(t.text), ids);
      const html = inline(t.text, ctx);
      blocks.push(`<h${t.depth} id="${id}">${html}</h${t.depth}>`);
      if (t.depth === 2 || t.depth === 3) toc.push({ level: t.depth, id, html });
    } else if (t.type === 'code') {
      let inner = '';
      for (const l of t.lines) inner += esc(l) + '\n';
      blocks.push(`<pre><code${t.lang === '' ? '' : ` class="language-${esc(t.lang)}"`}>${inner}</code></pre>`);
    } else if (t.type === 'hr') {
      blocks.push('<hr>');
    } else if (t.type === 'blockquote') {
      blocks.push(t.lines.length === 0 ? '<blockquote>\n</blockquote>' : `<blockquote>\n<p>${inline(t.lines.join('\n'), ctx)}</p>\n</blockquote>`);
    } else if (t.type === 'list') {
      const tag = t.ordered ? 'ol' : 'ul';
      const open = t.ordered && t.start !== 1 ? `<ol start="${t.start}">` : `<${tag}>`;
      let items = '';
      for (const item of t.items) items += `<li>${inline(item, ctx)}</li>\n`;
      blocks.push(`${open}\n${items}</${tag}>`);
    } else {
      const html = inline(t.text, ctx);
      if (excerpt === null) excerpt = html;
      blocks.push(`<p>${html}</p>`);
    }
  }
  let html = '';
  for (const b of blocks) html += b + '\n';
  return { html, toc, excerpt: excerpt === null ? '' : excerpt };
}

// ---------------------------------------------------------------- templates

/** A template that does not parse: its name and why. */
class TemplateError extends Error {}

const T_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const T_FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const T_IF = /^if (not )?(\S+)$/;
const T_INCLUDE = /^include "([a-z0-9-]+)"$/;

/** The path `text` names, split at its dots, or a TemplateError. */
function pathOf(name, text) {
  if (!T_PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function onlySpacesTabs(src, a, b) {
  for (let k = a; k < b; k++) {
    const c = src.charCodeAt(k);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** `src`'s tokens: text, {{ }}, {{{ }}} and {% %}, a standalone tag's line taken with it. */
function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = onlySpacesTabs(src, lineStart, open) && onlySpacesTabs(src, end, lineEnd);
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

/** `src` parsed into its tree of nodes (a TemplateError when it does not parse). */
function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = T_FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = T_IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : (parent.else ?? parent.then);
      } else if ((m = T_INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

// --- template code generation
//
// Each template compiles to a function (out, S, T, depth): it appends to the
// shared string array `out`, reads values from the one flat scope object `S`
// (top-level values plus the loop bindings in force, saved and restored around
// each for), looks includes up in `T` (name -> compiled function), and stops
// including past depth 8. A path's first name is a property of S; each next
// name is an own property of the value so far.

/** A value as text: a string itself, a number in JavaScript's digits, true or false, an array's items joined by ", ", else "". */
function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

/** Whether a value counts as true in an if. */
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

/** An own property of `v`, or undefined. */
function getSeg(v, key) {
  return v !== null && typeof v === 'object' && Object.hasOwn(v, key) ? v[key] : undefined;
}

const HELPERS = { e: esc, t: textOf, y: truthy, g: getSeg, has: (o, k) => Object.hasOwn(o, k) };

/** Generate a template's code into `lines` (a function body appending to `out`). */
function genTemplateLines(ast, lines) {
  let uid = 0;
  const lookupExpr = (path) => {
    let e = `S.${path[0]}`;
    for (let k = 1; k < path.length; k++) e = `H.g(${e}, ${JSON.stringify(path[k])})`;
    return e;
  };
  const gen = (nodes) => {
    for (const n of nodes) {
      if (n.kind === 'text') lines.push(`out+=${JSON.stringify(n.text)};`);
      else if (n.kind === 'var') lines.push(`out+=H.e(H.t(${lookupExpr(n.path)}));`);
      else if (n.kind === 'raw') lines.push(`out+=H.t(${lookupExpr(n.path)});`);
      else if (n.kind === 'if') {
        lines.push(`if(H.y(${lookupExpr(n.path)})!==${n.not ? 'true' : 'false'}){`);
        gen(n.then);
        if (n.else !== null) {
          lines.push('}else{');
          gen(n.else);
        }
        lines.push('}');
      } else if (n.kind === 'for') {
        const u = uid++;
        lines.push(`{const __a${u}=${lookupExpr(n.path)};if(Array.isArray(__a${u})){const __n${u}=__a${u}.length;const __h${u}=H.has(S,${JSON.stringify(n.name)});const __o${u}=S.${n.name};const __hl${u}=H.has(S,"loop");const __ol${u}=S.loop;for(let __i${u}=0;__i${u}<__n${u};__i${u}++){S.${n.name}=__a${u}[__i${u}];S.loop={index:__i${u}+1,first:__i${u}===0,last:__i${u}===__n${u}-1,length:__n${u}};`);
        gen(n.body);
        lines.push(`}if(__h${u})S.${n.name}=__o${u};else delete S.${n.name};if(__hl${u})S.loop=__ol${u};else delete S.loop;}}`);
      } else if (n.kind === 'include') {
        const u = uid++;
        lines.push(`{const __f${u}=T[${JSON.stringify(n.name)}];if(__f${u}!==undefined&&depth<8)out+=__f${u}(S,T,depth+1);}`);
      }
    }
  };
  gen(ast);
}

/** The value at `path` in the flat scope `S` (loop bindings shadow top values). */
function lookup(S, path) {
  let v = S[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

/**
 * `nodes` rendered in the flat scope `S` (`asts` holds every template's
 * nodes, for includes). This is the fallback when code generation from
 * strings is unavailable: same semantics as the compiled templates, just
 * slower.
 */
function renderNodes(nodes, S, asts, depth) {
  let out = '';
  for (const n of nodes) {
    if (n.kind === 'text') out += n.text;
    else if (n.kind === 'var') out += esc(textOf(lookup(S, n.path)));
    else if (n.kind === 'raw') out += textOf(lookup(S, n.path));
    else if (n.kind === 'if') {
      const branch = truthy(lookup(S, n.path)) !== n.not ? n.then : n.else;
      if (branch !== null) out += renderNodes(branch, S, asts, depth);
    } else if (n.kind === 'for') {
      const xs = lookup(S, n.path);
      if (Array.isArray(xs)) {
        const had = Object.hasOwn(S, n.name);
        const old = S[n.name];
        const hadLoop = Object.hasOwn(S, 'loop');
        const oldLoop = S.loop;
        for (let k = 0; k < xs.length; k++) {
          S[n.name] = xs[k];
          S.loop = { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length };
          out += renderNodes(n.body, S, asts, depth);
        }
        if (had) S[n.name] = old;
        else delete S[n.name];
        if (hadLoop) S.loop = oldLoop;
        else delete S.loop;
      }
    } else if (n.kind === 'include') {
      const sub = asts[n.name];
      if (sub !== undefined && depth < 8) out += renderNodes(sub, S, asts, depth + 1);
    }
  }
  return out;
}

/**
 * All of a case's templates, compiled in one Function call into a
 * null-prototype object: name -> function(S, T, depth) returning the rendered
 * string. A template that does not parse is left out and its reason returned.
 * When code generation from strings is unavailable (or the generated code
 * fails to build), `funcs` is null and the caller renders the parsed trees
 * (`asts`) with renderNodes instead.
 */
function compileTemplates(templatesObj) {
  const errors = [];
  const asts = Object.create(null);
  for (const name of Object.keys(templatesObj).sort(byCodeUnits)) {
    try {
      asts[name] = parseTemplate(name, templatesObj[name]);
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      errors.push(err.message);
    }
  }
  let funcs = null;
  try {
    const chunks = ['const F=Object.create(null);'];
    for (const name of Object.keys(asts).sort(byCodeUnits)) {
      const lines = [];
      genTemplateLines(asts[name], lines);
      chunks.push(`F[${JSON.stringify(name)}]=function(S,T,depth){let out="";${lines.join('')}return out;};`);
    }
    chunks.push('return F;');
    funcs = new Function('H', chunks.join('\n'))(HELPERS);
  } catch {
    funcs = null;
  }
  return { funcs, asts, errors };
}

// ---------------------------------------------------------------- the build

/** A page's link record as templates read it. */
const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });

/** One site's output tree: path -> text. */
function build(input) {
  const report = [];
  const templates = compileTemplates(input.templates);
  for (const e of templates.errors) report.push(e);
  const base = input.site.base_url.replace(/\/+$/, '');

  // 1. Sources, in code-unit order of their paths; a non-.md file is static.
  const files = [...input.files].sort((a, b) => byCodeUnits(a.path, b.path));
  let sources = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) sources.push({ path: f.path, text: f.content.replace(/\r\n?/g, '\n') });
    else statics.push(f);
  }

  // 2. Front matter: a refused source is reported and dropped, and so is a draft.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    let own = null;
    if (s.path.startsWith('tags/')) own = 'tags';
    else if (s.path.startsWith('page/')) own = 'page';
    if (own !== null) {
      report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !Object.hasOwn(s.data, 'date')) {
      report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (templates.asts[layout] === undefined) {
      report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      report.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }
  sources = kept;

  // 3. Permalinks: each page's slug made unique in its directory, in source order.
  const taken = new Map();
  const bySource = new Map();
  for (const s of sources) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) {
      set = new Set(dir === '' ? RESERVED : []);
      taken.set(dir, set);
    }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }

  // 4. Markdown, each body's links resolved to the pages and static files they name.
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  const ctx = { base, bySource, staticPaths, broken, dirParts: [], srcPath: '' };
  for (const s of sources) {
    ctx.dirParts = dirOf(s.path) === '' ? [] : dirOf(s.path).split('/');
    ctx.srcPath = s.path;
    const md = renderMarkdown(s.body, ctx);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. Collections: the posts, newest first and then by slug; neighbours.
  const posts = sources.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  posts.forEach((p, k) => {
    p.newer = k === 0 ? null : posts[k - 1];
    p.older = k === posts.length - 1 ? null : posts[k + 1];
  });

  // 6. Tags, grouped by their slug, named as first met in the posts' order.
  const bySlug = new Map();
  for (const p of posts) {
    p.tagRefs = [];
    const seen = new Set();
    for (const name of p.data.tags ?? []) {
      const slug = slugify(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      let tag = bySlug.get(slug);
      if (tag === undefined) {
        tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] };
        bySlug.set(slug, tag);
      }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of sources) {
    if (s.post) continue;
    const seen = new Set();
    const refs = [];
    for (const name of s.data.tags ?? []) {
      const slug = slugify(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      refs.push(bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
    }
    s.tagRefs = refs;
  }
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // 7. Layouts: every page through its layout, the index and tag pages through theirs.
  const cardCache = new Map();
  const cardOf = (s) => {
    let c = cardCache.get(s);
    if (c !== undefined) return c;
    c = {
      title: s.data.title,
      url: s.url,
      date: s.data.date ?? '',
      date_long: s.data.date === undefined ? '' : longDate(s.data.date),
      summary: s.data.summary ?? '',
      excerpt: s.excerpt,
      reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
      tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
    };
    cardCache.set(s, c);
    return c;
  };
  const pageOf = (s) => ({
    ...cardOf(s),
    slug: s.slug,
    content: s.html,
    toc: s.toc.map((h) => ({ level: h.level, id: h.id, html: h.html })),
    newer: linkOf(s.newer ?? null),
    older: linkOf(s.older ?? null),
    is_post: s.post,
  });
  const tagOf = (t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cardOf(p)) });

  const site = { title: input.site.title, base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cardOf(p));
  const output = {};
  const render = (name, vars) => {
    const S = Object.create(null);
    S.site = site;
    S.tags = allTags;
    S.recent = recent;
    for (const k in vars) S[k] = vars[k];
    if (templates.funcs !== null) {
      const fn = templates.funcs[name];
      if (fn === undefined) return null;
      return fn(S, templates.funcs, 0);
    }
    const ast = templates.asts[name];
    if (ast === undefined) return null;
    return renderNodes(ast, S, templates.asts, 0);
  };
  for (const s of sources) output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s) });
  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      posts: posts.slice((k - 1) * per, k * per).map((p) => cardOf(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of tags) {
    const html = render('tag', { tag: tagOf(t) });
    if (html !== null) output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) output['tags/index.html'] = tagsHtml;
  for (const f of statics) {
    if (Object.hasOwn(output, f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }

  // 8. The report: every refusal, draft, broken link and template that did not parse, sorted, then the counts.
  const lines = [...report, ...broken].sort(byCodeUnits);
  const pageCount = sources.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  output['build-report.txt'] = `${lines.join('\n')}\n`;
  return output;
}

/** makefun-site 1.0.0's answer for one case's input. */
export function run(input) {
  slugCache.clear();
  return build(input);
}
