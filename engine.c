/* engine.c — a native drop-in for makefun-site 1.0.0 (site-generator-speed-01).
 *
 * Written fresh in this session. build_all(raw_cases_json) parses the whole
 * cases.json text, builds every case exactly as makefun-site 1.0.0 does, and
 * returns the answers.json text. Everything is integer work in C: no JIT, no
 * GC, no per-operation interpreter overhead. The only allocation pattern is a
 * bump arena (no malloc/free churn) plus a few growable buffers.
 *
 * Exactness notes (all verified against the reference in the kit):
 *  - slug: NFKD via the generated table (slugtab.h), combining marks dropped,
 *    A-Z lowered, runs of non [a-z0-9] one '-', cut to 64, 'untitled' if empty.
 *  - strings are compared in code-unit order; for the ASCII paths, dates,
 *    slugs and report lines this is byte order.
 *  - JSON numbers keep their raw token: the input is JSON.stringify output,
 *    whose number text is JavaScript's own digits, which is what templates
 *    print. Arithmetic (per_page, recent) uses the parsed int64.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "slugtab.h"

/* ------------------------------------------------------------------ arena */

#define ARENA_BLOCK (1u << 24) /* 16 MiB blocks */

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t used, cap;
  char data[];
} ArenaBlock;

typedef struct {
  ArenaBlock *head;
} Arena;

static void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7u) & ~(size_t)7u;
  ArenaBlock *b = a->head;
  if (!b || b->used + n > b->cap) {
    size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
    b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!b) return NULL;
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
  }
  void *p = b->data + b->used;
  b->used += n;
  return p;
}

static void arena_free_all(Arena *a) {
  ArenaBlock *b = a->head;
  while (b) {
    ArenaBlock *n = b->next;
    free(b);
    b = n;
  }
  a->head = NULL;
}

static char *arena_dup(Arena *a, const char *s, size_t n) {
  char *p = (char *)arena_alloc(a, n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

/* ------------------------------------------------------------------ buf */

/* A growable byte buffer (malloc/realloc). */
typedef struct {
  char *buf;
  size_t len, cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra <= b->cap) return 1;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < b->len + extra) nc *= 2;
  char *nb = (char *)realloc(b->buf, nc);
  if (!nb) return 0;
  b->buf = nb;
  b->cap = nc;
  return 1;
}

static int buf_addn(Buf *b, const char *s, size_t n) {
  if (!buf_reserve(b, n)) return 0;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  return 1;
}

static int buf_addc(Buf *b, char c) {
  if (!buf_reserve(b, 1)) return 0;
  b->buf[b->len++] = c;
  return 1;
}

static int buf_adds(Buf *b, const char *s) { return buf_addn(b, s, strlen(s)); }

/* ------------------------------------------------------------------ str */

typedef struct {
  const char *p;
  size_t n;
} Str;

static Str str_make(const char *p, size_t n) {
  Str s;
  s.p = p;
  s.n = n;
  return s;
}

/* A string literal as a Str (length from strlen). */
static Str str_lit(const char *p) { return str_make(p, strlen(p)); }

static int str_eq(Str a, const char *b) {
  size_t n = strlen(b);
  return a.n == n && (n == 0 || memcmp(a.p, b, n) == 0);
}

static int str_cmp(Str a, Str b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = n == 0 ? 0 : memcmp(a.p, b.p, n);
  if (c != 0) return c;
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

/* ------------------------------------------------------------------ utf-8 */

/* Decode one UTF-8 code point at s[i..n). Returns bytes consumed (0 on bad
 * input, consuming 1 byte as U+FFFD-like replacement: we treat a bad byte as
 * a single gap char; the input is valid UTF-8 from JSON.stringify, so this
 * path is unreachable in practice). */
static size_t utf8_decode(const char *s, size_t n, size_t i, uint32_t *cp) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < n) {
    unsigned char c1 = (unsigned char)s[i + 1];
    if ((c1 & 0xC0) == 0x80) {
      *cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
      return 2;
    }
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < n) {
    unsigned char c1 = (unsigned char)s[i + 1], c2 = (unsigned char)s[i + 2];
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
      *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) | (c2 & 0x3F);
      return 3;
    }
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < n) {
    unsigned char c1 = (unsigned char)s[i + 1], c2 = (unsigned char)s[i + 2], c3 = (unsigned char)s[i + 3];
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
      *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(c1 & 0x3F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
      return 4;
    }
  }
  *cp = 0xFFFD;
  return 1;
}

/* Encode cp as UTF-8 into out (which must hold 4 bytes). Returns length. */
static size_t utf8_encode(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static int is_mark(uint32_t cp) {
  int lo = 0, hi = MARK_RANGES_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (cp < MARK_RANGES[mid].lo) hi = mid - 1;
    else if (cp > MARK_RANGES[mid].hi) lo = mid + 1;
    else return 1;
  }
  return 0;
}

static const char *slug_expansion(uint32_t cp) {
  int lo = 0, hi = SLUG_TAB_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (cp < SLUG_TAB[mid].cp) hi = mid - 1;
    else if (cp > SLUG_TAB[mid].cp) lo = mid + 1;
    else return SLUG_TAB[mid].exp;
  }
  return NULL;
}

/* ------------------------------------------------------------------ slug */

/* The slug of text, written to out (a Buf). Mirrors text.js slugify exactly:
 * NFKD (via table), marks dropped, A-Z lowered, each run of non [a-z0-9]
 * one '-', none at either end, cut to 64 (a '-' left at the cut dropped),
 * 'untitled' when nothing is left. */
static void slugify_buf(Str text, Buf *out) {
  size_t start = out->len;
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    if (c < 0x80) {
      int keep = 0;
      if (c >= 'A' && c <= 'Z') keep = c + 32;
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = (int)c;
      if (keep == 0) {
        if (out->len > start) gap = 1;
      } else {
        if (gap) buf_addc(out, '-');
        gap = 0;
        buf_addc(out, (char)keep);
      }
      i++;
      continue;
    }
    uint32_t cp;
    size_t adv = utf8_decode(text.p, text.n, i, &cp);
    i += adv;
    if (is_mark(cp)) continue;
    const char *exp = slug_expansion(cp);
    if (exp == NULL) {
      if (out->len > start) gap = 1;
      continue;
    }
    for (const char *e = exp; *e; e++) {
      unsigned char ec = (unsigned char)*e;
      int keep = 0;
      if (ec >= 'A' && ec <= 'Z') keep = ec + 32;
      else if ((ec >= 'a' && ec <= 'z') || (ec >= '0' && ec <= '9')) keep = (int)ec;
      if (keep == 0) {
        if (out->len > start) gap = 1;
      } else {
        if (gap) buf_addc(out, '-');
        gap = 0;
        buf_addc(out, (char)keep);
      }
    }
  }
  /* Cut to 64, dropping a '-' left at the cut. */
  size_t len = out->len - start;
  if (len > 64) {
    len = 64;
    while (len > 0 && out->buf[start + len - 1] == '-') len--;
    out->len = start + len;
  }
  if (out->len == start) buf_adds(out, "untitled");
}

/* slugify into a fresh arena string. */
static Str slugify_str(Arena *a, Str text) {
  Buf b = {0};
  slugify_buf(text, &b);
  char *p = arena_dup(a, b.buf ? b.buf : "", b.len);
  free(b.buf);
  Str s = str_make(p ? p : "", b.len);
  return s;
}

/* uniqueSlug: first of slug, slug-2, slug-3 ... not in taken (a Str set). */
typedef struct {
  Str *items;
  size_t n, cap;
  Arena *arena;
} StrSet;

static int strset_has(StrSet *s, Str v) {
  for (size_t i = 0; i < s->n; i++)
    if (s->items[i].n == v.n && memcmp(s->items[i].p, v.p, v.n) == 0) return 1;
  return 0;
}

static void strset_add(Arena *a, StrSet *s, Str v) {
  if (s->n == s->cap) {
    size_t nc = s->cap ? s->cap * 2 : 16;
    Str *ni = (Str *)arena_alloc(a, nc * sizeof(Str));
    if (s->items) memcpy(ni, s->items, s->n * sizeof(Str));
    s->items = ni;
    s->cap = nc;
  }
  s->items[s->n++] = v;
}

static Str unique_slug(Arena *a, Str slug, StrSet *taken) {
  if (!strset_has(taken, slug)) {
    strset_add(a, taken, slug);
    return slug;
  }
  Buf b = {0};
  for (long n = 2;; n++) {
    b.len = 0;
    buf_addn(&b, slug.p, slug.n);
    char nb[24];
    int nl = 0;
    long t = n;
    char rev[24];
    int rl = 0;
    do {
      rev[rl++] = (char)('0' + (t % 10));
      t /= 10;
    } while (t > 0);
    nb[nl++] = '-';
    while (rl > 0) nb[nl++] = rev[--rl];
    buf_addn(&b, nb, (size_t)nl);
    Str cand = str_make(arena_dup(a, b.buf, b.len), b.len);
    if (!strset_has(taken, cand)) {
      free(b.buf);
      strset_add(a, taken, cand);
      return cand;
    }
  }
}

/* ------------------------------------------------------------------ text */

static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

/* longDate: "2026-11-09" -> "9 November 2026". The date string is YYYY-MM-DD. */
static void long_date_buf(Str date, Buf *out) {
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  char nb[8];
  int nl = 0;
  if (d >= 10) nb[nl++] = (char)('0' + d / 10);
  nb[nl++] = (char)('0' + d % 10);
  buf_addn(out, nb, (size_t)nl);
  buf_addc(out, ' ');
  buf_adds(out, MONTHS[m - 1]);
  buf_addc(out, ' ');
  buf_addn(out, date.p, 4);
}

/* HTML escaping: & < > " ' only. Appends to out. Scans in bulk runs (one table
 * lookup per byte) instead of a memchr per special, so a long run of plain
 * text is a single memcpy. */
static const unsigned char ESC_TABLE[256] = {
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1,
};

static void esc_append(Buf *out, Str s) {
  size_t i = 0;
  while (i < s.n) {
    size_t j = i;
    while (j < s.n && !ESC_TABLE[(unsigned char)s.p[j]]) j++;
    if (j > i) buf_addn(out, s.p + i, j - i);
    if (j >= s.n) break;
    switch (s.p[j]) {
      case '&': buf_adds(out, "&amp;"); break;
      case '<': buf_adds(out, "&lt;"); break;
      case '>': buf_adds(out, "&gt;"); break;
      case '"': buf_adds(out, "&quot;"); break;
      default: buf_adds(out, "&#39;"); break;
    }
    i = j + 1;
  }
}

/* Whether cp is JavaScript whitespace (\s). */
static int is_js_space(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20) return 1;
  if (cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF) return 1;
  if (cp >= 0x2000 && cp <= 0x200A) return 1;
  return 0;
}

/* wordsOf: runs of non-whitespace. */
static long long words_of(Str text) {
  long long n = 0;
  int in_word = 0;
  size_t i = 0;
  while (i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    if (c < 0x80) {
      int ws = (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r');
      if (ws) in_word = 0;
      else if (!in_word) {
        in_word = 1;
        n++;
      }
      i++;
    } else {
      uint32_t cp;
      size_t adv = utf8_decode(text.p, text.n, i, &cp);
      i += adv;
      if (is_js_space(cp)) in_word = 0;
      else if (!in_word) {
        in_word = 1;
        n++;
      }
    }
  }
  return n;
}

/* ------------------------------------------------------------------ JSON */

/* A minimal JSON reader over the raw input bytes. Strings are returned as
 * views into the input when they hold no backslash, else unescaped into the
 * arena. Numbers keep their raw token (JavaScript's own digits). */
typedef struct {
  const char *p, *end;
  Arena *arena;
} JParser;

static void jp_ws(JParser *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int jp_expect(JParser *j, char c) {
  jp_ws(j);
  if (j->p < j->end && *j->p == c) {
    j->p++;
    return 1;
  }
  return 0;
}

/* Parse a JSON string. Returns 0 on error. */
static int jp_string(JParser *j, Str *out) {
  jp_ws(j);
  if (j->p >= j->end || *j->p != '"') return 0;
  const char *start = ++j->p;
  const char *s = start;
  while (s < j->end) {
    unsigned char c = (unsigned char)*s;
    if (c == '"') {
      *out = str_make(start, (size_t)(s - start));
      j->p = s + 1;
      return 1;
    }
    if (c == '\\') break;
    if (c < 0x20) return 0;
    s++;
  }
  /* Escapes present: unescape into the arena, copying each run with no
   * '"' and no '\\' in bulk. */
  Buf b = {0};
  s = start;
  while (s < j->end) {
    const char *r = s;
    while (r < j->end) {
      unsigned char c = (unsigned char)*r;
      if (c == '"' || c == '\\' || c < 0x20) break;
      r++;
    }
    if (r > s) buf_addn(&b, s, (size_t)(r - s));
    s = r;
    if (s >= j->end) {
      free(b.buf);
      return 0;
    }
    unsigned char c = (unsigned char)*s;
    if (c == '"') {
      char *p = arena_dup(j->arena, b.buf ? b.buf : "", b.len);
      free(b.buf);
      if (!p) return 0;
      *out = str_make(p, b.len);
      j->p = s + 1;
      return 1;
    }
    if (c < 0x20) {
      free(b.buf);
      return 0;
    }
    /* c == '\\': an escape. */
    {
      if (s + 1 >= j->end) {
        free(b.buf);
        return 0;
      }
      char e = s[1];
      if (e == '"' || e == '\\' || e == '/') {
        buf_addc(&b, e);
        s += 2;
      } else if (e == 'b') {
        buf_addc(&b, '\b');
        s += 2;
      } else if (e == 'f') {
        buf_addc(&b, '\f');
        s += 2;
      } else if (e == 'n') {
        buf_addc(&b, '\n');
        s += 2;
      } else if (e == 'r') {
        buf_addc(&b, '\r');
        s += 2;
      } else if (e == 't') {
        buf_addc(&b, '\t');
        s += 2;
      } else if (e == 'u') {
        if (s + 5 >= j->end) {
          free(b.buf);
          return 0;
        }
        uint32_t cp = 0;
        for (int k = 0; k < 4; k++) {
          char h = s[2 + k];
          cp <<= 4;
          if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
          else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
          else {
            free(b.buf);
            return 0;
          }
        }
        s += 6;
        if (cp >= 0xD800 && cp <= 0xDBFF && s + 5 < j->end && s[0] == '\\' && s[1] == 'u') {
          uint32_t lo = 0;
          int ok = 1;
          for (int k = 0; k < 4; k++) {
            char h = s[2 + k];
            lo <<= 4;
            if (h >= '0' && h <= '9') lo |= (uint32_t)(h - '0');
            else if (h >= 'a' && h <= 'f') lo |= (uint32_t)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') lo |= (uint32_t)(h - 'A' + 10);
            else ok = 0;
          }
          if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            s += 6;
          }
        }
        char enc[4];
        size_t el = utf8_encode(cp, enc);
        buf_addn(&b, enc, el);
      } else {
        free(b.buf);
        return 0;
      }
    }
  }
  free(b.buf);
  return 0;
}

/* Parse a JSON number: raw token + int64 when it is an integer. */
static int jp_number(JParser *j, Str *raw, long long *ival, int *is_int) {
  jp_ws(j);
  const char *s = j->p;
  if (s < j->end && *s == '-') s++;
  while (s < j->end) {
    char c = *s;
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') s++;
    else break;
  }
  if (s == j->p) return 0;
  *raw = str_make(j->p, (size_t)(s - j->p));
  j->p = s;
  *is_int = 1;
  for (size_t k = 0; k < raw->n; k++) {
    char c = raw->p[k];
    if (c == '.' || c == 'e' || c == 'E') {
      *is_int = 0;
      break;
    }
  }
  *ival = 0;
  if (*is_int) {
    size_t k = 0;
    int neg = 0;
    if (k < raw->n && raw->p[k] == '-') {
      neg = 1;
      k++;
    }
    long long v = 0;
    for (; k < raw->n; k++) v = v * 10 + (raw->p[k] - '0');
    *ival = neg ? -v : v;
  }
  return 1;
}

/* Skip one JSON value (for unknown keys). */
static int jp_skip(JParser *j) {
  jp_ws(j);
  if (j->p >= j->end) return 0;
  char c = *j->p;
  if (c == '"') {
    Str tmp;
    return jp_string(j, &tmp);
  }
  if (c == '{') {
    j->p++;
    if (jp_expect(j, '}')) return 1;
    for (;;) {
      Str k;
      if (!jp_string(j, &k)) return 0;
      if (!jp_expect(j, ':')) return 0;
      if (!jp_skip(j)) return 0;
      if (jp_expect(j, ',')) continue;
      if (jp_expect(j, '}')) return 1;
      return 0;
    }
  }
  if (c == '[') {
    j->p++;
    if (jp_expect(j, ']')) return 1;
    for (;;) {
      if (!jp_skip(j)) return 0;
      if (jp_expect(j, ',')) continue;
      if (jp_expect(j, ']')) return 1;
      return 0;
    }
  }
  if (c == 't') {
    if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "true", 4) == 0) {
      j->p += 4;
      return 1;
    }
    return 0;
  }
  if (c == 'f') {
    if ((size_t)(j->end - j->p) >= 5 && memcmp(j->p, "false", 5) == 0) {
      j->p += 5;
      return 1;
    }
    return 0;
  }
  if (c == 'n') {
    if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "null", 4) == 0) {
      j->p += 4;
      return 1;
    }
    return 0;
  }
  Str raw;
  long long iv;
  int ii;
  return jp_number(j, &raw, &iv, &ii);
}

/* ------------------------------------------------------------------ front matter */

/* Split s into lines (views, '\n' excluded). Returns the count; lines are
 * written to the arena array *out. */
static size_t split_lines(Arena *a, Str s, Str **out) {
  size_t n = 1;
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] == '\n') n++;
  Str *lines = (Str *)arena_alloc(a, n * sizeof(Str));
  size_t k = 0, start = 0;
  for (size_t i = 0; i <= s.n; i++) {
    if (i == s.n || s.p[i] == '\n') {
      lines[k++] = str_make(s.p + start, i - start);
      start = i + 1;
    }
  }
  *out = lines;
  return k;
}

typedef struct {
  Str title, summary;
  int has_summary;
  Str date;
  int has_date;
  Str *tags;
  size_t ntags;
  int has_tags;
  int draft;
  int has_draft;
  Str layout;
  int has_layout;
  Str slug;
  int has_slug;
  int has_title;
} FmData;

static int is_fm_key(const char *k, size_t n) {
  static const char *KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
  for (int i = 0; i < 7; i++) {
    size_t kn = strlen(KEYS[i]);
    if (n == kn && memcmp(k, KEYS[i], n) == 0) return 1;
  }
  return 0;
}

static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
  return 31;
}

/* JS trim (whitespace) of a Str view. */
static Str js_trim(Str s) {
  size_t a = 0, b = s.n;
  while (a < b) {
    unsigned char c = (unsigned char)s.p[a];
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') a++;
      else break;
    } else {
      uint32_t cp;
      size_t adv = utf8_decode(s.p, s.n, a, &cp);
      if (!is_js_space(cp)) break;
      a += adv;
    }
  }
  while (b > a) {
    size_t i = b - 1;
    while (i > a && ((unsigned char)s.p[i] & 0xC0) == 0x80) i--;
    unsigned char c = (unsigned char)s.p[i];
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') b = i;
      else break;
    } else {
      uint32_t cp;
      utf8_decode(s.p, s.n, i, &cp);
      if (!is_js_space(cp)) break;
      b = i;
    }
  }
  return str_make(s.p + a, b - a);
}

/* Format an error into the arena: a printf-style helper for the few fixed
 * messages that carry a value. */
static Str fmt_err(Arena *a, const char *pre, Str v, const char *post) {
  Buf b = {0};
  buf_adds(&b, pre);
  buf_addn(&b, v.p, v.n);
  buf_adds(&b, post);
  char *p = arena_dup(a, b.buf ? b.buf : "", b.len);
  free(b.buf);
  return str_make(p ? p : "", b.len);
}

/* One key's value, or the refusal reason (arena string). Returns 1 on ok. */
static int fm_value_of(Arena *a, Str key, Str raw, FmData *d, Str *err) {
  if (str_eq(key, "title") || str_eq(key, "summary")) {
    /* stringOf */
    if (raw.n > 0 && raw.p[0] == '"') {
      if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
        *err = fmt_err(a, "key \"", key, "\": a quoted string is not closed");
        return 0;
      }
      Buf b = {0};
      for (size_t i = 1; i + 1 < raw.n; i++) {
        char c = raw.p[i];
        if (c == '\\') {
          char nn = raw.p[i + 1];
          if ((nn != '"' && nn != '\\') || i + 1 == raw.n - 1) {
            free(b.buf);
            *err = fmt_err(a, "key \"", key, "\": a quoted string holds an escape other than \\\" or \\\\");
            return 0;
          }
          buf_addc(&b, nn);
          i++;
        } else if (c == '"') {
          free(b.buf);
          *err = fmt_err(a, "key \"", key, "\": a quoted string holds a stray quote");
          return 0;
        } else {
          buf_addc(&b, c);
        }
      }
      Str v = str_make(arena_dup(a, b.buf ? b.buf : "", b.len), b.len);
      free(b.buf);
      if (str_eq(key, "title")) d->title = v;
      else {
        d->summary = v;
        d->has_summary = 1;
      }
      return 1;
    }
    if (raw.n > 0 && raw.p[0] == '[') {
      *err = fmt_err(a, "key \"", key, "\": a bare string opens on [");
      return 0;
    }
    if (str_eq(key, "title")) d->title = raw;
    else {
      d->summary = raw;
      d->has_summary = 1;
    }
    return 1;
  }
  if (str_eq(key, "date")) {
    int ok = 0;
    if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
      int dig = 1;
      for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw.p[i] < '0' || raw.p[i] > '9') dig = 0;
      }
      if (dig) {
        int y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
        int mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
        int dd = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= days_in(y, mo)) ok = 1;
      }
    }
    if (!ok) {
      *err = str_lit("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return 0;
    }
    d->date = raw;
    d->has_date = 1;
    return 1;
  }
  if (str_eq(key, "tags")) {
    if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
      *err = str_lit("key \"tags\": not a list [a, b]");
      return 0;
    }
    Str inner = str_make(raw.p + 1, raw.n - 2);
    if (js_trim(inner).n == 0) {
      d->tags = NULL;
      d->ntags = 0;
      d->has_tags = 1;
      return 1;
    }
    /* split(',') then trim each */
    size_t cap = 4, n = 0;
    Str *tags = (Str *)arena_alloc(a, cap * sizeof(Str));
    size_t start = 0;
    for (size_t i = 0; i <= inner.n; i++) {
      if (i == inner.n || inner.p[i] == ',') {
        Str t = js_trim(str_make(inner.p + start, i - start));
        if (t.n == 0) {
          *err = str_lit("key \"tags\": an empty tag, or one holding \" [ or ]");
          return 0;
        }
        for (size_t k = 0; k < t.n; k++) {
          if (t.p[k] == '"' || t.p[k] == '[' || t.p[k] == ']') {
            *err = str_lit("key \"tags\": an empty tag, or one holding \" [ or ]");
            return 0;
          }
        }
        if (n == cap) {
          cap *= 2;
          Str *nt = (Str *)arena_alloc(a, cap * sizeof(Str));
          memcpy(nt, tags, n * sizeof(Str));
          tags = nt;
        }
        tags[n++] = t;
        start = i + 1;
      }
    }
    d->tags = tags;
    d->ntags = n;
    d->has_tags = 1;
    return 1;
  }
  if (str_eq(key, "draft")) {
    if (str_eq(raw, "true")) {
      d->draft = 1;
      d->has_draft = 1;
      return 1;
    }
    if (str_eq(raw, "false")) {
      d->draft = 0;
      d->has_draft = 1;
      return 1;
    }
    *err = str_lit("key \"draft\": true or false");
    return 0;
  }
  if (str_eq(key, "layout")) {
    int ok = raw.n > 0;
    for (size_t i = 0; i < raw.n && ok; i++) {
      char c = raw.p[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
    }
    if (!ok) {
      *err = str_lit("key \"layout\": not a template name");
      return 0;
    }
    d->layout = raw;
    d->has_layout = 1;
    return 1;
  }
  if (str_eq(key, "slug")) {
    int ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      /* ^[a-z0-9]+(?:-[a-z0-9]+)*$ */
      size_t i = 0;
      while (i < raw.n) {
        if (!((raw.p[i] >= 'a' && raw.p[i] <= 'z') || (raw.p[i] >= '0' && raw.p[i] <= '9'))) {
          ok = 0;
          break;
        }
        while (i < raw.n && ((raw.p[i] >= 'a' && raw.p[i] <= 'z') || (raw.p[i] >= '0' && raw.p[i] <= '9'))) i++;
        if (i < raw.n) {
          if (raw.p[i] != '-') {
            ok = 0;
            break;
          }
          i++;
          if (i >= raw.n) {
            ok = 0;
            break;
          }
        }
      }
    }
    if (!ok) {
      *err = str_lit("key \"slug\": not a slug");
      return 0;
    }
    d->slug = raw;
    d->has_slug = 1;
    return 1;
  }
  *err = fmt_err(a, "unknown key \"", key, "\"");
  return 0;
}

/* Parse front matter. Returns 1 on success (data + body set), 0 on refusal
 * (err set to the reason). */
static int parse_fm(Arena *a, Str text, FmData *d, Str *body_out, Str *err) {
  memset(d, 0, sizeof(*d));
  Str *lines = NULL;
  size_t nlines = split_lines(a, text, &lines);
  if (nlines == 0 || !str_eq(lines[0], "---")) {
    *err = str_lit("no front matter");
    return 0;
  }
  long close = -1;
  for (size_t i = 1; i < nlines; i++) {
    if (str_eq(lines[i], "---")) {
      close = (long)i;
      break;
    }
  }
  if (close == -1) {
    *err = str_lit("the front matter is not closed");
    return 0;
  }
  for (long i = 1; i < close; i++) {
    Str line = lines[i];
    int blank = 1;
    for (size_t k = 0; k < line.n; k++) {
      if (line.p[k] != ' ' && line.p[k] != '\t') {
        blank = 0;
        break;
      }
    }
    if (blank) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    /* LINE: ^([a-z_]+):(?: (.*))?$ */
    size_t k = 0;
    while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_')) k++;
    if (k == 0 || k >= line.n || line.p[k] != ':') {
      Buf b = {0};
      buf_adds(&b, "line ");
      char nb[24];
      long ln = i + 1;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (ln % 10));
        ln /= 10;
      } while (ln > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&b, nb, (size_t)nl);
      buf_adds(&b, " is not \"key: value\"");
      char *p = arena_dup(a, b.buf, b.len);
      free(b.buf);
      *err = str_make(p, b.len);
      return 0;
    }
    Str key = str_make(line.p, k);
    Str raw;
    if (k + 1 == line.n) {
      raw = str_make(line.p + k + 1, 0);
    } else if (line.p[k + 1] == ' ') {
      raw = str_make(line.p + k + 2, line.n - (k + 2));
    } else {
      Buf b = {0};
      buf_adds(&b, "line ");
      char nb[24];
      long ln = i + 1;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (ln % 10));
        ln /= 10;
      } while (ln > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&b, nb, (size_t)nl);
      buf_adds(&b, " is not \"key: value\"");
      char *p = arena_dup(a, b.buf, b.len);
      free(b.buf);
      *err = str_make(p, b.len);
      return 0;
    }
    /* raw: trailing [ \t] dropped */
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    if (!is_fm_key(key.p, key.n)) {
      *err = fmt_err(a, "unknown key \"", key, "\"");
      return 0;
    }
    int dup = 0;
    if (str_eq(key, "title")) dup = d->has_title;
    else if (str_eq(key, "date")) dup = d->has_date;
    else if (str_eq(key, "tags")) dup = d->has_tags;
    else if (str_eq(key, "draft")) dup = d->has_draft;
    else if (str_eq(key, "layout")) dup = d->has_layout;
    else if (str_eq(key, "slug")) dup = d->has_slug;
    else if (str_eq(key, "summary")) dup = d->has_summary;
    if (dup) {
      *err = fmt_err(a, "key \"", key, "\" is given twice");
      return 0;
    }
    if (raw.n == 0) {
      *err = fmt_err(a, "key \"", key, "\" has no value");
      return 0;
    }
    if (!fm_value_of(a, key, raw, d, err)) return 0;
    if (str_eq(key, "title")) d->has_title = 1;
  }
  if (!d->has_title) {
    *err = str_lit("no title");
    return 0;
  }
  /* body = lines after the closing line, rejoined with '\n' */
  size_t body_start = 0;
  {
    /* find the byte offset just past the '\n' ending line `close` */
    size_t pos = 0;
    for (long i = 0; i <= close; i++) {
      pos += lines[i].n + 1; /* line + its '\n' (the last line may lack one) */
      if (i == close) break;
    }
    if (pos <= text.n) body_start = pos;
    else body_start = text.n;
  }
  *body_out = str_make(text.p + body_start, text.n - body_start);
  return 1;
}

/* ------------------------------------------------------------------ markdown */

typedef struct {
  Str (*link)(void *ctx, Str target);
  void *ctx;
} MdEnv;

static int is_blank_line(Str l) {
  for (size_t i = 0; i < l.n; i++)
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  return 1;
}

/* FENCE_OPEN: ``` + lang [A-Za-z0-9_+-]* + [ \t]*. Returns 1 and sets lang. */
static int fence_open(Str l, Str *lang) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < l.n) {
    char c = l.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') i++;
    else break;
  }
  size_t lang_end = i;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  if (i != l.n) return 0;
  *lang = str_make(l.p + 3, lang_end - 3);
  return 1;
}

static int fence_close(Str l) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  for (size_t i = 3; i < l.n; i++)
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  return 1;
}

/* HEADING: 1-6 '#', >=1 [ \t], text (trailing [ \t] dropped), non-empty. */
static int heading_of(Str l, int *depth, Str *text) {
  size_t i = 0;
  while (i < l.n && l.p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i >= l.n || (l.p[i] != ' ' && l.p[i] != '\t')) return 0;
  int d = (int)i; /* the depth is the hash count, before the spaces */
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  size_t end = l.n;
  while (end > i && (l.p[end - 1] == ' ' || l.p[end - 1] == '\t')) end--;
  if (end == i) return 0;
  *depth = d;
  *text = str_make(l.p + i, end - i);
  return 1;
}

static int is_hr(Str l) {
  if (l.n < 3) return 0;
  if (memcmp(l.p, "---", 3) != 0 && memcmp(l.p, "***", 3) != 0) return 0;
  for (size_t i = 3; i < l.n; i++)
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  return 1;
}

/* BULLET: [-*] + ' ' + rest. Returns 1 and sets item text. */
static int bullet_of(Str l, Str *item) {
  if (l.n < 2) return 0;
  if ((l.p[0] != '-' && l.p[0] != '*') || l.p[1] != ' ') return 0;
  *item = str_make(l.p + 2, l.n - 2);
  return 1;
}

/* ORDERED: 1-9 digits + '. ' + rest. Returns 1, sets number and item text. */
static int ordered_of(Str l, long *num, Str *item) {
  size_t i = 0;
  while (i < l.n && l.p[i] >= '0' && l.p[i] <= '9') i++;
  if (i == 0 || i > 9) return 0;
  if (i + 1 >= l.n || l.p[i] != '.' || l.p[i + 1] != ' ') return 0;
  long v = 0;
  for (size_t k = 0; k < i; k++) v = v * 10 + (l.p[k] - '0');
  *num = v;
  *item = str_make(l.p + i + 2, l.n - (i + 2));
  return 1;
}

/* CONTINUED: >=2 spaces then a non-whitespace char. */
static int is_continued(Str l) {
  size_t i = 0;
  while (i < l.n && l.p[i] == ' ') i++;
  if (i < 2 || i >= l.n) return 0;
  unsigned char c = (unsigned char)l.p[i];
  if (c < 0x80) return !(c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r');
  uint32_t cp;
  utf8_decode(l.p, l.n, i, &cp);
  return !is_js_space(cp);
}

static Str trim_line(Str l) {
  size_t a = 0, b = l.n;
  while (a < b && (l.p[a] == ' ' || l.p[a] == '\t')) a++;
  while (b > a && (l.p[b - 1] == ' ' || l.p[b - 1] == '\t')) b--;
  return str_make(l.p + a, b - a);
}

static int ends_paragraph(Str l) {
  Str lang;
  int d;
  Str t;
  return is_blank_line(l) || fence_open(l, &lang) || heading_of(l, &d, &t) || is_hr(l);
}

typedef enum { TK_HEADING, TK_CODE, TK_HR, TK_QUOTE, TK_LIST, TK_PARA } TkKind;

typedef struct {
  TkKind kind;
  int depth;
  Str text;
  Str lang;
  Str *lines;
  size_t nlines;
  int ordered;
  long start;
  Str *items;
  size_t nitems;
} Token;

/* Lex src into tokens (arena). */
static size_t lex_md(Arena *a, Str src, Token **out) {
  Str *lines = NULL;
  size_t nlines = split_lines(a, src, &lines);
  size_t cap = 16, n = 0;
  Token *toks = (Token *)arena_alloc(a, cap * sizeof(Token));
  size_t i = 0;
  while (i < nlines) {
    Str line = lines[i];
    if (is_blank_line(line)) {
      i++;
      continue;
    }
    Str lang;
    if (fence_open(line, &lang)) {
      size_t bcap = 8, bn = 0;
      Str *body = (Str *)arena_alloc(a, bcap * sizeof(Str));
      i++;
      while (i < nlines && !fence_close(lines[i])) {
        if (bn == bcap) {
          bcap *= 2;
          Str *nb = (Str *)arena_alloc(a, bcap * sizeof(Str));
          memcpy(nb, body, bn * sizeof(Str));
          body = nb;
        }
        body[bn++] = lines[i++];
      }
      i++;
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_CODE;
      t->lang = lang;
      t->lines = body;
      t->nlines = bn;
      continue;
    }
    int depth;
    Str htext;
    if (heading_of(line, &depth, &htext)) {
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_HEADING;
      t->depth = depth;
      t->text = htext;
      i++;
      continue;
    }
    if (is_hr(line)) {
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_HR;
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      size_t bcap = 8, bn = 0;
      Str *body = (Str *)arena_alloc(a, bcap * sizeof(Str));
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        Str l = lines[i++];
        size_t st = 1;
        if (st < l.n && l.p[st] == ' ') st++;
        Str tl = trim_line(str_make(l.p + st, l.n - st));
        if (tl.n == 0) continue;
        if (bn == bcap) {
          bcap *= 2;
          Str *nb = (Str *)arena_alloc(a, bcap * sizeof(Str));
          memcpy(nb, body, bn * sizeof(Str));
          body = nb;
        }
        body[bn++] = tl;
      }
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_QUOTE;
      t->lines = body;
      t->nlines = bn;
      continue;
    }
    Str bitem, oitem;
    long onum = 0;
    int is_bullet = bullet_of(line, &bitem);
    int is_ordered = !is_bullet && ordered_of(line, &onum, &oitem);
    if (is_bullet || is_ordered) {
      long first_num = onum;
      size_t icap = 4, in = 0;
      Str *items = (Str *)arena_alloc(a, icap * sizeof(Str));
      /* each item's parts, joined later with '\n' */
      Buf cur = {0};
      int have_cur = 0;
      while (i < nlines) {
        Str bi, oi;
        long on = 0;
        int mb = is_ordered ? 0 : bullet_of(lines[i], &bi);
        int mo = is_ordered ? ordered_of(lines[i], &on, &oi) : 0;
        if ((is_ordered && mo) || (!is_ordered && mb)) {
          if (have_cur) {
            if (in == icap) {
              icap *= 2;
              Str *ni = (Str *)arena_alloc(a, icap * sizeof(Str));
              memcpy(ni, items, in * sizeof(Str));
              items = ni;
            }
            items[in++] = str_make(arena_dup(a, cur.buf ? cur.buf : "", cur.len), cur.len);
            cur.len = 0;
          }
          Str it = trim_line(is_ordered ? oi : bi);
          buf_addn(&cur, it.p, it.n);
          have_cur = 1;
          i++;
        } else if (is_continued(lines[i])) {
          Str tl = trim_line(lines[i]);
          buf_addc(&cur, '\n');
          buf_addn(&cur, tl.p, tl.n);
          i++;
        } else {
          break;
        }
      }
      if (have_cur) {
        if (in == icap) {
          icap *= 2;
          Str *ni = (Str *)arena_alloc(a, icap * sizeof(Str));
          memcpy(ni, items, in * sizeof(Str));
          items = ni;
        }
        items[in++] = str_make(arena_dup(a, cur.buf ? cur.buf : "", cur.len), cur.len);
      }
      free(cur.buf);
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_LIST;
      t->ordered = is_ordered;
      t->start = is_ordered ? first_num : 1;
      t->items = items;
      t->nitems = in;
      continue;
    }
    /* paragraph */
    {
      Buf pb = {0};
      Str tl = trim_line(line);
      buf_addn(&pb, tl.p, tl.n);
      i++;
      while (i < nlines && !ends_paragraph(lines[i])) {
        Str t2 = trim_line(lines[i++]);
        buf_addc(&pb, '\n');
        buf_addn(&pb, t2.p, t2.n);
      }
      if (n == cap) {
        cap *= 2;
        Token *nt = (Token *)arena_alloc(a, cap * sizeof(Token));
        memcpy(nt, toks, n * sizeof(Token));
        toks = nt;
      }
      Token *t = &toks[n++];
      memset(t, 0, sizeof(*t));
      t->kind = TK_PARA;
      t->text = str_make(arena_dup(a, pb.buf ? pb.buf : "", pb.len), pb.len);
      free(pb.buf);
    }
  }
  *out = toks;
  return n;
}

/* ------------------------------------------------------------------ inline */

static int is_escapable(char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']': case '(': case ')': case '#': case '+': case '-': case '.': case '!': case '>':
      return 1;
    default:
      return 0;
  }
}

/* First index >= from where s holds `what` outside a backslash escape, or -1. */
static long find_unescaped(Str s, const char *what, size_t wlen, size_t from) {
  if (from > s.n || s.n < wlen) return -1;
  size_t last = s.n - wlen;
  for (size_t j = from; j <= last; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.p + j, what, wlen) == 0) return (long)j;
  }
  return -1;
}

/* First single * from `from` (one whose run is not **), outside an escape. */
static long find_single_star(Str s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    char c = s.p[j];
    if (c == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

static int str_has_js_space(Str s) {
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = (unsigned char)s.p[i];
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
      i++;
    } else {
      uint32_t cp;
      size_t adv = utf8_decode(s.p, s.n, i, &cp);
      if (is_js_space(cp)) return 1;
      i += adv;
    }
  }
  return 0;
}

static void inline_append(Buf *out, Str s, MdEnv *env);

/* The characters inline rendering stops at: the Markdown ones plus the five
 * HTML specials. Anything else is copied in bulk. */
static const unsigned char INLINE_TABLE[256] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['['] = 1,
    ['&'] = 1, ['<'] = 1, ['>'] = 1, ['"'] = 1, ['\''] = 1,
};

static void inline_append(Buf *out, Str s, MdEnv *env) {
  size_t i = 0;
  while (i < s.n) {
    size_t j = i;
    while (j < s.n && !INLINE_TABLE[(unsigned char)s.p[j]]) j++;
    if (j > i) buf_addn(out, s.p + i, j - i);
    if (j >= s.n) return;
    i = j;
    unsigned char c = (unsigned char)s.p[i];
    if (c == '\\') {
      if (i + 1 < s.n && is_escapable(s.p[i + 1])) {
        esc_append(out, str_make(s.p + i + 1, 1));
        i += 2;
      } else {
        buf_addc(out, '\\');
        i++;
      }
      continue;
    }
    if (c == '`') {
      const char *found = (const char *)memchr(s.p + i + 1, '`', s.n - (i + 1));
      long k = found ? (long)(found - s.p) : -1;
      if (k > (long)(i + 1)) {
        buf_adds(out, "<code>");
        esc_append(out, str_make(s.p + i + 1, (size_t)(k - (long)(i + 1))));
        buf_adds(out, "</code>");
        i = (size_t)k + 1;
      } else {
        buf_addc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.n && s.p[i + 1] == '*') {
        long k = find_unescaped(s, "**", 2, i + 2);
        if (k > (long)(i + 2)) {
          buf_adds(out, "<strong>");
          inline_append(out, str_make(s.p + i + 2, (size_t)(k - (long)(i + 2))), env);
          buf_adds(out, "</strong>");
          i = (size_t)k + 2;
        } else {
          buf_adds(out, "**");
          i += 2;
        }
        continue;
      }
      long k = find_single_star(s, i + 1);
      if (k > (long)(i + 1)) {
        buf_adds(out, "<em>");
        inline_append(out, str_make(s.p + i + 1, (size_t)(k - (long)(i + 1))), env);
        buf_adds(out, "</em>");
        i = (size_t)k + 1;
      } else {
        buf_addc(out, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, "]", 1, i + 1);
      long m = -1;
      if (k != -1 && (size_t)k + 1 < s.n && s.p[k + 1] == '(') {
        const char *found = (const char *)memchr(s.p + k + 2, ')', s.n - ((size_t)k + 2));
        if (found) m = (long)(found - s.p);
      }
      Str target = m == -1 ? str_make("", 0) : str_make(s.p + k + 2, (size_t)(m - (k + 2)));
      if (target.n > 0 && !str_has_js_space(target)) {
        Str href = env->link(env->ctx, target);
        buf_adds(out, "<a href=\"");
        esc_append(out, href);
        buf_adds(out, "\">");
        inline_append(out, str_make(s.p + i + 1, (size_t)(k - (long)(i + 1))), env);
        buf_adds(out, "</a>");
        i = (size_t)m + 1;
        continue;
      }
      buf_addc(out, '[');
      i++;
      continue;
    }
    /* An HTML special: emit its entity. */
    switch (c) {
      case '&': buf_adds(out, "&amp;"); break;
      case '<': buf_adds(out, "&lt;"); break;
      case '>': buf_adds(out, "&gt;"); break;
      case '"': buf_adds(out, "&quot;"); break;
      default: buf_adds(out, "&#39;"); break;
    }
    i++;
  }
}

/* ------------------------------------------------------------------ render markdown */

typedef struct {
  int level;
  Str id;
  Str html;
} TocEnt;

typedef struct {
  TocEnt *toc;
  size_t ntoc;
  Str excerpt; /* "" when there is no paragraph */
} MdResult;

/* Render src: HTML into out, toc + excerpt into res (arena). tmp is a
 * caller-owned reusable scratch Buf (reset per block). */
static void render_md(Arena *a, Str src, MdEnv *env, Buf *out, MdResult *res, Buf *tmpb) {
  Token *toks = NULL;
  size_t ntoks = lex_md(a, src, &toks);
  StrSet ids = {0};
  ids.arena = a;
  size_t tcap = 8, tn = 0;
  TocEnt *toc = (TocEnt *)arena_alloc(a, tcap * sizeof(TocEnt));
  Str excerpt = str_make("", 0);
  int have_excerpt = 0;
  Buf *tmp = tmpb;
  for (size_t ti = 0; ti < ntoks; ti++) {
    Token *t = &toks[ti];
    switch (t->kind) {
      case TK_HEADING: {
        Str slug = slugify_str(a, t->text);
        Str id = unique_slug(a, slug, &ids);
        tmp->len = 0;
        inline_append(tmp, t->text, env);
        /* Only level-2/3 headings keep their HTML (the toc); the rest are
         * written straight out of tmp with no arena copy. */
        int keep = (t->depth == 2 || t->depth == 3);
        Str html = keep ? str_make(arena_dup(a, tmp->buf ? tmp->buf : "", tmp->len), tmp->len)
                        : str_make(tmp->buf ? tmp->buf : "", tmp->len);
        buf_adds(out, "<h");
        buf_addc(out, (char)('0' + t->depth));
        buf_adds(out, " id=\"");
        buf_addn(out, id.p, id.n);
        buf_adds(out, "\">");
        buf_addn(out, html.p, html.n);
        buf_adds(out, "</h");
        buf_addc(out, (char)('0' + t->depth));
        buf_adds(out, ">\n");
        if (keep) {
          if (tn == tcap) {
            tcap *= 2;
            TocEnt *nt = (TocEnt *)arena_alloc(a, tcap * sizeof(TocEnt));
            memcpy(nt, toc, tn * sizeof(TocEnt));
            toc = nt;
          }
          toc[tn].level = t->depth;
          toc[tn].id = id;
          toc[tn].html = html;
          tn++;
        }
        break;
      }
      case TK_CODE: {
        buf_adds(out, "<pre><code");
        if (t->lang.n > 0) {
          buf_adds(out, " class=\"language-");
          esc_append(out, t->lang);
          buf_addc(out, '"');
        }
        buf_addc(out, '>');
        for (size_t li = 0; li < t->nlines; li++) {
          esc_append(out, t->lines[li]);
          buf_addc(out, '\n');
        }
        buf_adds(out, "</code></pre>\n");
        break;
      }
      case TK_HR:
        buf_adds(out, "<hr>\n");
        break;
      case TK_QUOTE: {
        if (t->nlines == 0) {
          buf_adds(out, "<blockquote>\n</blockquote>\n");
        } else {
          buf_adds(out, "<blockquote>\n<p>");
          /* Join the lines with '\n' into the shared tmp Buf (no malloc). */
          tmp->len = 0;
          for (size_t li = 0; li < t->nlines; li++) {
            if (li > 0) buf_addc(tmp, '\n');
            buf_addn(tmp, t->lines[li].p, t->lines[li].n);
          }
          inline_append(out, str_make(tmp->buf ? tmp->buf : "", tmp->len), env);
          buf_adds(out, "</p>\n</blockquote>\n");
        }
        break;
      }
      case TK_LIST: {
        if (t->ordered) {
          if (t->start != 1) {
            buf_adds(out, "<ol start=\"");
            char nb[24];
            long v = t->start;
            int nl = 0;
            char rev[24];
            int rl = 0;
            do {
              rev[rl++] = (char)('0' + (v % 10));
              v /= 10;
            } while (v > 0);
            while (rl > 0) nb[nl++] = rev[--rl];
            buf_addn(out, nb, (size_t)nl);
            buf_adds(out, "\">\n");
          } else {
            buf_adds(out, "<ol>\n");
          }
        } else {
          buf_adds(out, "<ul>\n");
        }
        for (size_t ii = 0; ii < t->nitems; ii++) {
          buf_adds(out, "<li>");
          inline_append(out, t->items[ii], env);
          buf_adds(out, "</li>\n");
        }
        buf_adds(out, t->ordered ? "</ol>\n" : "</ul>\n");
        break;
      }
      case TK_PARA: {
        tmp->len = 0;
        inline_append(tmp, t->text, env);
        if (!have_excerpt) {
          excerpt = str_make(arena_dup(a, tmp->buf ? tmp->buf : "", tmp->len), tmp->len);
          have_excerpt = 1;
        }
        buf_adds(out, "<p>");
        buf_addn(out, tmp->buf ? tmp->buf : "", tmp->len);
        buf_adds(out, "</p>\n");
        break;
      }
    }
  }
  res->toc = toc;
  res->ntoc = tn;
  res->excerpt = excerpt;
}

/* ------------------------------------------------------------------ template values */

typedef enum { V_STR, V_NUM, V_BOOL, V_ARR, V_REC, V_NULL } VType;

typedef struct Val Val;
typedef struct {
  Str name;
  Val *val;
} Field;

struct Val {
  VType t;
  union {
    Str s;
    long long num;
    int b;
    struct {
      Val **items;
      size_t n;
    } arr;
    struct {
      Field *fields;
      size_t n;
    } rec;
  } u;
};

static Val *val_str(Arena *a, Str s) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->t = V_STR;
  v->u.s = s;
  return v;
}

static Val *val_num(Arena *a, long long n) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->t = V_NUM;
  v->u.num = n;
  return v;
}

static Val *val_bool(Arena *a, int b) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->t = V_BOOL;
  v->u.b = b ? 1 : 0;
  return v;
}

static Val val_null_singleton = {V_NULL, {{0}}};

static Val *val_arr(Arena *a, Val **items, size_t n) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->t = V_ARR;
  v->u.arr.items = items;
  v->u.arr.n = n;
  return v;
}

static Val *val_rec(Arena *a, Field *fields, size_t n) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->t = V_REC;
  v->u.rec.fields = fields;
  v->u.rec.n = n;
  return v;
}

/* textOf: append a value's text to out. */
static void textof_append(Buf *out, Val *v) {
  if (v == NULL) return;
  switch (v->t) {
    case V_STR:
      buf_addn(out, v->u.s.p, v->u.s.n);
      break;
    case V_NUM: {
      char nb[24];
      long long x = v->u.num;
      int neg = x < 0;
      unsigned long long ux = neg ? (unsigned long long)(-(x + 1)) + 1 : (unsigned long long)x;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (ux % 10));
        ux /= 10;
      } while (ux > 0);
      if (neg) nb[nl++] = '-';
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(out, nb, (size_t)nl);
      break;
    }
    case V_BOOL:
      buf_adds(out, v->u.b ? "true" : "false");
      break;
    case V_ARR:
      for (size_t i = 0; i < v->u.arr.n; i++) {
        if (i > 0) buf_adds(out, ", ");
        textof_append(out, v->u.arr.items[i]);
      }
      break;
    default:
      break;
  }
}

static int val_truthy(Val *v) {
  if (v == NULL) return 0;
  switch (v->t) {
    case V_ARR:
      return v->u.arr.n > 0;
    case V_BOOL:
      return v->u.b;
    case V_STR:
      return v->u.s.n > 0;
    case V_NUM:
      return v->u.num != 0;
    case V_REC:
      return 1;
    default:
      return 0;
  }
}

typedef struct Scope Scope;
struct Scope {
  Field *binds;
  size_t nbinds;
  Scope *parent;
};

/* The value at path in the scope chain, or NULL (undefined). */
static Val *lookup_val(Arena *a, Scope *scope, Str *path, size_t npath) {
  Scope *s = scope;
  Val *v = NULL;
  while (s != NULL) {
    int found = 0;
    for (size_t i = 0; i < s->nbinds; i++) {
      if (s->binds[i].name.n == path[0].n && memcmp(s->binds[i].name.p, path[0].p, path[0].n) == 0) {
        v = s->binds[i].val;
        found = 1;
        break;
      }
    }
    if (found) break;
    s = s->parent;
  }
  if (v == NULL) return NULL;
  for (size_t k = 1; k < npath; k++) {
    if (v->t == V_REC) {
      Val *nv = NULL;
      for (size_t i = 0; i < v->u.rec.n; i++) {
        if (v->u.rec.fields[i].name.n == path[k].n && memcmp(v->u.rec.fields[i].name.p, path[k].p, path[k].n) == 0) {
          nv = v->u.rec.fields[i].val;
          break;
        }
      }
      if (nv == NULL) return NULL;
      v = nv;
    } else if (v->t == V_ARR) {
      if (str_eq(path[k], "length")) {
        v = val_num(a, (long long)v->u.arr.n);
      } else {
        /* a canonical array index */
        Str seg = path[k];
        int alldig = seg.n > 0;
        for (size_t i = 0; i < seg.n; i++)
          if (seg.p[i] < '0' || seg.p[i] > '9') alldig = 0;
        if (!alldig) return NULL;
        if (seg.n > 1 && seg.p[0] == '0') return NULL;
        unsigned long long idx = 0;
        for (size_t i = 0; i < seg.n; i++) {
          idx = idx * 10 + (unsigned long long)(seg.p[i] - '0');
          if (idx > (unsigned long long)v->u.arr.n) return NULL;
        }
        if (idx >= (unsigned long long)v->u.arr.n) return NULL;
        v = v->u.arr.items[idx];
      }
    } else {
      return NULL;
    }
  }
  return v;
}

/* ------------------------------------------------------------------ templates */

typedef enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INC } NKind;

typedef struct Node Node;
typedef struct {
  Node *head, *tail;
} NodeList;

struct Node {
  NKind kind;
  Str text;
  Str *path;
  size_t npath;
  int not;
  NodeList then_b, else_b;
  int has_else;
  Str varname;
  NodeList body;
  Str inc_name;
  Node *inc_ast; /* resolved include target (NULL = none / did not parse) */
  Node *next;
};

static void nodelist_push(NodeList *l, Node *n) {
  n->next = NULL;
  if (l->tail) l->tail->next = n;
  else l->head = n;
  l->tail = n;
}

/* JSON.stringify of a string, into out (with the quotes). */
static void json_escape_str(Buf *out, Str s) {
  buf_addc(out, '"');
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = (unsigned char)s.p[i];
    if (c == '"') buf_adds(out, "\\\"");
    else if (c == '\\') buf_adds(out, "\\\\");
    else if (c == '\b') buf_adds(out, "\\b");
    else if (c == '\f') buf_adds(out, "\\f");
    else if (c == '\n') buf_adds(out, "\\n");
    else if (c == '\r') buf_adds(out, "\\r");
    else if (c == '\t') buf_adds(out, "\\t");
    else if (c < 0x20) {
      char hb[8];
      hb[0] = '\\';
      hb[1] = 'u';
      hb[2] = '0';
      hb[3] = '0';
      hb[4] = "0123456789abcdef"[(c >> 4) & 0xF];
      hb[5] = "0123456789abcdef"[c & 0xF];
      buf_addn(out, hb, 6);
    } else {
      buf_addc(out, (char)c);
    }
    i++;
  }
  buf_addc(out, '"');
}

/* Format a template error into the arena: `template "<name>": <msg>`. */
static Str tpl_err(Arena *a, Str name, const char *msg) {
  Buf b = {0};
  buf_adds(&b, "template \"");
  buf_addn(&b, name.p, name.n);
  buf_adds(&b, "\": ");
  buf_adds(&b, msg);
  char *p = arena_dup(a, b.buf ? b.buf : "", b.len);
  free(b.buf);
  return str_make(p ? p : "", b.len);
}

/* Validate a path string (PATH regex) and split it into segments. Returns 1
 * on ok (segments in the arena), 0 with err set. */
static int path_of(Arena *a, Str name, Str text, Str **segs_out, size_t *nsegs_out, Str *err) {
  /* ^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$ */
  size_t i = 0;
  int ok = text.n > 0;
  while (ok && i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
      ok = 0;
      break;
    }
    i++;
    while (i < text.n) {
      c = (unsigned char)text.p[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
      else break;
    }
    if (i < text.n) {
      if (text.p[i] != '.') {
        ok = 0;
        break;
      }
      i++;
      if (i >= text.n) {
        ok = 0;
        break;
      }
    }
  }
  if (!ok) {
    Buf b = {0};
    buf_adds(&b, "template \"");
    buf_addn(&b, name.p, name.n);
    buf_adds(&b, "\": ");
    json_escape_str(&b, text);
    buf_adds(&b, " is not a path");
    char *p = arena_dup(a, b.buf ? b.buf : "", b.len);
    free(b.buf);
    *err = str_make(p ? p : "", b.len);
    return 0;
  }
  size_t cap = 4, n = 0;
  Str *segs = (Str *)arena_alloc(a, cap * sizeof(Str));
  size_t start = 0;
  for (size_t k = 0; k <= text.n; k++) {
    if (k == text.n || text.p[k] == '.') {
      if (n == cap) {
        cap *= 2;
        Str *ns = (Str *)arena_alloc(a, cap * sizeof(Str));
        memcpy(ns, segs, n * sizeof(Str));
        segs = ns;
      }
      segs[n++] = str_make(text.p + start, k - start);
      start = k + 1;
    }
  }
  *segs_out = segs;
  *nsegs_out = n;
  return 1;
}

typedef enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG } TokKind;

typedef struct {
  TokKind kind;
  Str text; /* TK_TEXT: the text; TK_TAG: the trimmed tag */
  Str *path;
  size_t npath;
} Tok;

/* Tokenize a template. Returns 1 on ok, 0 with err set. */
static int tokenize_tpl(Arena *a, Str name, Str src, Tok **out, size_t *nout, Str *err) {
  size_t cap = 32, n = 0;
  Tok *toks = (Tok *)arena_alloc(a, cap * sizeof(Tok));
  Buf text = {0};
  size_t i = 0;
  while (i < src.n) {
    const char *found = (const char *)memchr(src.p + i, '{', src.n - i);
    size_t open = found ? (size_t)(found - src.p) : (size_t)-1;
    if (open == (size_t)-1 || open == src.n - 1) {
      buf_addn(&text, src.p + i, src.n - i);
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      buf_addn(&text, src.p + i, (open + 1) - i);
      i = open + 1;
      continue;
    }
    buf_addn(&text, src.p + i, open - i);
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      const char *needle = raw ? "}}}" : "}}";
      size_t nlen = raw ? 3 : 2;
      size_t from = open + nlen;
      const char *cf = NULL;
      if (from <= src.n) {
        /* indexOf needle from `from` */
        for (size_t k = from; k + nlen <= src.n; k++) {
          if (memcmp(src.p + k, needle, nlen) == 0) {
            cf = src.p + k;
            break;
          }
        }
      }
      if (cf == NULL) {
        free(text.buf);
        *err = tpl_err(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      size_t close = (size_t)(cf - src.p);
      if (text.len > 0) {
        if (n == cap) {
          cap *= 2;
          Tok *nt = (Tok *)arena_alloc(a, cap * sizeof(Tok));
          memcpy(nt, toks, n * sizeof(Tok));
          toks = nt;
        }
        toks[n].kind = TK_TEXT;
        toks[n].text = str_make(arena_dup(a, text.buf, text.len), text.len);
        toks[n].path = NULL;
        toks[n].npath = 0;
        n++;
        text.len = 0;
      }
      Str rawpath = js_trim(str_make(src.p + from, close - from));
      Str *segs = NULL;
      size_t nsegs = 0;
      if (!path_of(a, name, rawpath, &segs, &nsegs, err)) {
        free(text.buf);
        return 0;
      }
      if (n == cap) {
        cap *= 2;
        Tok *nt = (Tok *)arena_alloc(a, cap * sizeof(Tok));
        memcpy(nt, toks, n * sizeof(Tok));
        toks = nt;
      }
      toks[n].kind = raw ? TK_RAW : TK_VAR;
      toks[n].text = str_make("", 0);
      toks[n].path = segs;
      toks[n].npath = nsegs;
      n++;
      i = close + nlen;
      continue;
    }
    /* {% %} */
    const char *cf = NULL;
    for (size_t k = open + 2; k + 2 <= src.n; k++) {
      if (src.p[k] == '%' && src.p[k + 1] == '}') {
        cf = src.p + k;
        break;
      }
    }
    if (cf == NULL) {
      free(text.buf);
      *err = tpl_err(a, name, "a {% is not closed");
      return 0;
    }
    size_t close = (size_t)(cf - src.p);
    size_t end = close + 2;
    size_t line_start;
    if (open == 0) {
      line_start = 0;
    } else {
      long ls = -1;
      for (size_t k = open; k-- > 0;) {
        if (src.p[k] == '\n') {
          ls = (long)k;
          break;
        }
      }
      line_start = ls == -1 ? 0 : (size_t)ls + 1;
    }
    const char *nlf = (const char *)memchr(src.p + end, '\n', src.n - end);
    long nl = nlf ? (long)(nlf - src.p) : -1;
    size_t line_end = nl == -1 ? src.n : (size_t)nl;
    int before_ok = 1;
    for (size_t k = line_start; k < open; k++)
      if (src.p[k] != ' ' && src.p[k] != '\t') before_ok = 0;
    int after_ok = 1;
    for (size_t k = end; k < line_end; k++)
      if (src.p[k] != ' ' && src.p[k] != '\t') after_ok = 0;
    int standalone = before_ok && after_ok;
    if (standalone) {
      size_t strip = open - line_start;
      if (strip <= text.len) text.len -= strip;
      else text.len = 0;
    }
    if (text.len > 0) {
      if (n == cap) {
        cap *= 2;
        Tok *nt = (Tok *)arena_alloc(a, cap * sizeof(Tok));
        memcpy(nt, toks, n * sizeof(Tok));
        toks = nt;
      }
      toks[n].kind = TK_TEXT;
      toks[n].text = str_make(arena_dup(a, text.buf, text.len), text.len);
      toks[n].path = NULL;
      toks[n].npath = 0;
      n++;
      text.len = 0;
    }
    Str tag = js_trim(str_make(src.p + open + 2, close - (open + 2)));
    if (n == cap) {
      cap *= 2;
      Tok *nt = (Tok *)arena_alloc(a, cap * sizeof(Tok));
      memcpy(nt, toks, n * sizeof(Tok));
      toks = nt;
    }
    toks[n].kind = TK_TAG;
    toks[n].text = tag;
    toks[n].path = NULL;
    toks[n].npath = 0;
    n++;
    i = standalone ? (nl == -1 ? src.n : (size_t)nl + 1) : end;
  }
  if (text.len > 0) {
    if (n == cap) {
      cap *= 2;
      Tok *nt = (Tok *)arena_alloc(a, cap * sizeof(Tok));
      memcpy(nt, toks, n * sizeof(Tok));
      toks = nt;
    }
    toks[n].kind = TK_TEXT;
    toks[n].text = str_make(arena_dup(a, text.buf, text.len), text.len);
    toks[n].path = NULL;
    toks[n].npath = 0;
    n++;
  }
  free(text.buf);
  *out = toks;
  *nout = n;
  return 1;
}

/* Parse a FOR tag: "for <name> in <path>". Returns 1 and sets name/path. */
static int parse_for_tag(Str tag, Str *vname, Str *vpath) {
  if (tag.n < 4 || memcmp(tag.p, "for ", 4) != 0) return 0;
  size_t i = 4;
  if (i >= tag.n) return 0;
  unsigned char c = (unsigned char)tag.p[i];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
  size_t ns = i;
  i++;
  while (i < tag.n) {
    c = (unsigned char)tag.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
    else break;
  }
  if (i + 4 > tag.n || memcmp(tag.p + i, " in ", 4) != 0) return 0;
  size_t ps = i + 4;
  if (ps >= tag.n) return 0;
  for (size_t k = ps; k < tag.n; k++) {
    unsigned char cc = (unsigned char)tag.p[k];
    if (cc == ' ' || cc == '\t' || cc == '\n' || cc == '\v' || cc == '\f' || cc == '\r') return 0;
  }
  *vname = str_make(tag.p + ns, i - ns);
  *vpath = str_make(tag.p + ps, tag.n - ps);
  return 1;
}

/* Parse an IF tag: "if [not ]<path>". Returns 1 and sets not/path. */
static int parse_if_tag(Str tag, int *not, Str *vpath) {
  if (tag.n < 3 || memcmp(tag.p, "if ", 3) != 0) return 0;
  size_t i = 3;
  *not = 0;
  if (i + 4 <= tag.n && memcmp(tag.p + i, "not ", 4) == 0) {
    *not = 1;
    i += 4;
  }
  if (i >= tag.n) return 0;
  for (size_t k = i; k < tag.n; k++) {
    unsigned char cc = (unsigned char)tag.p[k];
    if (cc == ' ' || cc == '\t' || cc == '\n' || cc == '\v' || cc == '\f' || cc == '\r') return 0;
  }
  *vpath = str_make(tag.p + i, tag.n - i);
  return 1;
}

/* Parse an INCLUDE tag: include "<name>". Returns 1 and sets name. */
static int parse_include_tag(Str tag, Str *vname) {
  if (tag.n < 11 || memcmp(tag.p, "include \"", 9) != 0) return 0;
  if (tag.p[tag.n - 1] != '"') return 0;
  Str inner = str_make(tag.p + 9, tag.n - 10);
  if (inner.n == 0) return 0;
  for (size_t i = 0; i < inner.n; i++) {
    char c = inner.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
  }
  *vname = inner;
  return 1;
}

/* Parse tokens into a node list. Returns 1 on ok, 0 with err set. */
static int parse_tpl(Arena *a, Str name, Tok *toks, size_t ntoks, Node **root_out, Str *err) {
  NodeList root = {0};
  typedef struct {
    Node *node; /* NULL for root */
    NodeList *list;
  } Frame;
  Frame stack[64];
  int sp = 0;
  stack[sp].node = NULL;
  stack[sp].list = &root;
  sp++;
  for (size_t ti = 0; ti < ntoks; ti++) {
    Tok *tok = &toks[ti];
    if (tok->kind == TK_TEXT) {
      Node *nd = (Node *)arena_alloc(a, sizeof(Node));
      memset(nd, 0, sizeof(*nd));
      nd->kind = N_TEXT;
      nd->text = tok->text;
      nodelist_push(stack[sp - 1].list, nd);
    } else if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      Node *nd = (Node *)arena_alloc(a, sizeof(Node));
      memset(nd, 0, sizeof(*nd));
      nd->kind = tok->kind == TK_VAR ? N_VAR : N_RAW;
      nd->path = tok->path;
      nd->npath = tok->npath;
      nodelist_push(stack[sp - 1].list, nd);
    } else {
      Str tag = tok->text;
      Str vn, vp;
      int nt;
      Str ip;
      if (parse_for_tag(tag, &vn, &vp)) {
        Str *segs = NULL;
        size_t nsegs = 0;
        if (!path_of(a, name, vp, &segs, &nsegs, err)) return 0;
        Node *nd = (Node *)arena_alloc(a, sizeof(Node));
        memset(nd, 0, sizeof(*nd));
        nd->kind = N_FOR;
        nd->varname = vn;
        nd->path = segs;
        nd->npath = nsegs;
        nodelist_push(stack[sp - 1].list, nd);
        if (sp >= 64) {
          *err = tpl_err(a, name, "a for is not closed");
          return 0;
        }
        stack[sp].node = nd;
        stack[sp].list = &nd->body;
        sp++;
      } else if (parse_if_tag(tag, &nt, &ip)) {
        Str *segs = NULL;
        size_t nsegs = 0;
        if (!path_of(a, name, ip, &segs, &nsegs, err)) return 0;
        Node *nd = (Node *)arena_alloc(a, sizeof(Node));
        memset(nd, 0, sizeof(*nd));
        nd->kind = N_IF;
        nd->not = nt;
        nd->path = segs;
        nd->npath = nsegs;
        nodelist_push(stack[sp - 1].list, nd);
        if (sp >= 64) {
          *err = tpl_err(a, name, "an if is not closed");
          return 0;
        }
        stack[sp].node = nd;
        stack[sp].list = &nd->then_b;
        sp++;
      } else if (str_eq(tag, "else")) {
        Node *top = stack[sp - 1].node;
        if (top == NULL || top->kind != N_IF || top->has_else) {
          *err = tpl_err(a, name, "an else outside an if");
          return 0;
        }
        top->has_else = 1;
        stack[sp - 1].list = &top->else_b;
      } else if (str_eq(tag, "endfor") || str_eq(tag, "endif")) {
        Node *top = stack[sp - 1].node;
        NKind want = str_eq(tag, "endfor") ? N_FOR : N_IF;
        if (top == NULL || top->kind != want) {
          *err = tpl_err(a, name, str_eq(tag, "endfor") ? "an endfor with no open for" : "an endif with no open if");
          return 0;
        }
        sp--;
      } else if (parse_include_tag(tag, &vn)) {
        Node *nd = (Node *)arena_alloc(a, sizeof(Node));
        memset(nd, 0, sizeof(*nd));
        nd->kind = N_INC;
        nd->inc_name = vn;
        nodelist_push(stack[sp - 1].list, nd);
      } else {
        Buf b = {0};
        buf_adds(&b, "{% ");
        buf_addn(&b, tag.p, tag.n);
        buf_adds(&b, " %} is not a tag");
        char *p = arena_dup(a, b.buf ? b.buf : "", b.len);
        free(b.buf);
        *err = tpl_err(a, name, p);
        return 0;
      }
    }
  }
  if (sp > 1) {
    Node *top = stack[sp - 1].node;
    *err = tpl_err(a, name, top->kind == N_FOR ? "a for is not closed" : "an if is not closed");
    return 0;
  }
  *root_out = root.head;
  return 1;
}

/* ------------------------------------------------------------------ template render */

typedef struct {
  Str name;
  Node *ast; /* NULL when the template did not parse */
} ParsedTpl;

typedef struct {
  ParsedTpl *tpls;
  size_t ntpls;
  Arena *arena;
} TplCtx;

/* Format a long long's decimal digits directly into out (no temp Buf). */
static void buf_add_ll(Buf *out, long long x) {
  char nb[24];
  int neg = x < 0;
  unsigned long long ux = neg ? (unsigned long long)(-(x + 1)) + 1 : (unsigned long long)x;
  int nl = 0;
  if (ux == 0) nb[nl++] = '0';
  else {
    while (ux > 0) {
      nb[nl++] = (char)('0' + (ux % 10));
      ux /= 10;
    }
  }
  if (neg) buf_addc(out, '-');
  while (nl > 0) buf_addc(out, nb[--nl]);
}

static void render_nodes(Buf *out, Node *nodes, Scope *scope, TplCtx *ctx, int depth) {
  Arena *a = ctx->arena;
  for (Node *n = nodes; n != NULL; n = n->next) {
    switch (n->kind) {
      case N_TEXT:
        buf_addn(out, n->text.p, n->text.n);
        break;
      case N_VAR: {
        Val *v = lookup_val(a, scope, n->path, n->npath);
        if (v == NULL || v->t == V_NULL || v->t == V_REC) break;
        if (v->t == V_STR) {
          esc_append(out, v->u.s);
          break;
        }
        if (v->t == V_BOOL) {
          buf_adds(out, v->u.b ? "true" : "false");
          break;
        }
        if (v->t == V_NUM) {
          /* A number's digits hold no HTML special, so no escape pass. */
          buf_add_ll(out, v->u.num);
          break;
        }
        /* An array's text joins its items with ", ": the rare path. */
        Buf tb = {0};
        textof_append(&tb, v);
        esc_append(out, str_make(tb.buf ? tb.buf : "", tb.len));
        free(tb.buf);
        break;
      }
      case N_RAW: {
        Val *v = lookup_val(a, scope, n->path, n->npath);
        textof_append(out, v);
        break;
      }
      case N_IF: {
        Val *v = lookup_val(a, scope, n->path, n->npath);
        int tv = val_truthy(v);
        NodeList *br = (tv != n->not) ? &n->then_b : (n->has_else ? &n->else_b : NULL);
        if (br != NULL) render_nodes(out, br->head, scope, ctx, depth);
        break;
      }
      case N_FOR: {
        Val *xs = lookup_val(a, scope, n->path, n->npath);
        if (xs == NULL || xs->t != V_ARR) break;
        if (xs->u.arr.n == 0) break;
        /* The loop record and its binds are built ONCE per loop, not per
         * iteration: the per-iteration values (index/first/last/element) are
         * mutated in place, since nothing outside this loop reads them. */
        Field *lf = (Field *)arena_alloc(a, 4 * sizeof(Field));
        Val *vi = val_num(a, 0);
        Val *vf = val_bool(a, 0);
        Val *vl = val_bool(a, 0);
        Val *vn = val_num(a, (long long)xs->u.arr.n);
        lf[0].name = str_lit("index");
        lf[0].val = vi;
        lf[1].name = str_lit("first");
        lf[1].val = vf;
        lf[2].name = str_lit("last");
        lf[2].val = vl;
        lf[3].name = str_lit("length");
        lf[3].val = vn;
        Val *loop = val_rec(a, lf, 4);
        Field *binds = (Field *)arena_alloc(a, 2 * sizeof(Field));
        binds[0].name = n->varname;
        binds[0].val = NULL;
        binds[1].name = str_lit("loop");
        binds[1].val = loop;
        Scope fr = {binds, 2, scope};
        for (size_t k = 0; k < xs->u.arr.n; k++) {
          vi->u.num = (long long)k + 1;
          vf->u.b = (k == 0) ? 1 : 0;
          vl->u.b = (k == xs->u.arr.n - 1) ? 1 : 0;
          binds[0].val = xs->u.arr.items[k];
          render_nodes(out, n->body.head, &fr, ctx, depth);
        }
        break;
      }
      case N_INC: {
        if (depth >= 8) break;
        if (n->inc_ast != NULL) render_nodes(out, n->inc_ast, scope, ctx, depth + 1);
        break;
      }
    }
  }
}

/* ------------------------------------------------------------------ cases from JSON */

typedef struct {
  Str title, base_url;
  long long per_page, recent;
} SiteInfo;

typedef struct {
  Str path, content;
} FileEnt;

typedef struct {
  Str name, src;
  Node *ast;
  Str err;
  int ok;
} Tpl;

typedef struct {
  Str id;
  SiteInfo site;
  Tpl *tpls;
  size_t ntpls;
  FileEnt *files;
  size_t nfiles;
} Case;

/* Parse a cases.json array into Cases. Returns 1 on ok. */
static int parse_cases(Arena *a, const char *json, size_t len, Case **out_cases, size_t *out_n) {
  JParser j = {json, json + len, a};
  if (!jp_expect(&j, '[')) return 0;
  size_t cap = 4, n = 0;
  Case *cases = (Case *)arena_alloc(a, cap * sizeof(Case));
  if (jp_expect(&j, ']')) {
    *out_cases = cases;
    *out_n = 0;
    return 1;
  }
  for (;;) {
    if (n == cap) {
      cap *= 2;
      Case *nc = (Case *)arena_alloc(a, cap * sizeof(Case));
      memcpy(nc, cases, n * sizeof(Case));
      cases = nc;
    }
    Case *c = &cases[n];
    memset(c, 0, sizeof(*c));
    if (!jp_expect(&j, '{')) return 0;
    if (jp_expect(&j, '}')) {
      n++;
    } else {
      for (;;) {
        Str key;
        if (!jp_string(&j, &key)) return 0;
        if (!jp_expect(&j, ':')) return 0;
        if (str_eq(key, "id")) {
          if (!jp_string(&j, &c->id)) return 0;
        } else if (str_eq(key, "input")) {
          if (!jp_expect(&j, '{')) return 0;
          if (jp_expect(&j, '}')) {
            /* empty input */
          } else {
            for (;;) {
              Str ik;
              if (!jp_string(&j, &ik)) return 0;
              if (!jp_expect(&j, ':')) return 0;
              if (str_eq(ik, "site")) {
                if (!jp_expect(&j, '{')) return 0;
                if (jp_expect(&j, '}')) {
                } else {
                  for (;;) {
                    Str sk;
                    if (!jp_string(&j, &sk)) return 0;
                    if (!jp_expect(&j, ':')) return 0;
                    if (str_eq(sk, "title")) {
                      if (!jp_string(&j, &c->site.title)) return 0;
                    } else if (str_eq(sk, "base_url")) {
                      if (!jp_string(&j, &c->site.base_url)) return 0;
                    } else if (str_eq(sk, "per_page")) {
                      Str raw;
                      long long iv;
                      int ii;
                      if (!jp_number(&j, &raw, &iv, &ii)) return 0;
                      c->site.per_page = iv;
                    } else if (str_eq(sk, "recent")) {
                      Str raw;
                      long long iv;
                      int ii;
                      if (!jp_number(&j, &raw, &iv, &ii)) return 0;
                      c->site.recent = iv;
                    } else {
                      if (!jp_skip(&j)) return 0;
                    }
                    if (jp_expect(&j, ',')) continue;
                    if (jp_expect(&j, '}')) break;
                    return 0;
                  }
                }
              } else if (str_eq(ik, "templates")) {
                if (!jp_expect(&j, '{')) return 0;
                size_t tcap = 8, tn = 0;
                Tpl *tpls = (Tpl *)arena_alloc(a, tcap * sizeof(Tpl));
                if (jp_expect(&j, '}')) {
                } else {
                  for (;;) {
                    Str tk;
                    if (!jp_string(&j, &tk)) return 0;
                    if (!jp_expect(&j, ':')) return 0;
                    Str tsrc;
                    if (!jp_string(&j, &tsrc)) return 0;
                    if (tn == tcap) {
                      tcap *= 2;
                      Tpl *nt = (Tpl *)arena_alloc(a, tcap * sizeof(Tpl));
                      memcpy(nt, tpls, tn * sizeof(Tpl));
                      tpls = nt;
                    }
                    memset(&tpls[tn], 0, sizeof(Tpl));
                    tpls[tn].name = tk;
                    tpls[tn].src = tsrc;
                    tn++;
                    if (jp_expect(&j, ',')) continue;
                    if (jp_expect(&j, '}')) break;
                    return 0;
                  }
                }
                c->tpls = tpls;
                c->ntpls = tn;
              } else if (str_eq(ik, "files")) {
                if (!jp_expect(&j, '[')) return 0;
                size_t fcap = 64, fn = 0;
                FileEnt *files = (FileEnt *)arena_alloc(a, fcap * sizeof(FileEnt));
                if (jp_expect(&j, ']')) {
                } else {
                  for (;;) {
                    if (!jp_expect(&j, '{')) return 0;
                    FileEnt fe = {str_make("", 0), str_make("", 0)};
                    if (jp_expect(&j, '}')) {
                    } else {
                      for (;;) {
                        Str fk;
                        if (!jp_string(&j, &fk)) return 0;
                        if (!jp_expect(&j, ':')) return 0;
                        if (str_eq(fk, "path")) {
                          if (!jp_string(&j, &fe.path)) return 0;
                        } else if (str_eq(fk, "content")) {
                          if (!jp_string(&j, &fe.content)) return 0;
                        } else {
                          if (!jp_skip(&j)) return 0;
                        }
                        if (jp_expect(&j, ',')) continue;
                        if (jp_expect(&j, '}')) break;
                        return 0;
                      }
                    }
                    if (fn == fcap) {
                      fcap *= 2;
                      FileEnt *nf = (FileEnt *)arena_alloc(a, fcap * sizeof(FileEnt));
                      memcpy(nf, files, fn * sizeof(FileEnt));
                      files = nf;
                    }
                    files[fn++] = fe;
                    if (jp_expect(&j, ',')) continue;
                    if (jp_expect(&j, ']')) break;
                    return 0;
                  }
                }
                c->files = files;
                c->nfiles = fn;
              } else {
                if (!jp_skip(&j)) return 0;
              }
              if (jp_expect(&j, ',')) continue;
              if (jp_expect(&j, '}')) break;
              return 0;
            }
          }
        } else {
          if (!jp_skip(&j)) return 0;
        }
        if (jp_expect(&j, ',')) continue;
        if (jp_expect(&j, '}')) break;
        return 0;
      }
      n++;
    }
    if (jp_expect(&j, ',')) continue;
    if (jp_expect(&j, ']')) break;
    return 0;
  }
  *out_cases = cases;
  *out_n = n;
  return 1;
}

/* ------------------------------------------------------------------ build */

typedef struct Src Src;
typedef struct Tag Tag;

struct Tag {
  Str name, slug, url;
  Src **posts;
  size_t nposts, cap;
};

struct Src {
  Str path, text;
  FmData fm;
  Str body;
  int post;
  Str layout;
  Str slug, outdir, url;
  Str html;
  TocEnt *toc;
  size_t ntoc;
  Str excerpt;
  Src *newer, *older;
  Tag **tagrefs;
  size_t ntagrefs;
  Val *card;
  int kept;
};

typedef struct {
  Str path, content;
} OutEnt;

typedef struct {
  Arena *arena;
  Case *c;
  Str base;
  ParsedTpl *tpls;
  size_t ntpls;
  Src *srcs;
  size_t nsrcs;
  FileEnt *statics;
  size_t nstatics;
  Str *report;
  size_t nreport, report_cap;
  Str *broken;
  size_t nbroken, broken_cap;
  Src **posts;
  size_t nposts;
  Tag **tags; /* array of pointers: the Tag structs are never moved by the sort */
  size_t ntags;
  OutEnt *out;
  size_t nout, out_cap;
  Buf linkbuf;
  Buf render_buf; /* reused across every render_tpl call: stays at page size,
                     so no malloc/realloc churn per page */
  Buf scratch_buf; /* reusable scratch for render_md's per-block tmp */
} Build;

static void build_report_push(Build *b, Str line) {
  if (b->nreport == b->report_cap) {
    size_t nc = b->report_cap ? b->report_cap * 2 : 16;
    Str *nr = (Str *)arena_alloc(b->arena, nc * sizeof(Str));
    if (b->report) memcpy(nr, b->report, b->nreport * sizeof(Str));
    b->report = nr;
    b->report_cap = nc;
  }
  b->report[b->nreport++] = line;
}

static void build_broken_push(Build *b, Str line) {
  for (size_t i = 0; i < b->nbroken; i++)
    if (b->broken[i].n == line.n && memcmp(b->broken[i].p, line.p, line.n) == 0) return;
  if (b->nbroken == b->broken_cap) {
    size_t nc = b->broken_cap ? b->broken_cap * 2 : 16;
    Str *nr = (Str *)arena_alloc(b->arena, nc * sizeof(Str));
    if (b->broken) memcpy(nr, b->broken, b->nbroken * sizeof(Str));
    b->broken = nr;
    b->broken_cap = nc;
  }
  b->broken[b->nbroken++] = line;
}

static void build_out_push(Build *b, Str path, Str content) {
  if (b->nout == b->out_cap) {
    size_t nc = b->out_cap ? b->out_cap * 2 : 64;
    OutEnt *no = (OutEnt *)arena_alloc(b->arena, nc * sizeof(OutEnt));
    if (b->out) memcpy(no, b->out, b->nout * sizeof(OutEnt));
    b->out = no;
    b->out_cap = nc;
  }
  b->out[b->nout].path = path;
  b->out[b->nout].content = content;
  b->nout++;
}

static int build_out_has(Build *b, Str path) {
  for (size_t i = 0; i < b->nout; i++)
    if (b->out[i].path.n == path.n && memcmp(b->out[i].path.p, path.p, path.n) == 0) return 1;
  return 0;
}

static const char *dir_of_str(Str path, size_t *dir_len) {
  const char *slash = NULL;
  for (size_t i = 0; i < path.n; i++)
    if (path.p[i] == '/') slash = path.p + i;
  if (slash == NULL) {
    *dir_len = 0;
    return path.p;
  }
  *dir_len = (size_t)(slash - path.p);
  return path.p;
}

/* Normalize \r\n? -> \n. Returns the input unchanged when it holds no '\r'. */
static Str normalize_crlf(Arena *a, Str s) {
  int has_cr = 0;
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] == '\r') {
      has_cr = 1;
      break;
    }
  if (!has_cr) return s;
  Buf b = {0};
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] == '\r') {
      buf_addc(&b, '\n');
      if (i + 1 < s.n && s.p[i + 1] == '\n') i++;
    } else {
      buf_addc(&b, s.p[i]);
    }
  }
  Str out = str_make(arena_dup(a, b.buf ? b.buf : "", b.len), b.len);
  free(b.buf);
  return out;
}

static int cmp_strp(const void *x, const void *y) {
  const Str *a = (const Str *)x, *b = (const Str *)y;
  return str_cmp(*a, *b);
}

static int cmp_src_date(const void *x, const void *y) {
  Src *a = *(Src *const *)x, *b = *(Src *const *)y;
  int c = str_cmp(b->fm.date, a->fm.date); /* date descending */
  if (c != 0) return c;
  return str_cmp(a->slug, b->slug);
}

static int cmp_tag(const void *x, const void *y) {
  const Tag *a = *(Tag *const *)x, *b = *(Tag *const *)y;
  if (a->nposts != b->nposts) return a->nposts < b->nposts ? 1 : -1;
  return str_cmp(a->slug, b->slug);
}

/* bySource: binary search over the kept sources (sorted by path). */
static Src *by_source(Build *b, Str path) {
  size_t lo = 0, hi = b->nsrcs;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = str_cmp(b->srcs[mid].path, path);
    if (c == 0) return &b->srcs[mid];
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}

static int static_has(Build *b, Str path) {
  for (size_t i = 0; i < b->nstatics; i++)
    if (b->statics[i].path.n == path.n && memcmp(b->statics[i].path.p, path.p, path.n) == 0) return 1;
  return 0;
}

/* resolvePath(dir, rel): NULL (0) when it climbs out of the root. */
static int resolve_path(Arena *a, Str dir, Str rel, Str *out) {
  size_t cap = 8, n = 0;
  Str *parts = (Str *)arena_alloc(a, cap * sizeof(Str));
  size_t start = 0;
  if (dir.n > 0) {
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (n == cap) {
          cap *= 2;
          Str *np = (Str *)arena_alloc(a, cap * sizeof(Str));
          memcpy(np, parts, n * sizeof(Str));
          parts = np;
        }
        parts[n++] = str_make(dir.p + start, i - start);
        start = i + 1;
      }
    }
  }
  start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Str seg = str_make(rel.p + start, i - start);
      start = i + 1;
      if (seg.n == 0 || str_eq(seg, ".")) continue;
      if (str_eq(seg, "..")) {
        if (n == 0) return 0;
        n--;
        continue;
      }
      if (n == cap) {
        cap *= 2;
        Str *np = (Str *)arena_alloc(a, cap * sizeof(Str));
        memcpy(np, parts, n * sizeof(Str));
        parts = np;
      }
      parts[n++] = seg;
    }
  }
  Buf b = {0};
  for (size_t i = 0; i < n; i++) {
    if (i > 0) buf_addc(&b, '/');
    buf_addn(&b, parts[i].p, parts[i].n);
  }
  *out = str_make(arena_dup(a, b.buf ? b.buf : "", b.len), b.len);
  free(b.buf);
  return 1;
}

typedef struct {
  Build *b;
  Src *src;
  Str dir;
} LinkCtx;

static int is_scheme(Str t) {
  if (t.n < 2) return 0;
  unsigned char c0 = (unsigned char)t.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
  size_t i = 1;
  while (i < t.n) {
    unsigned char c = (unsigned char)t.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') i++;
    else break;
  }
  return i < t.n && t.p[i] == ':';
}

static Str md_link(void *vctx, Str target) {
  LinkCtx *lc = (LinkCtx *)vctx;
  Build *b = lc->b;
  if (is_scheme(target) || (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#'))) return target;
  long hash = -1;
  for (size_t i = 0; i < target.n; i++)
    if (target.p[i] == '#') {
      hash = (long)i;
      break;
    }
  Str rel = hash == -1 ? target : str_make(target.p, (size_t)hash);
  Str frag = hash == -1 ? str_make("", 0) : str_make(target.p + hash, target.n - (size_t)hash);
  Str path;
  int have_path = rel.n > 0 && resolve_path(b->arena, lc->dir, rel, &path);
  if (have_path) {
    Src *s = by_source(b, path);
    if (s != NULL) {
      Buf *lb = &b->linkbuf;
      lb->len = 0;
      buf_addn(lb, s->url.p, s->url.n);
      buf_addn(lb, frag.p, frag.n);
      return str_make(arena_dup(b->arena, lb->buf ? lb->buf : "", lb->len), lb->len);
    }
    if (static_has(b, path)) {
      Buf *lb = &b->linkbuf;
      lb->len = 0;
      buf_addn(lb, b->base.p, b->base.n);
      buf_addc(lb, '/');
      buf_addn(lb, path.p, path.n);
      buf_addn(lb, frag.p, frag.n);
      return str_make(arena_dup(b->arena, lb->buf ? lb->buf : "", lb->len), lb->len);
    }
  }
  Buf lb2 = {0};
  buf_adds(&lb2, "broken-link ");
  buf_addn(&lb2, lc->src->path.p, lc->src->path.n);
  buf_adds(&lb2, " -> ");
  buf_addn(&lb2, target.p, target.n);
  Str line = str_make(arena_dup(b->arena, lb2.buf ? lb2.buf : "", lb2.len), lb2.len);
  free(lb2.buf);
  build_broken_push(b, line);
  return target;
}

/* A page's link record as templates read it. */
static Val *link_of(Arena *a, Src *s) {
  if (s == NULL) return &val_null_singleton;
  Field *f = (Field *)arena_alloc(a, 4 * sizeof(Field));
  f[0].name = str_lit("title");
  f[0].val = val_str(a, s->fm.title);
  f[1].name = str_lit("url");
  f[1].val = val_str(a, s->url);
  f[2].name = str_lit("date");
  f[2].val = val_str(a, s->fm.has_date ? s->fm.date : str_make("", 0));
  Buf db = {0};
  if (s->fm.has_date) long_date_buf(s->fm.date, &db);
  f[3].name = str_lit("date_long");
  f[3].val = val_str(a, str_make(arena_dup(a, db.buf ? db.buf : "", db.len), db.len));
  free(db.buf);
  return val_rec(a, f, 4);
}

/* A post's card, as lists show it. */
static Val *card_of(Build *b, Src *s) {
  if (s->card != NULL) return s->card;
  Arena *a = b->arena;
  Field *f = (Field *)arena_alloc(a, 8 * sizeof(Field));
  f[0].name = str_lit("title");
  f[0].val = val_str(a, s->fm.title);
  f[1].name = str_lit("url");
  f[1].val = val_str(a, s->url);
  f[2].name = str_lit("date");
  f[2].val = val_str(a, s->fm.has_date ? s->fm.date : str_make("", 0));
  Buf db = {0};
  if (s->fm.has_date) long_date_buf(s->fm.date, &db);
  f[3].name = str_lit("date_long");
  f[3].val = val_str(a, str_make(arena_dup(a, db.buf ? db.buf : "", db.len), db.len));
  free(db.buf);
  f[4].name = str_lit("summary");
  f[4].val = val_str(a, s->fm.has_summary ? s->fm.summary : str_make("", 0));
  f[5].name = str_lit("excerpt");
  f[5].val = val_str(a, s->excerpt);
  long long w = words_of(s->body);
  long long rm = (w + 199) / 200;
  if (rm < 1) rm = 1;
  f[6].name = str_lit("reading_minutes");
  f[6].val = val_num(a, rm);
  Val **titems = (Val **)arena_alloc(a, (s->ntagrefs > 0 ? s->ntagrefs : 1) * sizeof(Val *));
  for (size_t i = 0; i < s->ntagrefs; i++) {
    Tag *t = s->tagrefs[i];
    Field *tf = (Field *)arena_alloc(a, 3 * sizeof(Field));
    tf[0].name = str_lit("name");
    tf[0].val = val_str(a, t->name);
    tf[1].name = str_lit("slug");
    tf[1].val = val_str(a, t->slug);
    tf[2].name = str_lit("url");
    tf[2].val = val_str(a, t->url);
    titems[i] = val_rec(a, tf, 3);
  }
  f[7].name = str_lit("tags");
  f[7].val = val_arr(a, titems, s->ntagrefs);
  s->card = val_rec(a, f, 8);
  return s->card;
}

/* A page as its layout reads it. */
static Val *page_of(Build *b, Src *s) {
  Arena *a = b->arena;
  Val *card = card_of(b, s);
  Field *f = (Field *)arena_alloc(a, 14 * sizeof(Field));
  for (size_t i = 0; i < card->u.rec.n; i++) f[i] = card->u.rec.fields[i];
  size_t n = card->u.rec.n;
  f[n].name = str_lit("slug");
  f[n].val = val_str(a, s->slug);
  n++;
  f[n].name = str_lit("content");
  f[n].val = val_str(a, s->html);
  n++;
  Val **titems = (Val **)arena_alloc(a, (s->ntoc > 0 ? s->ntoc : 1) * sizeof(Val *));
  for (size_t i = 0; i < s->ntoc; i++) {
    Field *hf = (Field *)arena_alloc(a, 3 * sizeof(Field));
    hf[0].name = str_lit("level");
    hf[0].val = val_num(a, s->toc[i].level);
    hf[1].name = str_lit("id");
    hf[1].val = val_str(a, s->toc[i].id);
    hf[2].name = str_lit("html");
    hf[2].val = val_str(a, s->toc[i].html);
    titems[i] = val_rec(a, hf, 3);
  }
  f[n].name = str_lit("toc");
  f[n].val = val_arr(a, titems, s->ntoc);
  n++;
  f[n].name = str_lit("newer");
  f[n].val = link_of(a, s->newer);
  n++;
  f[n].name = str_lit("older");
  f[n].val = link_of(a, s->older);
  n++;
  f[n].name = str_lit("is_post");
  f[n].val = val_bool(a, s->post);
  n++;
  return val_rec(a, f, n);
}

/* A tag as templates read it (its posts as cards). */
static Val *tag_of(Build *b, Tag *t) {
  Arena *a = b->arena;
  Val **pitems = (Val **)arena_alloc(a, (t->nposts > 0 ? t->nposts : 1) * sizeof(Val *));
  for (size_t i = 0; i < t->nposts; i++) pitems[i] = t->posts[i]->card;
  Field *f = (Field *)arena_alloc(a, 5 * sizeof(Field));
  f[0].name = str_lit("name");
  f[0].val = val_str(a, t->name);
  f[1].name = str_lit("slug");
  f[1].val = val_str(a, t->slug);
  f[2].name = str_lit("url");
  f[2].val = val_str(a, t->url);
  f[3].name = str_lit("count");
  f[3].val = val_num(a, (long long)t->nposts);
  f[4].name = str_lit("posts");
  f[4].val = val_arr(a, pitems, t->nposts);
  return val_rec(a, f, 5);
}

/* Render a template by name. The top-level scope is (site, tags, recent) plus
 * the extra vars. Returns 1 and sets html (arena) when the template exists
 * and parsed, else 0. */
/* Resolve every {% include %} in an AST to its target's root node, once per
 * case, so rendering never scans the template table per include. */
static void resolve_includes_list(Node *nodes, TplCtx *ctx) {
  for (Node *n = nodes; n != NULL; n = n->next) {
    if (n->kind == N_INC) {
      n->inc_ast = NULL;
      for (size_t i = 0; i < ctx->ntpls; i++) {
        if (ctx->tpls[i].name.n == n->inc_name.n && memcmp(ctx->tpls[i].name.p, n->inc_name.p, n->inc_name.n) == 0) {
          n->inc_ast = ctx->tpls[i].ast;
          break;
        }
      }
    } else if (n->kind == N_IF) {
      resolve_includes_list(n->then_b.head, ctx);
      resolve_includes_list(n->else_b.head, ctx);
    } else if (n->kind == N_FOR) {
      resolve_includes_list(n->body.head, ctx);
    }
  }
}

static int render_tpl(Build *b, TplCtx *ctx, Val *site, Val *allTags, Val *recent, Str name, Field *extra, size_t nextra, Str *html_out) {
  Arena *a = b->arena;
  ParsedTpl *found = NULL;
  for (size_t i = 0; i < ctx->ntpls; i++) {
    if (ctx->tpls[i].name.n == name.n && memcmp(ctx->tpls[i].name.p, name.p, name.n) == 0) {
      found = &ctx->tpls[i];
      break;
    }
  }
  if (found == NULL || found->ast == NULL) return 0;
  Field *binds = (Field *)arena_alloc(a, (3 + nextra) * sizeof(Field));
  binds[0].name = str_lit("site");
  binds[0].val = site;
  binds[1].name = str_lit("tags");
  binds[1].val = allTags;
  binds[2].name = str_lit("recent");
  binds[2].val = recent;
  for (size_t i = 0; i < nextra; i++) binds[3 + i] = extra[i];
  Scope top = {binds, 3 + nextra, NULL};
  Buf *out = &b->render_buf;
  out->len = 0;
  render_nodes(out, found->ast, &top, ctx, 0);
  *html_out = str_make(arena_dup(a, out->buf ? out->buf : "", out->len), out->len);
  return 1;
}

/* Build one case into b->out (the output tree, in insertion order). */
static void build_case(Build *b) {
  Arena *a = b->arena;
  Case *c = b->c;
  /* base = base_url minus trailing slashes */
  {
    Str bu = c->site.base_url;
    while (bu.n > 0 && bu.p[bu.n - 1] == '/') bu.n--;
    b->base = bu;
  }
  /* Parse the templates, sorted by name; a failed one is reported. */
  {
    /* sort the templates by name (insertion sort: few) */
    for (size_t i = 1; i < c->ntpls; i++) {
      Tpl t = c->tpls[i];
      size_t j = i;
      while (j > 0 && str_cmp(c->tpls[j - 1].name, t.name) > 0) {
        c->tpls[j] = c->tpls[j - 1];
        j--;
      }
      c->tpls[j] = t;
    }
    b->tpls = (ParsedTpl *)arena_alloc(a, (c->ntpls > 0 ? c->ntpls : 1) * sizeof(ParsedTpl));
    b->ntpls = c->ntpls;
    for (size_t i = 0; i < c->ntpls; i++) {
      b->tpls[i].name = c->tpls[i].name;
      b->tpls[i].ast = NULL;
      Tok *toks = NULL;
      size_t ntoks = 0;
      Str err;
      if (tokenize_tpl(a, c->tpls[i].name, c->tpls[i].src, &toks, &ntoks, &err)) {
        Node *root = NULL;
        if (parse_tpl(a, c->tpls[i].name, toks, ntoks, &root, &err)) {
          b->tpls[i].ast = root;
          c->tpls[i].ok = 1;
          c->tpls[i].ast = root;
          continue;
        }
      }
      build_report_push(b, err);
      c->tpls[i].ok = 0;
      c->tpls[i].err = err;
    }
  }
  /* 1. readSources: sort files by path; .md = source, else static. */
  {
    FileEnt *files = c->files;
    size_t nf = c->nfiles;
    /* stable sort by path: qsort with the original index as tiebreaker */
    typedef struct {
      FileEnt *f;
      size_t idx;
    } FIdx;
    FIdx *fi = (FIdx *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(FIdx));
    for (size_t i = 0; i < nf; i++) {
      fi[i].f = &files[i];
      fi[i].idx = i;
    }
    /* insertion sort (few hundred files) */
    for (size_t i = 1; i < nf; i++) {
      FIdx t = fi[i];
      size_t j = i;
      while (j > 0) {
        int cc = str_cmp(fi[j - 1].f->path, t.f->path);
        if (cc > 0 || (cc == 0 && fi[j - 1].idx > t.idx)) {
          fi[j] = fi[j - 1];
          j--;
        } else break;
      }
      fi[j] = t;
    }
    b->srcs = (Src *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(Src));
    b->statics = (FileEnt *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(FileEnt));
    b->nsrcs = 0;
    b->nstatics = 0;
    for (size_t i = 0; i < nf; i++) {
      FileEnt *f = fi[i].f;
      if (f->path.n >= 3 && memcmp(f->path.p + f->path.n - 3, ".md", 3) == 0) {
        Src *s = &b->srcs[b->nsrcs++];
        memset(s, 0, sizeof(*s));
        s->path = f->path;
        s->text = normalize_crlf(a, f->content);
      } else {
        b->statics[b->nstatics++] = *f;
      }
    }
  }
  /* 2. frontMatter */
  {
    size_t kept = 0;
    for (size_t i = 0; i < b->nsrcs; i++) {
      Src *s = &b->srcs[i];
      Str err, body;
      if (!parse_fm(a, s->text, &s->fm, &body, &err)) {
        Buf lb = {0};
        buf_adds(&lb, "refused ");
        buf_addn(&lb, s->path.p, s->path.n);
        buf_adds(&lb, ": ");
        buf_addn(&lb, err.p, err.n);
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
        continue;
      }
      s->body = body;
      s->post = s->path.n >= 6 && memcmp(s->path.p, "posts/", 6) == 0;
      const char *own = NULL;
      if (s->path.n >= 5 && memcmp(s->path.p, "tags/", 5) == 0) own = "tags";
      else if (s->path.n >= 5 && memcmp(s->path.p, "page/", 5) == 0) own = "page";
      if (own != NULL) {
        Buf lb = {0};
        buf_adds(&lb, "refused ");
        buf_addn(&lb, s->path.p, s->path.n);
        buf_adds(&lb, ": ");
        buf_adds(&lb, own);
        buf_adds(&lb, "/ is the generator's own");
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
        continue;
      }
      if (s->post && !s->fm.has_date) {
        Buf lb = {0};
        buf_adds(&lb, "refused ");
        buf_addn(&lb, s->path.p, s->path.n);
        buf_adds(&lb, ": a post needs a date");
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
        continue;
      }
      Str layout = s->fm.has_layout ? s->fm.layout : str_lit(s->post ? "post" : "page");
      int has_layout = 0;
      for (size_t k = 0; k < b->ntpls; k++) {
        if (b->tpls[k].ast != NULL && b->tpls[k].name.n == layout.n && memcmp(b->tpls[k].name.p, layout.p, layout.n) == 0) {
          has_layout = 1;
          break;
        }
      }
      if (!has_layout) {
        Buf lb = {0};
        buf_adds(&lb, "refused ");
        buf_addn(&lb, s->path.p, s->path.n);
        buf_adds(&lb, ": layout \"");
        buf_addn(&lb, layout.p, layout.n);
        buf_adds(&lb, "\" is no template that parses");
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
        continue;
      }
      s->layout = layout;
      if (s->fm.has_draft && s->fm.draft) {
        Buf lb = {0};
        buf_adds(&lb, "draft ");
        buf_addn(&lb, s->path.p, s->path.n);
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
        continue;
      }
      s->kept = 1;
      if (kept != i) b->srcs[kept] = *s;
      kept++;
    }
    b->nsrcs = kept;
  }
  /* 3. permalinks */
  {
    typedef struct {
      Str dir;
      StrSet taken;
    } DirTaken;
    size_t dcap = 8, dn = 0;
    DirTaken *dirs = (DirTaken *)arena_alloc(a, dcap * sizeof(DirTaken));
    for (size_t i = 0; i < b->nsrcs; i++) {
      Src *s = &b->srcs[i];
      Str dir;
      if (s->post) {
        dir = str_lit("posts");
      } else {
        size_t dl;
        const char *dp = dir_of_str(s->path, &dl);
        dir = str_make(dp, dl);
      }
      DirTaken *dt = NULL;
      for (size_t k = 0; k < dn; k++) {
        if (dirs[k].dir.n == dir.n && memcmp(dirs[k].dir.p, dir.p, dir.n) == 0) {
          dt = &dirs[k];
          break;
        }
      }
      if (dt == NULL) {
        if (dn == dcap) {
          dcap *= 2;
          DirTaken *nd = (DirTaken *)arena_alloc(a, dcap * sizeof(DirTaken));
          memcpy(nd, dirs, dn * sizeof(DirTaken));
          dirs = nd;
        }
        dt = &dirs[dn++];
        dt->dir = dir;
        memset(&dt->taken, 0, sizeof(dt->taken));
        dt->taken.arena = a;
        if (dir.n == 0) {
          strset_add(a, &dt->taken, str_lit("posts"));
          strset_add(a, &dt->taken, str_lit("tags"));
          strset_add(a, &dt->taken, str_lit("page"));
        }
      }
      Str base_slug = s->fm.has_slug ? s->fm.slug : slugify_str(a, s->fm.title);
      Str slug = unique_slug(a, base_slug, &dt->taken);
      s->slug = slug;
      Buf ob = {0};
      if (dir.n > 0) {
        buf_addn(&ob, dir.p, dir.n);
        buf_addc(&ob, '/');
      }
      buf_addn(&ob, slug.p, slug.n);
      s->outdir = str_make(arena_dup(a, ob.buf ? ob.buf : "", ob.len), ob.len);
      free(ob.buf);
      Buf ub = {0};
      buf_addn(&ub, b->base.p, b->base.n);
      buf_addc(&ub, '/');
      buf_addn(&ub, s->outdir.p, s->outdir.n);
      buf_addc(&ub, '/');
      s->url = str_make(arena_dup(a, ub.buf ? ub.buf : "", ub.len), ub.len);
      free(ub.buf);
    }
  }
  /* 4. markdown */
  {
    for (size_t i = 0; i < b->nsrcs; i++) {
      Src *s = &b->srcs[i];
      size_t dl;
      const char *dp = dir_of_str(s->path, &dl);
      LinkCtx lc = {b, s, str_make(dp, dl)};
      MdEnv env = {md_link, &lc};
      /* Render into the shared reusable Buf (no malloc per source). */
      Buf *html = &b->render_buf;
      html->len = 0;
      MdResult res;
      render_md(a, s->body, &env, html, &res, &b->scratch_buf);
      s->html = str_make(arena_dup(a, html->buf ? html->buf : "", html->len), html->len);
      s->toc = res.toc;
      s->ntoc = res.ntoc;
      s->excerpt = res.excerpt;
    }
  }
  /* 5. collections */
  {
    b->posts = (Src **)arena_alloc(a, (b->nsrcs > 0 ? b->nsrcs : 1) * sizeof(Src *));
    b->nposts = 0;
    for (size_t i = 0; i < b->nsrcs; i++)
      if (b->srcs[i].post) b->posts[b->nposts++] = &b->srcs[i];
    qsort(b->posts, b->nposts, sizeof(Src *), cmp_src_date);
    for (size_t k = 0; k < b->nposts; k++) {
      b->posts[k]->newer = k == 0 ? NULL : b->posts[k - 1];
      b->posts[k]->older = k == b->nposts - 1 ? NULL : b->posts[k + 1];
    }
  }
  /* 6. tags */
  {
    size_t tcap = 16, tn = 0;
    Tag **tags = (Tag **)arena_alloc(a, tcap * sizeof(Tag *));
    for (size_t pi = 0; pi < b->nposts; pi++) {
      Src *p = b->posts[pi];
      /* ownTags: each slug once, first spelling kept */
      StrSet seen = {0};
      seen.arena = a;
      size_t rcap = 4, rn = 0;
      Tag **refs = (Tag **)arena_alloc(a, rcap * sizeof(Tag *));
      for (size_t ti = 0; ti < p->fm.ntags; ti++) {
        Str tname = p->fm.tags[ti];
        Str tsl = slugify_str(a, tname);
        if (strset_has(&seen, tsl)) continue;
        strset_add(a, &seen, tsl);
        Tag *tg = NULL;
        for (size_t k = 0; k < tn; k++) {
          if (tags[k]->slug.n == tsl.n && memcmp(tags[k]->slug.p, tsl.p, tsl.n) == 0) {
            tg = tags[k];
            break;
          }
        }
        if (tg == NULL) {
          if (tn == tcap) {
            tcap *= 2;
            Tag **nt = (Tag **)arena_alloc(a, tcap * sizeof(Tag *));
            memcpy(nt, tags, tn * sizeof(Tag *));
            tags = nt;
          }
          tg = (Tag *)arena_alloc(a, sizeof(Tag));
          memset(tg, 0, sizeof(*tg));
          tags[tn++] = tg;
          tg->name = tname;
          tg->slug = tsl;
          Buf ub = {0};
          buf_addn(&ub, b->base.p, b->base.n);
          buf_adds(&ub, "/tags/");
          buf_addn(&ub, tsl.p, tsl.n);
          buf_addc(&ub, '/');
          tg->url = str_make(arena_dup(a, ub.buf ? ub.buf : "", ub.len), ub.len);
          free(ub.buf);
        }
        if (tg->nposts == tg->cap) {
          size_t nc = tg->cap ? tg->cap * 2 : 4;
          Src **np = (Src **)arena_alloc(a, nc * sizeof(Src *));
          if (tg->posts) memcpy(np, tg->posts, tg->nposts * sizeof(Src *));
          tg->posts = np;
          tg->cap = nc;
        }
        tg->posts[tg->nposts++] = p;
        if (rn == rcap) {
          rcap *= 2;
          Tag **nr = (Tag **)arena_alloc(a, rcap * sizeof(Tag *));
          memcpy(nr, refs, rn * sizeof(Tag *));
          refs = nr;
        }
        refs[rn++] = tg;
      }
      p->tagrefs = refs;
      p->ntagrefs = rn;
    }
    for (size_t i = 0; i < b->nsrcs; i++) {
      Src *s = &b->srcs[i];
      if (s->post) continue;
      StrSet seen = {0};
      seen.arena = a;
      size_t rcap = 4, rn = 0;
      Tag **refs = (Tag **)arena_alloc(a, rcap * sizeof(Tag *));
      for (size_t ti = 0; ti < s->fm.ntags; ti++) {
        Str tname = s->fm.tags[ti];
        Str tsl = slugify_str(a, tname);
        if (strset_has(&seen, tsl)) continue;
        strset_add(a, &seen, tsl);
        Tag *tg = NULL;
        for (size_t k = 0; k < tn; k++) {
          if (tags[k]->slug.n == tsl.n && memcmp(tags[k]->slug.p, tsl.p, tsl.n) == 0) {
            tg = tags[k];
            break;
          }
        }
        if (tg == NULL) {
          Tag *ph = (Tag *)arena_alloc(a, sizeof(Tag));
          memset(ph, 0, sizeof(*ph));
          ph->name = tname;
          ph->slug = tsl;
          ph->url = str_make("", 0);
          tg = ph;
        }
        if (rn == rcap) {
          rcap *= 2;
          Tag **nr = (Tag **)arena_alloc(a, rcap * sizeof(Tag *));
          memcpy(nr, refs, rn * sizeof(Tag *));
          refs = nr;
        }
        refs[rn++] = tg;
      }
      s->tagrefs = refs;
      s->ntagrefs = rn;
    }
    qsort(tags, tn, sizeof(Tag *), cmp_tag);
    b->tags = tags;
    b->ntags = tn;
  }
  /* 7. layouts */
  {
    TplCtx ctx = {b->tpls, b->ntpls, a};
    for (size_t i = 0; i < b->ntpls; i++)
      if (b->tpls[i].ast != NULL) resolve_includes_list(b->tpls[i].ast, &ctx);
    for (size_t i = 0; i < b->nposts; i++) b->posts[i]->card = card_of(b, b->posts[i]);
    Field *sf = (Field *)arena_alloc(a, 2 * sizeof(Field));
    sf[0].name = str_lit("title");
    sf[0].val = val_str(a, c->site.title);
    sf[1].name = str_lit("base_url");
    sf[1].val = val_str(a, b->base);
    Val *site = val_rec(a, sf, 2);
    Val **atitems = (Val **)arena_alloc(a, (b->ntags > 0 ? b->ntags : 1) * sizeof(Val *));
    for (size_t i = 0; i < b->ntags; i++) {
      Tag *t = b->tags[i];
      Field *tf = (Field *)arena_alloc(a, 4 * sizeof(Field));
      tf[0].name = str_lit("name");
      tf[0].val = val_str(a, t->name);
      tf[1].name = str_lit("slug");
      tf[1].val = val_str(a, t->slug);
      tf[2].name = str_lit("url");
      tf[2].val = val_str(a, t->url);
      tf[3].name = str_lit("count");
      tf[3].val = val_num(a, (long long)t->nposts);
      atitems[i] = val_rec(a, tf, 4);
    }
    Val *allTags = val_arr(a, atitems, b->ntags);
    long long rc = c->site.recent;
    if (rc < 0) rc = 0;
    if (rc > (long long)b->nposts) rc = (long long)b->nposts;
    Val **ritems = (Val **)arena_alloc(a, ((size_t)rc > 0 ? (size_t)rc : 1) * sizeof(Val *));
    for (long long i = 0; i < rc; i++) ritems[i] = b->posts[i]->card;
    Val *recent = val_arr(a, ritems, (size_t)rc);
    for (size_t i = 0; i < b->nsrcs; i++) {
      Src *s = &b->srcs[i];
      Field ex[1];
      ex[0].name = str_lit("page");
      ex[0].val = page_of(b, s);
      Str html;
      if (render_tpl(b, &ctx, site, allTags, recent, s->layout, ex, 1, &html)) {
        /* outdir + "/index.html", straight into the arena (no temp Buf). */
        size_t plen = s->outdir.n + 11;
        char *pp = (char *)arena_alloc(a, plen + 1);
        memcpy(pp, s->outdir.p, s->outdir.n);
        memcpy(pp + s->outdir.n, "/index.html", 11);
        pp[plen] = 0;
        build_out_push(b, str_make(pp, plen), html);
      }
    }
    long long per = c->site.per_page;
    long long pages = 1;
    if (per > 0) {
      pages = ((long long)b->nposts + per - 1) / per;
      if (pages < 1) pages = 1;
    }
    for (long long k = 1; k <= pages; k++) {
      long long from = (k - 1) * per;
      long long to = k * per;
      if (to > (long long)b->nposts) to = (long long)b->nposts;
      if (from < 0) from = 0;
      size_t cnt = to > from ? (size_t)(to - from) : 0;
      Val **pitems = (Val **)arena_alloc(a, (cnt > 0 ? cnt : 1) * sizeof(Val *));
      for (size_t i = 0; i < cnt; i++) pitems[i] = b->posts[from + (long long)i]->card;
      Val *posts = val_arr(a, pitems, cnt);
      Buf ub2 = {0}, ub3 = {0};
      if (k == 1) {
        buf_addn(&ub2, b->base.p, b->base.n);
        buf_addc(&ub2, '/');
      } else {
        buf_addn(&ub2, b->base.p, b->base.n);
        buf_adds(&ub2, "/page/");
        char nb[24];
        long long v = k;
        int nl = 0;
        char rev[24];
        int rl = 0;
        do {
          rev[rl++] = (char)('0' + (v % 10));
          v /= 10;
        } while (v > 0);
        while (rl > 0) nb[nl++] = rev[--rl];
        buf_addn(&ub2, nb, (size_t)nl);
        buf_addc(&ub2, '/');
      }
      Str urlk = str_make(arena_dup(a, ub2.buf ? ub2.buf : "", ub2.len), ub2.len);
      free(ub2.buf);
      Str newer_url, older_url;
      if (k == 1) {
        newer_url = str_make("", 0);
      } else if (k == 2) {
        /* indexUrl(1) = base + '/' */
        buf_addn(&ub3, b->base.p, b->base.n);
        buf_addc(&ub3, '/');
        newer_url = str_make(arena_dup(a, ub3.buf ? ub3.buf : "", ub3.len), ub3.len);
      } else {
        /* indexUrl(k-1) for k-1 >= 2 */
        buf_addn(&ub3, b->base.p, b->base.n);
        buf_adds(&ub3, "/page/");
        char nb[24];
        long long v = k - 1;
        int nl = 0;
        char rev[24];
        int rl = 0;
        do {
          rev[rl++] = (char)('0' + (v % 10));
          v /= 10;
        } while (v > 0);
        while (rl > 0) nb[nl++] = rev[--rl];
        buf_addn(&ub3, nb, (size_t)nl);
        buf_addc(&ub3, '/');
        newer_url = str_make(arena_dup(a, ub3.buf ? ub3.buf : "", ub3.len), ub3.len);
      }
      ub3.len = 0;
      if (k == pages) {
        older_url = str_make("", 0);
      } else {
        buf_addn(&ub3, b->base.p, b->base.n);
        buf_adds(&ub3, "/page/");
        char nb[24];
        long long v = k + 1;
        int nl = 0;
        char rev[24];
        int rl = 0;
        do {
          rev[rl++] = (char)('0' + (v % 10));
          v /= 10;
        } while (v > 0);
        while (rl > 0) nb[nl++] = rev[--rl];
        buf_addn(&ub3, nb, (size_t)nl);
        buf_addc(&ub3, '/');
        older_url = str_make(arena_dup(a, ub3.buf ? ub3.buf : "", ub3.len), ub3.len);
      }
      free(ub3.buf);
      Field *pf = (Field *)arena_alloc(a, 5 * sizeof(Field));
      pf[0].name = str_lit("number");
      pf[0].val = val_num(a, k);
      pf[1].name = str_lit("total");
      pf[1].val = val_num(a, pages);
      pf[2].name = str_lit("url");
      pf[2].val = val_str(a, urlk);
      pf[3].name = str_lit("newer_url");
      pf[3].val = val_str(a, newer_url);
      pf[4].name = str_lit("older_url");
      pf[4].val = val_str(a, older_url);
      Val *pager = val_rec(a, pf, 5);
      Field ex[2];
      ex[0].name = str_lit("posts");
      ex[0].val = posts;
      ex[1].name = str_lit("pager");
      ex[1].val = pager;
      Str html;
      if (render_tpl(b, &ctx, site, allTags, recent, str_lit("index"), ex, 2, &html)) {
        if (k == 1) {
          build_out_push(b, str_lit("index.html"), html);
        } else {
          Buf pb = {0};
          buf_adds(&pb, "page/");
          char nb[24];
          long long v = k;
          int nl = 0;
          char rev[24];
          int rl = 0;
          do {
            rev[rl++] = (char)('0' + (v % 10));
            v /= 10;
          } while (v > 0);
          while (rl > 0) nb[nl++] = rev[--rl];
          buf_addn(&pb, nb, (size_t)nl);
          buf_adds(&pb, "/index.html");
          build_out_push(b, str_make(arena_dup(a, pb.buf ? pb.buf : "", pb.len), pb.len), html);
          free(pb.buf);
        }
      }
    }
    for (size_t ti = 0; ti < b->ntags; ti++) {
      Tag *t = b->tags[ti];
      Field ex[1];
      ex[0].name = str_lit("tag");
      ex[0].val = tag_of(b, t);
      Str html;
      if (render_tpl(b, &ctx, site, allTags, recent, str_lit("tag"), ex, 1, &html)) {
        Buf pb = {0};
        buf_adds(&pb, "tags/");
        buf_addn(&pb, t->slug.p, t->slug.n);
        buf_adds(&pb, "/index.html");
        build_out_push(b, str_make(arena_dup(a, pb.buf ? pb.buf : "", pb.len), pb.len), html);
        free(pb.buf);
      }
    }
    {
      Str html;
      if (render_tpl(b, &ctx, site, allTags, recent, str_lit("tags"), NULL, 0, &html)) {
        build_out_push(b, str_lit("tags/index.html"), html);
      }
    }
    for (size_t i = 0; i < b->nstatics; i++) {
      FileEnt *f = &b->statics[i];
      if (build_out_has(b, f->path)) {
        Buf lb = {0};
        buf_adds(&lb, "shadowed ");
        buf_addn(&lb, f->path.p, f->path.n);
        buf_adds(&lb, ": a built page has its path");
        build_report_push(b, str_make(arena_dup(a, lb.buf ? lb.buf : "", lb.len), lb.len));
        free(lb.buf);
      } else {
        build_out_push(b, f->path, f->content);
      }
    }
  }
  /* 8. report */
  {
    size_t total = b->nreport + b->nbroken;
    Str *lines = (Str *)arena_alloc(a, (total > 0 ? total : 1) * sizeof(Str));
    size_t li = 0;
    for (size_t i = 0; i < b->nreport; i++) lines[li++] = b->report[i];
    for (size_t i = 0; i < b->nbroken; i++) lines[li++] = b->broken[i];
    qsort(lines, total, sizeof(Str), cmp_strp);
    size_t npages = 0;
    for (size_t i = 0; i < b->nsrcs; i++)
      if (!b->srcs[i].post) npages++;
    Buf rb = {0};
    for (size_t i = 0; i < total; i++) {
      buf_addn(&rb, lines[i].p, lines[i].n);
      buf_addc(&rb, '\n');
    }
    buf_adds(&rb, "built ");
    {
      char nb[24];
      long long v = (long long)npages;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (v % 10));
        v /= 10;
      } while (v > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&rb, nb, (size_t)nl);
    }
    buf_adds(&rb, " pages, ");
    {
      char nb[24];
      long long v = (long long)b->nposts;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (v % 10));
        v /= 10;
      } while (v > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&rb, nb, (size_t)nl);
    }
    buf_adds(&rb, " posts, ");
    {
      char nb[24];
      long long v = (long long)b->ntags;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (v % 10));
        v /= 10;
      } while (v > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&rb, nb, (size_t)nl);
    }
    buf_adds(&rb, " tags and ");
    {
      char nb[24];
      long long v = (long long)b->nstatics;
      int nl = 0;
      char rev[24];
      int rl = 0;
      do {
        rev[rl++] = (char)('0' + (v % 10));
        v /= 10;
      } while (v > 0);
      while (rl > 0) nb[nl++] = rev[--rl];
      buf_addn(&rb, nb, (size_t)nl);
    }
    buf_adds(&rb, " static files\n");
    build_out_push(b, str_lit("build-report.txt"), str_make(arena_dup(a, rb.buf ? rb.buf : "", rb.len), rb.len));
    free(rb.buf);
  }
}

/* ------------------------------------------------------------------ emit + entry */

/* JSON.stringify escaping of a string value (with the quotes). Bulk runs:
 * copy every stretch with no '"' , '\\' or control char in one memcpy.
 * The next special byte is found 16 bytes at a time with SSE2 (the
 * top entries' "SSE2 mask-draining"): bytes equal to '"' or '\\', or
 * with (b & 0xE0) == 0 (i.e. b < 0x20). */
#include <emmintrin.h>
static void emit_json_str(Buf *out, Str s) {
  buf_addc(out, '"');
  const char *p = s.p;
  const char *end = s.p + s.n;
  const __m128i q = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i e0 = _mm_set1_epi8((char)0xE0);
  const __m128i zero = _mm_setzero_si128();
  while (p < end) {
    size_t rem = (size_t)(end - p);
    if (rem >= 16) {
      __m128i x = _mm_loadu_si128((const __m128i *)p);
      __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, q), _mm_cmpeq_epi8(x, bs));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_and_si128(x, e0), zero));
      unsigned mask = (unsigned)_mm_movemask_epi8(m);
      if (mask == 0) {
        buf_addn(out, p, 16);
        p += 16;
        continue;
      }
      unsigned off = (unsigned)__builtin_ctz(mask);
      if (off > 0) buf_addn(out, p, off);
      p += off;
    }
    /* Scalar tail / the special byte at p. */
    if (p >= end) break;
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\' || c < 0x20) {
      if (c == '"') buf_adds(out, "\\\"");
      else if (c == '\\') buf_adds(out, "\\\\");
      else if (c == '\b') buf_adds(out, "\\b");
      else if (c == '\f') buf_adds(out, "\\f");
      else if (c == '\n') buf_adds(out, "\\n");
      else if (c == '\r') buf_adds(out, "\\r");
      else if (c == '\t') buf_adds(out, "\\t");
      else {
        char hb[8];
        hb[0] = '\\';
        hb[1] = 'u';
        hb[2] = '0';
        hb[3] = '0';
        hb[4] = "0123456789abcdef"[(c >> 4) & 0xF];
        hb[5] = "0123456789abcdef"[c & 0xF];
        buf_addn(out, hb, 6);
      }
      p++;
    } else {
      /* A short clean run (tail): copy it in bulk. */
      const char *r = p;
      while (r < end) {
        unsigned char d = (unsigned char)*r;
        if (d == '"' || d == '\\' || d < 0x20) break;
        r++;
      }
      buf_addn(out, p, (size_t)(r - p));
      p = r;
    }
  }
  buf_addc(out, '"');
}

/* Build all cases from the raw cases.json text; return the answers.json text
 * (malloc'd, *out_len set). Returns NULL on failure. */
char *build_all(const char *json, size_t len, size_t *out_len) {
  Arena arena = {0};
  Case *cases = NULL;
  size_t ncases = 0;
  if (!parse_cases(&arena, json, len, &cases, &ncases)) {
    arena_free_all(&arena);
    return NULL;
  }
  Buf out = {0};
  /* Pre-size the output: answers run ~2.5x the input bytes, so reserving
   * 3x avoids the doubling-realloc copy churn (~2x the output size). */
  buf_reserve(&out, len * 3);
  buf_addc(&out, '[');
  for (size_t ci = 0; ci < ncases; ci++) {
    Build b;
    memset(&b, 0, sizeof(b));
    b.arena = &arena;
    b.c = &cases[ci];
    build_case(&b);
    if (ci > 0) buf_addc(&out, ',');
    buf_addc(&out, '{');
    for (size_t i = 0; i < b.nout; i++) {
      if (i > 0) buf_addc(&out, ',');
      emit_json_str(&out, b.out[i].path);
      buf_addc(&out, ':');
      emit_json_str(&out, b.out[i].content);
    }
    buf_addc(&out, '}');
    free(b.linkbuf.buf);
    free(b.render_buf.buf);
    free(b.scratch_buf.buf);
  }
  buf_addc(&out, ']');
  arena_free_all(&arena);
  *out_len = out.len;
  return out.buf;
}

