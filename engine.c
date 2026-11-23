/* engine.c — r3 streaming zero-copy SIMD site generator for
 * site-generator-speed-01-run-20261122 ("Build a static site faster").
 *
 * Written fresh in this session. build_all() takes the raw cases.json bytes
 * and returns the answers.json bytes for every case, exactly as makefun-site
 * 1.0.0 builds them. The design is a streaming pipeline over the input
 * buffer: no JSON DOM, no per-value allocation (one bump allocator), SIMD
 * scans for strings / lines / escaping, templates compiled once per case
 * into flat jump ops, and page HTML rendered straight into the output.
 * Only SSE2 is used, so the module loads on any x86-64 machine.
 *
 * Exactness notes (all checked against the kit's reference + oracle):
 *  - slug: NFKD via the small table below (post-drop expansions), combining
 *    marks dropped, A-Z lowered, runs of non [a-z0-9] one '-', cut to 64,
 *    'untitled' if empty; uniqueness per directory with -2, -3, ...
 *  - ordering is code-unit order; the corpus is BMP-only, so UTF-8 byte
 *    order is the same order.
 *  - JSON strings without a backslash are used in place, never copied.
 *  - template values are read through interned field ids; loop scopes are
 *    searched innermost-first, with `loop` winning over an element of the
 *    same name, exactly like the reference's scope chain.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <immintrin.h>

/* ------------------------------------------------------------------ types */

typedef struct {
  const unsigned char *p;
  size_t n;
} Slice;

static Slice slice_make(const unsigned char *p, size_t n) {
  Slice s;
  s.p = p;
  s.n = n;
  return s;
}

/* A slice of a string literal, its length from strlen (never hand-counted:
 * a wrong count silently garbles the message). */
#define SLIT(s) slice_make((const unsigned char *)(s), strlen(s))

static int slice_eq(Slice a, Slice b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

/* Lexicographic by bytes; a shorter prefix sorts first. */
static int slice_cmp(Slice a, Slice b) {
  size_t m = a.n < b.n ? a.n : b.n;
  int c = m == 0 ? 0 : memcmp(a.p, b.p, m);
  if (c != 0) return c;
  if (a.n < b.n) return -1;
  if (a.n > b.n) return 1;
  return 0;
}

static uint64_t slice_hash(Slice s) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.n; i++) {
    h ^= s.p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* A growable byte buffer (the answers output, scratch areas). */
typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} Grow;

static int grow_reserve(Grow *g, size_t extra) {
  if (g->len + extra <= g->cap) return 1;
  size_t nc = g->cap == 0 ? 4096 : g->cap;
  while (nc < g->len + extra) nc *= 2;
  unsigned char *nd = (unsigned char *)realloc(g->data, nc);
  if (!nd) return 0;
  g->data = nd;
  g->cap = nc;
  return 1;
}

static int grow_addn(Grow *g, const void *p, size_t n) {
  if (!grow_reserve(g, n)) return 0;
  memcpy(g->data + g->len, p, n);
  g->len += n;
  return 1;
}

static int grow_addc(Grow *g, unsigned char c) {
  if (!grow_reserve(g, 1)) return 0;
  g->data[g->len++] = c;
  return 1;
}

static int grow_add_slice(Grow *g, Slice s) { return grow_addn(g, s.p, s.n); }

static int grow_add_lit(Grow *g, const char *s) { return grow_addn(g, s, strlen(s)); }

/* A bump allocator: blocks never move, so slices into it stay valid. */
typedef struct BumpBlock {
  struct BumpBlock *next;
  size_t used;
  size_t cap;
  unsigned char mem[];
} BumpBlock;

typedef struct {
  BumpBlock *head;
  BumpBlock *tail;
} Bump;

static void *bump_alloc(Bump *b, size_t n) {
  if (n == 0) n = 1;
  n = (n + 15) & ~(size_t)15;
  BumpBlock *blk = b->tail;
  if (!blk || blk->used + n > blk->cap) {
    size_t cap = 65536;
    while (cap < n) cap *= 2;
    blk = (BumpBlock *)malloc(sizeof(BumpBlock) + cap);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->used = 0;
    blk->cap = cap;
    if (b->tail) b->tail->next = blk;
    else b->head = blk;
    b->tail = blk;
  }
  void *p = blk->mem + blk->used;
  blk->used += n;
  return p;
}

static Slice bump_dup(Bump *b, Slice s) {
  unsigned char *p = (unsigned char *)bump_alloc(b, s.n == 0 ? 1 : s.n);
  if (!p) return slice_make(NULL, 0);
  if (s.n) memcpy(p, s.p, s.n);
  return slice_make(p, s.n);
}

static void bump_free_all(Bump *b) {
  BumpBlock *blk = b->head;
  while (blk) {
    BumpBlock *nx = blk->next;
    free(blk);
    blk = nx;
  }
  b->head = b->tail = NULL;
}

/* ------------------------------------------------------------ SIMD scans */

/* First index of byte c at or after p[0..n), or n. glibc memchr is already
 * vectorised; use it for single-byte searches. */
static size_t find_byte(const unsigned char *p, size_t n, unsigned char c) {
  const void *f = n == 0 ? NULL : memchr(p, c, n);
  return f ? (size_t)((const unsigned char *)f - p) : n;
}

/* First index where the byte equals a or b, or n. SSE2, 16 bytes at a time. */
static inline __attribute__((always_inline)) size_t scan_two(const unsigned char *p, size_t n, unsigned char a, unsigned char b) {
  size_t i = 0;
  __m128i va = _mm_set1_epi8((char)a);
  __m128i vb = _mm_set1_epi8((char)b);
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    i += 16;
  }
  while (i < n) {
    if (p[i] == a || p[i] == b) return i;
    i++;
  }
  return n;
}

/* First index where the byte is one of & < > " ', or n. SSE2. */
static inline __attribute__((always_inline)) size_t scan_html(const unsigned char *p, size_t n) {
  size_t i = 0;
  __m128i va = _mm_set1_epi8('&');
  __m128i vb = _mm_set1_epi8('<');
  __m128i vc = _mm_set1_epi8('>');
  __m128i vd = _mm_set1_epi8('"');
  __m128i ve = _mm_set1_epi8('\'');
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_cmpeq_epi8(v, va);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, vb));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, vc));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, vd));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, ve));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    i += 16;
  }
  while (i < n) {
    unsigned char c = p[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
    i++;
  }
  return n;
}

/* First index where the byte is an inline special: \ ` * [ & < > " ', or n. */
static inline __attribute__((always_inline)) size_t scan_inline(const unsigned char *p, size_t n) {
  size_t i = 0;
  __m128i v0 = _mm_set1_epi8('\\');
  __m128i v1 = _mm_set1_epi8('`');
  __m128i v2 = _mm_set1_epi8('*');
  __m128i v3 = _mm_set1_epi8('[');
  __m128i v4 = _mm_set1_epi8('&');
  __m128i v5 = _mm_set1_epi8('<');
  __m128i v6 = _mm_set1_epi8('>');
  __m128i v7 = _mm_set1_epi8('"');
  __m128i v8 = _mm_set1_epi8('\'');
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_cmpeq_epi8(v, v0);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v1));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v2));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v3));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v4));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v5));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v6));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v7));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, v8));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    i += 16;
  }
  while (i < n) {
    unsigned char c = p[i];
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
    i++;
  }
  return n;
}

/* First index where the byte needs JSON escaping (" \ or < 0x20), or n. */
static inline __attribute__((always_inline)) size_t scan_jesc(const unsigned char *p, size_t n) {
  size_t i = 0;
  __m128i vq = _mm_set1_epi8('"');
  __m128i vb = _mm_set1_epi8('\\');
  __m128i zero = _mm_setzero_si128();
  __m128i lim = _mm_set1_epi8(0x20);
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_cmpeq_epi8(v, vq);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, vb));
    /* unsigned v < 0x20 iff saturating (v - 0x20) is 0. */
    __m128i sub = _mm_subs_epu8(v, lim);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(sub, zero));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    i += 16;
  }
  while (i < n) {
    unsigned char c = p[i];
    if (c == '"' || c == '\\' || c < 0x20) return i;
    i++;
  }
  return n;
}

/* Whether any byte in p[0..n) is >= 0x80. */
static int has_hi(const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    if (_mm_movemask_epi8(v)) return 1;
    i += 16;
  }
  while (i < n) {
    if (p[i] >= 0x80) return 1;
    i++;
  }
  return 0;
}

/* ---------------------------------------------------------------- UTF-8 */

/* Decode one UTF-8 code point at p[i..n). Returns its byte length, or 0
 * when the bytes are not a valid sequence (the caller then treats the byte
 * as a single-byte character). */
static size_t u8_decode(const unsigned char *p, size_t n, size_t i, uint32_t *cp) {
  unsigned char c = p[i];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if (c >= 0xC2 && c <= 0xDF) {
    if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return 0;
    *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[i + 1] & 0x3F);
    return 2;
  }
  if (c >= 0xE0 && c <= 0xEF) {
    if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) | (uint32_t)(p[i + 2] & 0x3F);
    if (v < 0x800) return 0;
    if (v >= 0xD800 && v <= 0xDFFF) return 0;
    *cp = v;
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (i + 3 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12) | ((uint32_t)(p[i + 2] & 0x3F) << 6) | (uint32_t)(p[i + 3] & 0x3F);
    if (v < 0x10000 || v > 0x10FFFF) return 0;
    *cp = v;
    return 4;
  }
  return 0;
}

/* Encode cp as UTF-8 into out[4]; returns the byte length. */
static size_t u8_encode(uint32_t cp, unsigned char *out) {
  if (cp < 0x80) {
    out[0] = (unsigned char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (unsigned char)(0xC0 | (cp >> 6));
    out[1] = (unsigned char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (unsigned char)(0xE0 | (cp >> 12));
    out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (unsigned char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (unsigned char)(0xF0 | (cp >> 18));
  out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (unsigned char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Combining-mark ranges (Unicode general category M), the common ones. A
 * literal mark in the input is dropped by slugify, as NFKD + \p{M} removal
 * drops it. */
static const uint32_t MARK_RANGES[][2] = {
  {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
  {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
  {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
  {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
  {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
  {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x0900, 0x0903},
  {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
  {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EB9},
  {0x0EBB, 0x0EBC}, {0x0EC8, 0x0ECE}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84},
  {0x0F86, 0x0F87}, {0x0F8D, 0x0F97}, {0x0F99, 0x0FBC}, {0x0FC6, 0x0FC6},
  {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
  {0xFE20, 0xFE2F},
};

static int cp_is_mark(uint32_t cp) {
  size_t lo = 0, hi = sizeof(MARK_RANGES) / sizeof(MARK_RANGES[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < MARK_RANGES[mid][0]) hi = mid;
    else if (cp > MARK_RANGES[mid][1]) lo = mid + 1;
    else return 1;
  }
  return 0;
}

/* ------------------------------------------------------------- slug table */

/* NFKD expansions with combining marks already dropped, for the code
 * points whose decomposition contributes ASCII (or a gap). A code point
 * absent here contributes a gap, unless it is a mark (dropped) or ASCII.
 * Fullwidth ASCII (U+FF01..U+FF5E) is handled by arithmetic, not the table.
 * Generated from node String.normalize('NFKD') for every code point
 * U+0080..U+10FFFF (an entry exactly when the mark-stripped NFKD differs
 * from the character itself and holds an ASCII letter or digit), so the
 * table matches the reference's slugify by construction. */
typedef struct {
  uint32_t cp;
  const char *exp;
} SlugExp;

static const SlugExp SLUG_EXP[] = {
  {0xAA, "a"},   {0xB2, "2"},   {0xB3, "3"},   {0xB9, "1"},   {0xBA, "o"},
  {0xBC, "1⁄4"},   {0xBD, "1⁄2"},   {0xBE, "3⁄4"},   {0xC0, "A"},   {0xC1, "A"},
  {0xC2, "A"},   {0xC3, "A"},   {0xC4, "A"},   {0xC5, "A"},   {0xC7, "C"},
  {0xC8, "E"},   {0xC9, "E"},   {0xCA, "E"},   {0xCB, "E"},   {0xCC, "I"},
  {0xCD, "I"},   {0xCE, "I"},   {0xCF, "I"},   {0xD1, "N"},   {0xD2, "O"},
  {0xD3, "O"},   {0xD4, "O"},   {0xD5, "O"},   {0xD6, "O"},   {0xD9, "U"},
  {0xDA, "U"},   {0xDB, "U"},   {0xDC, "U"},   {0xDD, "Y"},   {0xE0, "a"},
  {0xE1, "a"},   {0xE2, "a"},   {0xE3, "a"},   {0xE4, "a"},   {0xE5, "a"},
  {0xE7, "c"},   {0xE8, "e"},   {0xE9, "e"},   {0xEA, "e"},   {0xEB, "e"},
  {0xEC, "i"},   {0xED, "i"},   {0xEE, "i"},   {0xEF, "i"},   {0xF1, "n"},
  {0xF2, "o"},   {0xF3, "o"},   {0xF4, "o"},   {0xF5, "o"},   {0xF6, "o"},
  {0xF9, "u"},   {0xFA, "u"},   {0xFB, "u"},   {0xFC, "u"},   {0xFD, "y"},
  {0xFF, "y"},   {0x100, "A"},   {0x101, "a"},   {0x102, "A"},   {0x103, "a"},
  {0x104, "A"},   {0x105, "a"},   {0x106, "C"},   {0x107, "c"},   {0x108, "C"},
  {0x109, "c"},   {0x10A, "C"},   {0x10B, "c"},   {0x10C, "C"},   {0x10D, "c"},
  {0x10E, "D"},   {0x10F, "d"},   {0x112, "E"},   {0x113, "e"},   {0x114, "E"},
  {0x115, "e"},   {0x116, "E"},   {0x117, "e"},   {0x118, "E"},   {0x119, "e"},
  {0x11A, "E"},   {0x11B, "e"},   {0x11C, "G"},   {0x11D, "g"},   {0x11E, "G"},
  {0x11F, "g"},   {0x120, "G"},   {0x121, "g"},   {0x122, "G"},   {0x123, "g"},
  {0x124, "H"},   {0x125, "h"},   {0x128, "I"},   {0x129, "i"},   {0x12A, "I"},
  {0x12B, "i"},   {0x12C, "I"},   {0x12D, "i"},   {0x12E, "I"},   {0x12F, "i"},
  {0x130, "I"},   {0x132, "IJ"},   {0x133, "ij"},   {0x134, "J"},   {0x135, "j"},
  {0x136, "K"},   {0x137, "k"},   {0x139, "L"},   {0x13A, "l"},   {0x13B, "L"},
  {0x13C, "l"},   {0x13D, "L"},   {0x13E, "l"},   {0x13F, "L·"},   {0x140, "l·"},
  {0x143, "N"},   {0x144, "n"},   {0x145, "N"},   {0x146, "n"},   {0x147, "N"},
  {0x148, "n"},   {0x149, "ʼn"},   {0x14C, "O"},   {0x14D, "o"},   {0x14E, "O"},
  {0x14F, "o"},   {0x150, "O"},   {0x151, "o"},   {0x154, "R"},   {0x155, "r"},
  {0x156, "R"},   {0x157, "r"},   {0x158, "R"},   {0x159, "r"},   {0x15A, "S"},
  {0x15B, "s"},   {0x15C, "S"},   {0x15D, "s"},   {0x15E, "S"},   {0x15F, "s"},
  {0x160, "S"},   {0x161, "s"},   {0x162, "T"},   {0x163, "t"},   {0x164, "T"},
  {0x165, "t"},   {0x168, "U"},   {0x169, "u"},   {0x16A, "U"},   {0x16B, "u"},
  {0x16C, "U"},   {0x16D, "u"},   {0x16E, "U"},   {0x16F, "u"},   {0x170, "U"},
  {0x171, "u"},   {0x172, "U"},   {0x173, "u"},   {0x174, "W"},   {0x175, "w"},
  {0x176, "Y"},   {0x177, "y"},   {0x178, "Y"},   {0x179, "Z"},   {0x17A, "z"},
  {0x17B, "Z"},   {0x17C, "z"},   {0x17D, "Z"},   {0x17E, "z"},   {0x17F, "s"},
  {0x1A0, "O"},   {0x1A1, "o"},   {0x1AF, "U"},   {0x1B0, "u"},   {0x1C4, "DZ"},
  {0x1C5, "Dz"},   {0x1C6, "dz"},   {0x1C7, "LJ"},   {0x1C8, "Lj"},   {0x1C9, "lj"},
  {0x1CA, "NJ"},   {0x1CB, "Nj"},   {0x1CC, "nj"},   {0x1CD, "A"},   {0x1CE, "a"},
  {0x1CF, "I"},   {0x1D0, "i"},   {0x1D1, "O"},   {0x1D2, "o"},   {0x1D3, "U"},
  {0x1D4, "u"},   {0x1D5, "U"},   {0x1D6, "u"},   {0x1D7, "U"},   {0x1D8, "u"},
  {0x1D9, "U"},   {0x1DA, "u"},   {0x1DB, "U"},   {0x1DC, "u"},   {0x1DE, "A"},
  {0x1DF, "a"},   {0x1E0, "A"},   {0x1E1, "a"},   {0x1E6, "G"},   {0x1E7, "g"},
  {0x1E8, "K"},   {0x1E9, "k"},   {0x1EA, "O"},   {0x1EB, "o"},   {0x1EC, "O"},
  {0x1ED, "o"},   {0x1F0, "j"},   {0x1F1, "DZ"},   {0x1F2, "Dz"},   {0x1F3, "dz"},
  {0x1F4, "G"},   {0x1F5, "g"},   {0x1F8, "N"},   {0x1F9, "n"},   {0x1FA, "A"},
  {0x1FB, "a"},   {0x200, "A"},   {0x201, "a"},   {0x202, "A"},   {0x203, "a"},
  {0x204, "E"},   {0x205, "e"},   {0x206, "E"},   {0x207, "e"},   {0x208, "I"},
  {0x209, "i"},   {0x20A, "I"},   {0x20B, "i"},   {0x20C, "O"},   {0x20D, "o"},
  {0x20E, "O"},   {0x20F, "o"},   {0x210, "R"},   {0x211, "r"},   {0x212, "R"},
  {0x213, "r"},   {0x214, "U"},   {0x215, "u"},   {0x216, "U"},   {0x217, "u"},
  {0x218, "S"},   {0x219, "s"},   {0x21A, "T"},   {0x21B, "t"},   {0x21E, "H"},
  {0x21F, "h"},   {0x226, "A"},   {0x227, "a"},   {0x228, "E"},   {0x229, "e"},
  {0x22A, "O"},   {0x22B, "o"},   {0x22C, "O"},   {0x22D, "o"},   {0x22E, "O"},
  {0x22F, "o"},   {0x230, "O"},   {0x231, "o"},   {0x232, "Y"},   {0x233, "y"},
  {0x2B0, "h"},   {0x2B2, "j"},   {0x2B3, "r"},   {0x2B7, "w"},   {0x2B8, "y"},
  {0x2E1, "l"},   {0x2E2, "s"},   {0x2E3, "x"},   {0x1D2C, "A"},   {0x1D2E, "B"},
  {0x1D30, "D"},   {0x1D31, "E"},   {0x1D33, "G"},   {0x1D34, "H"},   {0x1D35, "I"},
  {0x1D36, "J"},   {0x1D37, "K"},   {0x1D38, "L"},   {0x1D39, "M"},   {0x1D3A, "N"},
  {0x1D3C, "O"},   {0x1D3E, "P"},   {0x1D3F, "R"},   {0x1D40, "T"},   {0x1D41, "U"},
  {0x1D42, "W"},   {0x1D43, "a"},   {0x1D47, "b"},   {0x1D48, "d"},   {0x1D49, "e"},
  {0x1D4D, "g"},   {0x1D4F, "k"},   {0x1D50, "m"},   {0x1D52, "o"},   {0x1D56, "p"},
  {0x1D57, "t"},   {0x1D58, "u"},   {0x1D5B, "v"},   {0x1D62, "i"},   {0x1D63, "r"},
  {0x1D64, "u"},   {0x1D65, "v"},   {0x1D9C, "c"},   {0x1DA0, "f"},   {0x1DBB, "z"},
  {0x1E00, "A"},   {0x1E01, "a"},   {0x1E02, "B"},   {0x1E03, "b"},   {0x1E04, "B"},
  {0x1E05, "b"},   {0x1E06, "B"},   {0x1E07, "b"},   {0x1E08, "C"},   {0x1E09, "c"},
  {0x1E0A, "D"},   {0x1E0B, "d"},   {0x1E0C, "D"},   {0x1E0D, "d"},   {0x1E0E, "D"},
  {0x1E0F, "d"},   {0x1E10, "D"},   {0x1E11, "d"},   {0x1E12, "D"},   {0x1E13, "d"},
  {0x1E14, "E"},   {0x1E15, "e"},   {0x1E16, "E"},   {0x1E17, "e"},   {0x1E18, "E"},
  {0x1E19, "e"},   {0x1E1A, "E"},   {0x1E1B, "e"},   {0x1E1C, "E"},   {0x1E1D, "e"},
  {0x1E1E, "F"},   {0x1E1F, "f"},   {0x1E20, "G"},   {0x1E21, "g"},   {0x1E22, "H"},
  {0x1E23, "h"},   {0x1E24, "H"},   {0x1E25, "h"},   {0x1E26, "H"},   {0x1E27, "h"},
  {0x1E28, "H"},   {0x1E29, "h"},   {0x1E2A, "H"},   {0x1E2B, "h"},   {0x1E2C, "I"},
  {0x1E2D, "i"},   {0x1E2E, "I"},   {0x1E2F, "i"},   {0x1E30, "K"},   {0x1E31, "k"},
  {0x1E32, "K"},   {0x1E33, "k"},   {0x1E34, "K"},   {0x1E35, "k"},   {0x1E36, "L"},
  {0x1E37, "l"},   {0x1E38, "L"},   {0x1E39, "l"},   {0x1E3A, "L"},   {0x1E3B, "l"},
  {0x1E3C, "L"},   {0x1E3D, "l"},   {0x1E3E, "M"},   {0x1E3F, "m"},   {0x1E40, "M"},
  {0x1E41, "m"},   {0x1E42, "M"},   {0x1E43, "m"},   {0x1E44, "N"},   {0x1E45, "n"},
  {0x1E46, "N"},   {0x1E47, "n"},   {0x1E48, "N"},   {0x1E49, "n"},   {0x1E4A, "N"},
  {0x1E4B, "n"},   {0x1E4C, "O"},   {0x1E4D, "o"},   {0x1E4E, "O"},   {0x1E4F, "o"},
  {0x1E50, "O"},   {0x1E51, "o"},   {0x1E52, "O"},   {0x1E53, "o"},   {0x1E54, "P"},
  {0x1E55, "p"},   {0x1E56, "P"},   {0x1E57, "p"},   {0x1E58, "R"},   {0x1E59, "r"},
  {0x1E5A, "R"},   {0x1E5B, "r"},   {0x1E5C, "R"},   {0x1E5D, "r"},   {0x1E5E, "R"},
  {0x1E5F, "r"},   {0x1E60, "S"},   {0x1E61, "s"},   {0x1E62, "S"},   {0x1E63, "s"},
  {0x1E64, "S"},   {0x1E65, "s"},   {0x1E66, "S"},   {0x1E67, "s"},   {0x1E68, "S"},
  {0x1E69, "s"},   {0x1E6A, "T"},   {0x1E6B, "t"},   {0x1E6C, "T"},   {0x1E6D, "t"},
  {0x1E6E, "T"},   {0x1E6F, "t"},   {0x1E70, "T"},   {0x1E71, "t"},   {0x1E72, "U"},
  {0x1E73, "u"},   {0x1E74, "U"},   {0x1E75, "u"},   {0x1E76, "U"},   {0x1E77, "u"},
  {0x1E78, "U"},   {0x1E79, "u"},   {0x1E7A, "U"},   {0x1E7B, "u"},   {0x1E7C, "V"},
  {0x1E7D, "v"},   {0x1E7E, "V"},   {0x1E7F, "v"},   {0x1E80, "W"},   {0x1E81, "w"},
  {0x1E82, "W"},   {0x1E83, "w"},   {0x1E84, "W"},   {0x1E85, "w"},   {0x1E86, "W"},
  {0x1E87, "w"},   {0x1E88, "W"},   {0x1E89, "w"},   {0x1E8A, "X"},   {0x1E8B, "x"},
  {0x1E8C, "X"},   {0x1E8D, "x"},   {0x1E8E, "Y"},   {0x1E8F, "y"},   {0x1E90, "Z"},
  {0x1E91, "z"},   {0x1E92, "Z"},   {0x1E93, "z"},   {0x1E94, "Z"},   {0x1E95, "z"},
  {0x1E96, "h"},   {0x1E97, "t"},   {0x1E98, "w"},   {0x1E99, "y"},   {0x1E9A, "aʾ"},
  {0x1E9B, "s"},   {0x1EA0, "A"},   {0x1EA1, "a"},   {0x1EA2, "A"},   {0x1EA3, "a"},
  {0x1EA4, "A"},   {0x1EA5, "a"},   {0x1EA6, "A"},   {0x1EA7, "a"},   {0x1EA8, "A"},
  {0x1EA9, "a"},   {0x1EAA, "A"},   {0x1EAB, "a"},   {0x1EAC, "A"},   {0x1EAD, "a"},
  {0x1EAE, "A"},   {0x1EAF, "a"},   {0x1EB0, "A"},   {0x1EB1, "a"},   {0x1EB2, "A"},
  {0x1EB3, "a"},   {0x1EB4, "A"},   {0x1EB5, "a"},   {0x1EB6, "A"},   {0x1EB7, "a"},
  {0x1EB8, "E"},   {0x1EB9, "e"},   {0x1EBA, "E"},   {0x1EBB, "e"},   {0x1EBC, "E"},
  {0x1EBD, "e"},   {0x1EBE, "E"},   {0x1EBF, "e"},   {0x1EC0, "E"},   {0x1EC1, "e"},
  {0x1EC2, "E"},   {0x1EC3, "e"},   {0x1EC4, "E"},   {0x1EC5, "e"},   {0x1EC6, "E"},
  {0x1EC7, "e"},   {0x1EC8, "I"},   {0x1EC9, "i"},   {0x1ECA, "I"},   {0x1ECB, "i"},
  {0x1ECC, "O"},   {0x1ECD, "o"},   {0x1ECE, "O"},   {0x1ECF, "o"},   {0x1ED0, "O"},
  {0x1ED1, "o"},   {0x1ED2, "O"},   {0x1ED3, "o"},   {0x1ED4, "O"},   {0x1ED5, "o"},
  {0x1ED6, "O"},   {0x1ED7, "o"},   {0x1ED8, "O"},   {0x1ED9, "o"},   {0x1EDA, "O"},
  {0x1EDB, "o"},   {0x1EDC, "O"},   {0x1EDD, "o"},   {0x1EDE, "O"},   {0x1EDF, "o"},
  {0x1EE0, "O"},   {0x1EE1, "o"},   {0x1EE2, "O"},   {0x1EE3, "o"},   {0x1EE4, "U"},
  {0x1EE5, "u"},   {0x1EE6, "U"},   {0x1EE7, "u"},   {0x1EE8, "U"},   {0x1EE9, "u"},
  {0x1EEA, "U"},   {0x1EEB, "u"},   {0x1EEC, "U"},   {0x1EED, "u"},   {0x1EEE, "U"},
  {0x1EEF, "u"},   {0x1EF0, "U"},   {0x1EF1, "u"},   {0x1EF2, "Y"},   {0x1EF3, "y"},
  {0x1EF4, "Y"},   {0x1EF5, "y"},   {0x1EF6, "Y"},   {0x1EF7, "y"},   {0x1EF8, "Y"},
  {0x1EF9, "y"},   {0x2070, "0"},   {0x2071, "i"},   {0x2074, "4"},   {0x2075, "5"},
  {0x2076, "6"},   {0x2077, "7"},   {0x2078, "8"},   {0x2079, "9"},   {0x207F, "n"},
  {0x2080, "0"},   {0x2081, "1"},   {0x2082, "2"},   {0x2083, "3"},   {0x2084, "4"},
  {0x2085, "5"},   {0x2086, "6"},   {0x2087, "7"},   {0x2088, "8"},   {0x2089, "9"},
  {0x2090, "a"},   {0x2091, "e"},   {0x2092, "o"},   {0x2093, "x"},   {0x2095, "h"},
  {0x2096, "k"},   {0x2097, "l"},   {0x2098, "m"},   {0x2099, "n"},   {0x209A, "p"},
  {0x209B, "s"},   {0x209C, "t"},   {0x20A8, "Rs"},   {0x2100, "a/c"},   {0x2101, "a/s"},
  {0x2102, "C"},   {0x2103, "°C"},   {0x2105, "c/o"},   {0x2106, "c/u"},   {0x2109, "°F"},
  {0x210A, "g"},   {0x210B, "H"},   {0x210C, "H"},   {0x210D, "H"},   {0x210E, "h"},
  {0x2110, "I"},   {0x2111, "I"},   {0x2112, "L"},   {0x2113, "l"},   {0x2115, "N"},
  {0x2116, "No"},   {0x2119, "P"},   {0x211A, "Q"},   {0x211B, "R"},   {0x211C, "R"},
  {0x211D, "R"},   {0x2120, "SM"},   {0x2121, "TEL"},   {0x2122, "TM"},   {0x2124, "Z"},
  {0x2128, "Z"},   {0x212A, "K"},   {0x212B, "A"},   {0x212C, "B"},   {0x212D, "C"},
  {0x212F, "e"},   {0x2130, "E"},   {0x2131, "F"},   {0x2133, "M"},   {0x2134, "o"},
  {0x2139, "i"},   {0x213B, "FAX"},   {0x2145, "D"},   {0x2146, "d"},   {0x2147, "e"},
  {0x2148, "i"},   {0x2149, "j"},   {0x2150, "1⁄7"},   {0x2151, "1⁄9"},   {0x2152, "1⁄10"},
  {0x2153, "1⁄3"},   {0x2154, "2⁄3"},   {0x2155, "1⁄5"},   {0x2156, "2⁄5"},   {0x2157, "3⁄5"},
  {0x2158, "4⁄5"},   {0x2159, "1⁄6"},   {0x215A, "5⁄6"},   {0x215B, "1⁄8"},   {0x215C, "3⁄8"},
  {0x215D, "5⁄8"},   {0x215E, "7⁄8"},   {0x215F, "1⁄"},   {0x2160, "I"},   {0x2161, "II"},
  {0x2162, "III"},   {0x2163, "IV"},   {0x2164, "V"},   {0x2165, "VI"},   {0x2166, "VII"},
  {0x2167, "VIII"},   {0x2168, "IX"},   {0x2169, "X"},   {0x216A, "XI"},   {0x216B, "XII"},
  {0x216C, "L"},   {0x216D, "C"},   {0x216E, "D"},   {0x216F, "M"},   {0x2170, "i"},
  {0x2171, "ii"},   {0x2172, "iii"},   {0x2173, "iv"},   {0x2174, "v"},   {0x2175, "vi"},
  {0x2176, "vii"},   {0x2177, "viii"},   {0x2178, "ix"},   {0x2179, "x"},   {0x217A, "xi"},
  {0x217B, "xii"},   {0x217C, "l"},   {0x217D, "c"},   {0x217E, "d"},   {0x217F, "m"},
  {0x2189, "0⁄3"},   {0x2460, "1"},   {0x2461, "2"},   {0x2462, "3"},   {0x2463, "4"},
  {0x2464, "5"},   {0x2465, "6"},   {0x2466, "7"},   {0x2467, "8"},   {0x2468, "9"},
  {0x2469, "10"},   {0x246A, "11"},   {0x246B, "12"},   {0x246C, "13"},   {0x246D, "14"},
  {0x246E, "15"},   {0x246F, "16"},   {0x2470, "17"},   {0x2471, "18"},   {0x2472, "19"},
  {0x2473, "20"},   {0x2474, "(1)"},   {0x2475, "(2)"},   {0x2476, "(3)"},   {0x2477, "(4)"},
  {0x2478, "(5)"},   {0x2479, "(6)"},   {0x247A, "(7)"},   {0x247B, "(8)"},   {0x247C, "(9)"},
  {0x247D, "(10)"},   {0x247E, "(11)"},   {0x247F, "(12)"},   {0x2480, "(13)"},   {0x2481, "(14)"},
  {0x2482, "(15)"},   {0x2483, "(16)"},   {0x2484, "(17)"},   {0x2485, "(18)"},   {0x2486, "(19)"},
  {0x2487, "(20)"},   {0x2488, "1."},   {0x2489, "2."},   {0x248A, "3."},   {0x248B, "4."},
  {0x248C, "5."},   {0x248D, "6."},   {0x248E, "7."},   {0x248F, "8."},   {0x2490, "9."},
  {0x2491, "10."},   {0x2492, "11."},   {0x2493, "12."},   {0x2494, "13."},   {0x2495, "14."},
  {0x2496, "15."},   {0x2497, "16."},   {0x2498, "17."},   {0x2499, "18."},   {0x249A, "19."},
  {0x249B, "20."},   {0x249C, "(a)"},   {0x249D, "(b)"},   {0x249E, "(c)"},   {0x249F, "(d)"},
  {0x24A0, "(e)"},   {0x24A1, "(f)"},   {0x24A2, "(g)"},   {0x24A3, "(h)"},   {0x24A4, "(i)"},
  {0x24A5, "(j)"},   {0x24A6, "(k)"},   {0x24A7, "(l)"},   {0x24A8, "(m)"},   {0x24A9, "(n)"},
  {0x24AA, "(o)"},   {0x24AB, "(p)"},   {0x24AC, "(q)"},   {0x24AD, "(r)"},   {0x24AE, "(s)"},
  {0x24AF, "(t)"},   {0x24B0, "(u)"},   {0x24B1, "(v)"},   {0x24B2, "(w)"},   {0x24B3, "(x)"},
  {0x24B4, "(y)"},   {0x24B5, "(z)"},   {0x24B6, "A"},   {0x24B7, "B"},   {0x24B8, "C"},
  {0x24B9, "D"},   {0x24BA, "E"},   {0x24BB, "F"},   {0x24BC, "G"},   {0x24BD, "H"},
  {0x24BE, "I"},   {0x24BF, "J"},   {0x24C0, "K"},   {0x24C1, "L"},   {0x24C2, "M"},
  {0x24C3, "N"},   {0x24C4, "O"},   {0x24C5, "P"},   {0x24C6, "Q"},   {0x24C7, "R"},
  {0x24C8, "S"},   {0x24C9, "T"},   {0x24CA, "U"},   {0x24CB, "V"},   {0x24CC, "W"},
  {0x24CD, "X"},   {0x24CE, "Y"},   {0x24CF, "Z"},   {0x24D0, "a"},   {0x24D1, "b"},
  {0x24D2, "c"},   {0x24D3, "d"},   {0x24D4, "e"},   {0x24D5, "f"},   {0x24D6, "g"},
  {0x24D7, "h"},   {0x24D8, "i"},   {0x24D9, "j"},   {0x24DA, "k"},   {0x24DB, "l"},
  {0x24DC, "m"},   {0x24DD, "n"},   {0x24DE, "o"},   {0x24DF, "p"},   {0x24E0, "q"},
  {0x24E1, "r"},   {0x24E2, "s"},   {0x24E3, "t"},   {0x24E4, "u"},   {0x24E5, "v"},
  {0x24E6, "w"},   {0x24E7, "x"},   {0x24E8, "y"},   {0x24E9, "z"},   {0x24EA, "0"},
  {0x2C7C, "j"},   {0x2C7D, "V"},   {0x3250, "PTE"},   {0x3251, "21"},   {0x3252, "22"},
  {0x3253, "23"},   {0x3254, "24"},   {0x3255, "25"},   {0x3256, "26"},   {0x3257, "27"},
  {0x3258, "28"},   {0x3259, "29"},   {0x325A, "30"},   {0x325B, "31"},   {0x325C, "32"},
  {0x325D, "33"},   {0x325E, "34"},   {0x325F, "35"},   {0x32B1, "36"},   {0x32B2, "37"},
  {0x32B3, "38"},   {0x32B4, "39"},   {0x32B5, "40"},   {0x32B6, "41"},   {0x32B7, "42"},
  {0x32B8, "43"},   {0x32B9, "44"},   {0x32BA, "45"},   {0x32BB, "46"},   {0x32BC, "47"},
  {0x32BD, "48"},   {0x32BE, "49"},   {0x32BF, "50"},   {0x32C0, "1月"},   {0x32C1, "2月"},
  {0x32C2, "3月"},   {0x32C3, "4月"},   {0x32C4, "5月"},   {0x32C5, "6月"},   {0x32C6, "7月"},
  {0x32C7, "8月"},   {0x32C8, "9月"},   {0x32C9, "10月"},   {0x32CA, "11月"},   {0x32CB, "12月"},
  {0x32CC, "Hg"},   {0x32CD, "erg"},   {0x32CE, "eV"},   {0x32CF, "LTD"},   {0x3358, "0点"},
  {0x3359, "1点"},   {0x335A, "2点"},   {0x335B, "3点"},   {0x335C, "4点"},   {0x335D, "5点"},
  {0x335E, "6点"},   {0x335F, "7点"},   {0x3360, "8点"},   {0x3361, "9点"},   {0x3362, "10点"},
  {0x3363, "11点"},   {0x3364, "12点"},   {0x3365, "13点"},   {0x3366, "14点"},   {0x3367, "15点"},
  {0x3368, "16点"},   {0x3369, "17点"},   {0x336A, "18点"},   {0x336B, "19点"},   {0x336C, "20点"},
  {0x336D, "21点"},   {0x336E, "22点"},   {0x336F, "23点"},   {0x3370, "24点"},   {0x3371, "hPa"},
  {0x3372, "da"},   {0x3373, "AU"},   {0x3374, "bar"},   {0x3375, "oV"},   {0x3376, "pc"},
  {0x3377, "dm"},   {0x3378, "dm2"},   {0x3379, "dm3"},   {0x337A, "IU"},   {0x3380, "pA"},
  {0x3381, "nA"},   {0x3382, "μA"},   {0x3383, "mA"},   {0x3384, "kA"},   {0x3385, "KB"},
  {0x3386, "MB"},   {0x3387, "GB"},   {0x3388, "cal"},   {0x3389, "kcal"},   {0x338A, "pF"},
  {0x338B, "nF"},   {0x338C, "μF"},   {0x338D, "μg"},   {0x338E, "mg"},   {0x338F, "kg"},
  {0x3390, "Hz"},   {0x3391, "kHz"},   {0x3392, "MHz"},   {0x3393, "GHz"},   {0x3394, "THz"},
  {0x3395, "μl"},   {0x3396, "ml"},   {0x3397, "dl"},   {0x3398, "kl"},   {0x3399, "fm"},
  {0x339A, "nm"},   {0x339B, "μm"},   {0x339C, "mm"},   {0x339D, "cm"},   {0x339E, "km"},
  {0x339F, "mm2"},   {0x33A0, "cm2"},   {0x33A1, "m2"},   {0x33A2, "km2"},   {0x33A3, "mm3"},
  {0x33A4, "cm3"},   {0x33A5, "m3"},   {0x33A6, "km3"},   {0x33A7, "m∕s"},   {0x33A8, "m∕s2"},
  {0x33A9, "Pa"},   {0x33AA, "kPa"},   {0x33AB, "MPa"},   {0x33AC, "GPa"},   {0x33AD, "rad"},
  {0x33AE, "rad∕s"},   {0x33AF, "rad∕s2"},   {0x33B0, "ps"},   {0x33B1, "ns"},   {0x33B2, "μs"},
  {0x33B3, "ms"},   {0x33B4, "pV"},   {0x33B5, "nV"},   {0x33B6, "μV"},   {0x33B7, "mV"},
  {0x33B8, "kV"},   {0x33B9, "MV"},   {0x33BA, "pW"},   {0x33BB, "nW"},   {0x33BC, "μW"},
  {0x33BD, "mW"},   {0x33BE, "kW"},   {0x33BF, "MW"},   {0x33C0, "kΩ"},   {0x33C1, "MΩ"},
  {0x33C2, "a.m."},   {0x33C3, "Bq"},   {0x33C4, "cc"},   {0x33C5, "cd"},   {0x33C6, "C∕kg"},
  {0x33C7, "Co."},   {0x33C8, "dB"},   {0x33C9, "Gy"},   {0x33CA, "ha"},   {0x33CB, "HP"},
  {0x33CC, "in"},   {0x33CD, "KK"},   {0x33CE, "KM"},   {0x33CF, "kt"},   {0x33D0, "lm"},
  {0x33D1, "ln"},   {0x33D2, "log"},   {0x33D3, "lx"},   {0x33D4, "mb"},   {0x33D5, "mil"},
  {0x33D6, "mol"},   {0x33D7, "PH"},   {0x33D8, "p.m."},   {0x33D9, "PPM"},   {0x33DA, "PR"},
  {0x33DB, "sr"},   {0x33DC, "Sv"},   {0x33DD, "Wb"},   {0x33DE, "V∕m"},   {0x33DF, "A∕m"},
  {0x33E0, "1日"},   {0x33E1, "2日"},   {0x33E2, "3日"},   {0x33E3, "4日"},   {0x33E4, "5日"},
  {0x33E5, "6日"},   {0x33E6, "7日"},   {0x33E7, "8日"},   {0x33E8, "9日"},   {0x33E9, "10日"},
  {0x33EA, "11日"},   {0x33EB, "12日"},   {0x33EC, "13日"},   {0x33ED, "14日"},   {0x33EE, "15日"},
  {0x33EF, "16日"},   {0x33F0, "17日"},   {0x33F1, "18日"},   {0x33F2, "19日"},   {0x33F3, "20日"},
  {0x33F4, "21日"},   {0x33F5, "22日"},   {0x33F6, "23日"},   {0x33F7, "24日"},   {0x33F8, "25日"},
  {0x33F9, "26日"},   {0x33FA, "27日"},   {0x33FB, "28日"},   {0x33FC, "29日"},   {0x33FD, "30日"},
  {0x33FE, "31日"},   {0x33FF, "gal"},   {0xA7F1, "S"},   {0xA7F2, "C"},   {0xA7F3, "F"},
  {0xA7F4, "Q"},   {0xFB00, "ff"},   {0xFB01, "fi"},   {0xFB02, "fl"},   {0xFB03, "ffi"},
  {0xFB04, "ffl"},   {0xFB05, "st"},   {0xFB06, "st"},   {0x107A5, "q"},   {0x1CCD6, "A"},
  {0x1CCD7, "B"},   {0x1CCD8, "C"},   {0x1CCD9, "D"},   {0x1CCDA, "E"},   {0x1CCDB, "F"},
  {0x1CCDC, "G"},   {0x1CCDD, "H"},   {0x1CCDE, "I"},   {0x1CCDF, "J"},   {0x1CCE0, "K"},
  {0x1CCE1, "L"},   {0x1CCE2, "M"},   {0x1CCE3, "N"},   {0x1CCE4, "O"},   {0x1CCE5, "P"},
  {0x1CCE6, "Q"},   {0x1CCE7, "R"},   {0x1CCE8, "S"},   {0x1CCE9, "T"},   {0x1CCEA, "U"},
  {0x1CCEB, "V"},   {0x1CCEC, "W"},   {0x1CCED, "X"},   {0x1CCEE, "Y"},   {0x1CCEF, "Z"},
  {0x1CCF0, "0"},   {0x1CCF1, "1"},   {0x1CCF2, "2"},   {0x1CCF3, "3"},   {0x1CCF4, "4"},
  {0x1CCF5, "5"},   {0x1CCF6, "6"},   {0x1CCF7, "7"},   {0x1CCF8, "8"},   {0x1CCF9, "9"},
  {0x1D400, "A"},   {0x1D401, "B"},   {0x1D402, "C"},   {0x1D403, "D"},   {0x1D404, "E"},
  {0x1D405, "F"},   {0x1D406, "G"},   {0x1D407, "H"},   {0x1D408, "I"},   {0x1D409, "J"},
  {0x1D40A, "K"},   {0x1D40B, "L"},   {0x1D40C, "M"},   {0x1D40D, "N"},   {0x1D40E, "O"},
  {0x1D40F, "P"},   {0x1D410, "Q"},   {0x1D411, "R"},   {0x1D412, "S"},   {0x1D413, "T"},
  {0x1D414, "U"},   {0x1D415, "V"},   {0x1D416, "W"},   {0x1D417, "X"},   {0x1D418, "Y"},
  {0x1D419, "Z"},   {0x1D41A, "a"},   {0x1D41B, "b"},   {0x1D41C, "c"},   {0x1D41D, "d"},
  {0x1D41E, "e"},   {0x1D41F, "f"},   {0x1D420, "g"},   {0x1D421, "h"},   {0x1D422, "i"},
  {0x1D423, "j"},   {0x1D424, "k"},   {0x1D425, "l"},   {0x1D426, "m"},   {0x1D427, "n"},
  {0x1D428, "o"},   {0x1D429, "p"},   {0x1D42A, "q"},   {0x1D42B, "r"},   {0x1D42C, "s"},
  {0x1D42D, "t"},   {0x1D42E, "u"},   {0x1D42F, "v"},   {0x1D430, "w"},   {0x1D431, "x"},
  {0x1D432, "y"},   {0x1D433, "z"},   {0x1D434, "A"},   {0x1D435, "B"},   {0x1D436, "C"},
  {0x1D437, "D"},   {0x1D438, "E"},   {0x1D439, "F"},   {0x1D43A, "G"},   {0x1D43B, "H"},
  {0x1D43C, "I"},   {0x1D43D, "J"},   {0x1D43E, "K"},   {0x1D43F, "L"},   {0x1D440, "M"},
  {0x1D441, "N"},   {0x1D442, "O"},   {0x1D443, "P"},   {0x1D444, "Q"},   {0x1D445, "R"},
  {0x1D446, "S"},   {0x1D447, "T"},   {0x1D448, "U"},   {0x1D449, "V"},   {0x1D44A, "W"},
  {0x1D44B, "X"},   {0x1D44C, "Y"},   {0x1D44D, "Z"},   {0x1D44E, "a"},   {0x1D44F, "b"},
  {0x1D450, "c"},   {0x1D451, "d"},   {0x1D452, "e"},   {0x1D453, "f"},   {0x1D454, "g"},
  {0x1D456, "i"},   {0x1D457, "j"},   {0x1D458, "k"},   {0x1D459, "l"},   {0x1D45A, "m"},
  {0x1D45B, "n"},   {0x1D45C, "o"},   {0x1D45D, "p"},   {0x1D45E, "q"},   {0x1D45F, "r"},
  {0x1D460, "s"},   {0x1D461, "t"},   {0x1D462, "u"},   {0x1D463, "v"},   {0x1D464, "w"},
  {0x1D465, "x"},   {0x1D466, "y"},   {0x1D467, "z"},   {0x1D468, "A"},   {0x1D469, "B"},
  {0x1D46A, "C"},   {0x1D46B, "D"},   {0x1D46C, "E"},   {0x1D46D, "F"},   {0x1D46E, "G"},
  {0x1D46F, "H"},   {0x1D470, "I"},   {0x1D471, "J"},   {0x1D472, "K"},   {0x1D473, "L"},
  {0x1D474, "M"},   {0x1D475, "N"},   {0x1D476, "O"},   {0x1D477, "P"},   {0x1D478, "Q"},
  {0x1D479, "R"},   {0x1D47A, "S"},   {0x1D47B, "T"},   {0x1D47C, "U"},   {0x1D47D, "V"},
  {0x1D47E, "W"},   {0x1D47F, "X"},   {0x1D480, "Y"},   {0x1D481, "Z"},   {0x1D482, "a"},
  {0x1D483, "b"},   {0x1D484, "c"},   {0x1D485, "d"},   {0x1D486, "e"},   {0x1D487, "f"},
  {0x1D488, "g"},   {0x1D489, "h"},   {0x1D48A, "i"},   {0x1D48B, "j"},   {0x1D48C, "k"},
  {0x1D48D, "l"},   {0x1D48E, "m"},   {0x1D48F, "n"},   {0x1D490, "o"},   {0x1D491, "p"},
  {0x1D492, "q"},   {0x1D493, "r"},   {0x1D494, "s"},   {0x1D495, "t"},   {0x1D496, "u"},
  {0x1D497, "v"},   {0x1D498, "w"},   {0x1D499, "x"},   {0x1D49A, "y"},   {0x1D49B, "z"},
  {0x1D49C, "A"},   {0x1D49E, "C"},   {0x1D49F, "D"},   {0x1D4A2, "G"},   {0x1D4A5, "J"},
  {0x1D4A6, "K"},   {0x1D4A9, "N"},   {0x1D4AA, "O"},   {0x1D4AB, "P"},   {0x1D4AC, "Q"},
  {0x1D4AE, "S"},   {0x1D4AF, "T"},   {0x1D4B0, "U"},   {0x1D4B1, "V"},   {0x1D4B2, "W"},
  {0x1D4B3, "X"},   {0x1D4B4, "Y"},   {0x1D4B5, "Z"},   {0x1D4B6, "a"},   {0x1D4B7, "b"},
  {0x1D4B8, "c"},   {0x1D4B9, "d"},   {0x1D4BB, "f"},   {0x1D4BD, "h"},   {0x1D4BE, "i"},
  {0x1D4BF, "j"},   {0x1D4C0, "k"},   {0x1D4C1, "l"},   {0x1D4C2, "m"},   {0x1D4C3, "n"},
  {0x1D4C5, "p"},   {0x1D4C6, "q"},   {0x1D4C7, "r"},   {0x1D4C8, "s"},   {0x1D4C9, "t"},
  {0x1D4CA, "u"},   {0x1D4CB, "v"},   {0x1D4CC, "w"},   {0x1D4CD, "x"},   {0x1D4CE, "y"},
  {0x1D4CF, "z"},   {0x1D4D0, "A"},   {0x1D4D1, "B"},   {0x1D4D2, "C"},   {0x1D4D3, "D"},
  {0x1D4D4, "E"},   {0x1D4D5, "F"},   {0x1D4D6, "G"},   {0x1D4D7, "H"},   {0x1D4D8, "I"},
  {0x1D4D9, "J"},   {0x1D4DA, "K"},   {0x1D4DB, "L"},   {0x1D4DC, "M"},   {0x1D4DD, "N"},
  {0x1D4DE, "O"},   {0x1D4DF, "P"},   {0x1D4E0, "Q"},   {0x1D4E1, "R"},   {0x1D4E2, "S"},
  {0x1D4E3, "T"},   {0x1D4E4, "U"},   {0x1D4E5, "V"},   {0x1D4E6, "W"},   {0x1D4E7, "X"},
  {0x1D4E8, "Y"},   {0x1D4E9, "Z"},   {0x1D4EA, "a"},   {0x1D4EB, "b"},   {0x1D4EC, "c"},
  {0x1D4ED, "d"},   {0x1D4EE, "e"},   {0x1D4EF, "f"},   {0x1D4F0, "g"},   {0x1D4F1, "h"},
  {0x1D4F2, "i"},   {0x1D4F3, "j"},   {0x1D4F4, "k"},   {0x1D4F5, "l"},   {0x1D4F6, "m"},
  {0x1D4F7, "n"},   {0x1D4F8, "o"},   {0x1D4F9, "p"},   {0x1D4FA, "q"},   {0x1D4FB, "r"},
  {0x1D4FC, "s"},   {0x1D4FD, "t"},   {0x1D4FE, "u"},   {0x1D4FF, "v"},   {0x1D500, "w"},
  {0x1D501, "x"},   {0x1D502, "y"},   {0x1D503, "z"},   {0x1D504, "A"},   {0x1D505, "B"},
  {0x1D507, "D"},   {0x1D508, "E"},   {0x1D509, "F"},   {0x1D50A, "G"},   {0x1D50D, "J"},
  {0x1D50E, "K"},   {0x1D50F, "L"},   {0x1D510, "M"},   {0x1D511, "N"},   {0x1D512, "O"},
  {0x1D513, "P"},   {0x1D514, "Q"},   {0x1D516, "S"},   {0x1D517, "T"},   {0x1D518, "U"},
  {0x1D519, "V"},   {0x1D51A, "W"},   {0x1D51B, "X"},   {0x1D51C, "Y"},   {0x1D51E, "a"},
  {0x1D51F, "b"},   {0x1D520, "c"},   {0x1D521, "d"},   {0x1D522, "e"},   {0x1D523, "f"},
  {0x1D524, "g"},   {0x1D525, "h"},   {0x1D526, "i"},   {0x1D527, "j"},   {0x1D528, "k"},
  {0x1D529, "l"},   {0x1D52A, "m"},   {0x1D52B, "n"},   {0x1D52C, "o"},   {0x1D52D, "p"},
  {0x1D52E, "q"},   {0x1D52F, "r"},   {0x1D530, "s"},   {0x1D531, "t"},   {0x1D532, "u"},
  {0x1D533, "v"},   {0x1D534, "w"},   {0x1D535, "x"},   {0x1D536, "y"},   {0x1D537, "z"},
  {0x1D538, "A"},   {0x1D539, "B"},   {0x1D53B, "D"},   {0x1D53C, "E"},   {0x1D53D, "F"},
  {0x1D53E, "G"},   {0x1D540, "I"},   {0x1D541, "J"},   {0x1D542, "K"},   {0x1D543, "L"},
  {0x1D544, "M"},   {0x1D546, "O"},   {0x1D54A, "S"},   {0x1D54B, "T"},   {0x1D54C, "U"},
  {0x1D54D, "V"},   {0x1D54E, "W"},   {0x1D54F, "X"},   {0x1D550, "Y"},   {0x1D552, "a"},
  {0x1D553, "b"},   {0x1D554, "c"},   {0x1D555, "d"},   {0x1D556, "e"},   {0x1D557, "f"},
  {0x1D558, "g"},   {0x1D559, "h"},   {0x1D55A, "i"},   {0x1D55B, "j"},   {0x1D55C, "k"},
  {0x1D55D, "l"},   {0x1D55E, "m"},   {0x1D55F, "n"},   {0x1D560, "o"},   {0x1D561, "p"},
  {0x1D562, "q"},   {0x1D563, "r"},   {0x1D564, "s"},   {0x1D565, "t"},   {0x1D566, "u"},
  {0x1D567, "v"},   {0x1D568, "w"},   {0x1D569, "x"},   {0x1D56A, "y"},   {0x1D56B, "z"},
  {0x1D56C, "A"},   {0x1D56D, "B"},   {0x1D56E, "C"},   {0x1D56F, "D"},   {0x1D570, "E"},
  {0x1D571, "F"},   {0x1D572, "G"},   {0x1D573, "H"},   {0x1D574, "I"},   {0x1D575, "J"},
  {0x1D576, "K"},   {0x1D577, "L"},   {0x1D578, "M"},   {0x1D579, "N"},   {0x1D57A, "O"},
  {0x1D57B, "P"},   {0x1D57C, "Q"},   {0x1D57D, "R"},   {0x1D57E, "S"},   {0x1D57F, "T"},
  {0x1D580, "U"},   {0x1D581, "V"},   {0x1D582, "W"},   {0x1D583, "X"},   {0x1D584, "Y"},
  {0x1D585, "Z"},   {0x1D586, "a"},   {0x1D587, "b"},   {0x1D588, "c"},   {0x1D589, "d"},
  {0x1D58A, "e"},   {0x1D58B, "f"},   {0x1D58C, "g"},   {0x1D58D, "h"},   {0x1D58E, "i"},
  {0x1D58F, "j"},   {0x1D590, "k"},   {0x1D591, "l"},   {0x1D592, "m"},   {0x1D593, "n"},
  {0x1D594, "o"},   {0x1D595, "p"},   {0x1D596, "q"},   {0x1D597, "r"},   {0x1D598, "s"},
  {0x1D599, "t"},   {0x1D59A, "u"},   {0x1D59B, "v"},   {0x1D59C, "w"},   {0x1D59D, "x"},
  {0x1D59E, "y"},   {0x1D59F, "z"},   {0x1D5A0, "A"},   {0x1D5A1, "B"},   {0x1D5A2, "C"},
  {0x1D5A3, "D"},   {0x1D5A4, "E"},   {0x1D5A5, "F"},   {0x1D5A6, "G"},   {0x1D5A7, "H"},
  {0x1D5A8, "I"},   {0x1D5A9, "J"},   {0x1D5AA, "K"},   {0x1D5AB, "L"},   {0x1D5AC, "M"},
  {0x1D5AD, "N"},   {0x1D5AE, "O"},   {0x1D5AF, "P"},   {0x1D5B0, "Q"},   {0x1D5B1, "R"},
  {0x1D5B2, "S"},   {0x1D5B3, "T"},   {0x1D5B4, "U"},   {0x1D5B5, "V"},   {0x1D5B6, "W"},
  {0x1D5B7, "X"},   {0x1D5B8, "Y"},   {0x1D5B9, "Z"},   {0x1D5BA, "a"},   {0x1D5BB, "b"},
  {0x1D5BC, "c"},   {0x1D5BD, "d"},   {0x1D5BE, "e"},   {0x1D5BF, "f"},   {0x1D5C0, "g"},
  {0x1D5C1, "h"},   {0x1D5C2, "i"},   {0x1D5C3, "j"},   {0x1D5C4, "k"},   {0x1D5C5, "l"},
  {0x1D5C6, "m"},   {0x1D5C7, "n"},   {0x1D5C8, "o"},   {0x1D5C9, "p"},   {0x1D5CA, "q"},
  {0x1D5CB, "r"},   {0x1D5CC, "s"},   {0x1D5CD, "t"},   {0x1D5CE, "u"},   {0x1D5CF, "v"},
  {0x1D5D0, "w"},   {0x1D5D1, "x"},   {0x1D5D2, "y"},   {0x1D5D3, "z"},   {0x1D5D4, "A"},
  {0x1D5D5, "B"},   {0x1D5D6, "C"},   {0x1D5D7, "D"},   {0x1D5D8, "E"},   {0x1D5D9, "F"},
  {0x1D5DA, "G"},   {0x1D5DB, "H"},   {0x1D5DC, "I"},   {0x1D5DD, "J"},   {0x1D5DE, "K"},
  {0x1D5DF, "L"},   {0x1D5E0, "M"},   {0x1D5E1, "N"},   {0x1D5E2, "O"},   {0x1D5E3, "P"},
  {0x1D5E4, "Q"},   {0x1D5E5, "R"},   {0x1D5E6, "S"},   {0x1D5E7, "T"},   {0x1D5E8, "U"},
  {0x1D5E9, "V"},   {0x1D5EA, "W"},   {0x1D5EB, "X"},   {0x1D5EC, "Y"},   {0x1D5ED, "Z"},
  {0x1D5EE, "a"},   {0x1D5EF, "b"},   {0x1D5F0, "c"},   {0x1D5F1, "d"},   {0x1D5F2, "e"},
  {0x1D5F3, "f"},   {0x1D5F4, "g"},   {0x1D5F5, "h"},   {0x1D5F6, "i"},   {0x1D5F7, "j"},
  {0x1D5F8, "k"},   {0x1D5F9, "l"},   {0x1D5FA, "m"},   {0x1D5FB, "n"},   {0x1D5FC, "o"},
  {0x1D5FD, "p"},   {0x1D5FE, "q"},   {0x1D5FF, "r"},   {0x1D600, "s"},   {0x1D601, "t"},
  {0x1D602, "u"},   {0x1D603, "v"},   {0x1D604, "w"},   {0x1D605, "x"},   {0x1D606, "y"},
  {0x1D607, "z"},   {0x1D608, "A"},   {0x1D609, "B"},   {0x1D60A, "C"},   {0x1D60B, "D"},
  {0x1D60C, "E"},   {0x1D60D, "F"},   {0x1D60E, "G"},   {0x1D60F, "H"},   {0x1D610, "I"},
  {0x1D611, "J"},   {0x1D612, "K"},   {0x1D613, "L"},   {0x1D614, "M"},   {0x1D615, "N"},
  {0x1D616, "O"},   {0x1D617, "P"},   {0x1D618, "Q"},   {0x1D619, "R"},   {0x1D61A, "S"},
  {0x1D61B, "T"},   {0x1D61C, "U"},   {0x1D61D, "V"},   {0x1D61E, "W"},   {0x1D61F, "X"},
  {0x1D620, "Y"},   {0x1D621, "Z"},   {0x1D622, "a"},   {0x1D623, "b"},   {0x1D624, "c"},
  {0x1D625, "d"},   {0x1D626, "e"},   {0x1D627, "f"},   {0x1D628, "g"},   {0x1D629, "h"},
  {0x1D62A, "i"},   {0x1D62B, "j"},   {0x1D62C, "k"},   {0x1D62D, "l"},   {0x1D62E, "m"},
  {0x1D62F, "n"},   {0x1D630, "o"},   {0x1D631, "p"},   {0x1D632, "q"},   {0x1D633, "r"},
  {0x1D634, "s"},   {0x1D635, "t"},   {0x1D636, "u"},   {0x1D637, "v"},   {0x1D638, "w"},
  {0x1D639, "x"},   {0x1D63A, "y"},   {0x1D63B, "z"},   {0x1D63C, "A"},   {0x1D63D, "B"},
  {0x1D63E, "C"},   {0x1D63F, "D"},   {0x1D640, "E"},   {0x1D641, "F"},   {0x1D642, "G"},
  {0x1D643, "H"},   {0x1D644, "I"},   {0x1D645, "J"},   {0x1D646, "K"},   {0x1D647, "L"},
  {0x1D648, "M"},   {0x1D649, "N"},   {0x1D64A, "O"},   {0x1D64B, "P"},   {0x1D64C, "Q"},
  {0x1D64D, "R"},   {0x1D64E, "S"},   {0x1D64F, "T"},   {0x1D650, "U"},   {0x1D651, "V"},
  {0x1D652, "W"},   {0x1D653, "X"},   {0x1D654, "Y"},   {0x1D655, "Z"},   {0x1D656, "a"},
  {0x1D657, "b"},   {0x1D658, "c"},   {0x1D659, "d"},   {0x1D65A, "e"},   {0x1D65B, "f"},
  {0x1D65C, "g"},   {0x1D65D, "h"},   {0x1D65E, "i"},   {0x1D65F, "j"},   {0x1D660, "k"},
  {0x1D661, "l"},   {0x1D662, "m"},   {0x1D663, "n"},   {0x1D664, "o"},   {0x1D665, "p"},
  {0x1D666, "q"},   {0x1D667, "r"},   {0x1D668, "s"},   {0x1D669, "t"},   {0x1D66A, "u"},
  {0x1D66B, "v"},   {0x1D66C, "w"},   {0x1D66D, "x"},   {0x1D66E, "y"},   {0x1D66F, "z"},
  {0x1D670, "A"},   {0x1D671, "B"},   {0x1D672, "C"},   {0x1D673, "D"},   {0x1D674, "E"},
  {0x1D675, "F"},   {0x1D676, "G"},   {0x1D677, "H"},   {0x1D678, "I"},   {0x1D679, "J"},
  {0x1D67A, "K"},   {0x1D67B, "L"},   {0x1D67C, "M"},   {0x1D67D, "N"},   {0x1D67E, "O"},
  {0x1D67F, "P"},   {0x1D680, "Q"},   {0x1D681, "R"},   {0x1D682, "S"},   {0x1D683, "T"},
  {0x1D684, "U"},   {0x1D685, "V"},   {0x1D686, "W"},   {0x1D687, "X"},   {0x1D688, "Y"},
  {0x1D689, "Z"},   {0x1D68A, "a"},   {0x1D68B, "b"},   {0x1D68C, "c"},   {0x1D68D, "d"},
  {0x1D68E, "e"},   {0x1D68F, "f"},   {0x1D690, "g"},   {0x1D691, "h"},   {0x1D692, "i"},
  {0x1D693, "j"},   {0x1D694, "k"},   {0x1D695, "l"},   {0x1D696, "m"},   {0x1D697, "n"},
  {0x1D698, "o"},   {0x1D699, "p"},   {0x1D69A, "q"},   {0x1D69B, "r"},   {0x1D69C, "s"},
  {0x1D69D, "t"},   {0x1D69E, "u"},   {0x1D69F, "v"},   {0x1D6A0, "w"},   {0x1D6A1, "x"},
  {0x1D6A2, "y"},   {0x1D6A3, "z"},   {0x1D7CE, "0"},   {0x1D7CF, "1"},   {0x1D7D0, "2"},
  {0x1D7D1, "3"},   {0x1D7D2, "4"},   {0x1D7D3, "5"},   {0x1D7D4, "6"},   {0x1D7D5, "7"},
  {0x1D7D6, "8"},   {0x1D7D7, "9"},   {0x1D7D8, "0"},   {0x1D7D9, "1"},   {0x1D7DA, "2"},
  {0x1D7DB, "3"},   {0x1D7DC, "4"},   {0x1D7DD, "5"},   {0x1D7DE, "6"},   {0x1D7DF, "7"},
  {0x1D7E0, "8"},   {0x1D7E1, "9"},   {0x1D7E2, "0"},   {0x1D7E3, "1"},   {0x1D7E4, "2"},
  {0x1D7E5, "3"},   {0x1D7E6, "4"},   {0x1D7E7, "5"},   {0x1D7E8, "6"},   {0x1D7E9, "7"},
  {0x1D7EA, "8"},   {0x1D7EB, "9"},   {0x1D7EC, "0"},   {0x1D7ED, "1"},   {0x1D7EE, "2"},
  {0x1D7EF, "3"},   {0x1D7F0, "4"},   {0x1D7F1, "5"},   {0x1D7F2, "6"},   {0x1D7F3, "7"},
  {0x1D7F4, "8"},   {0x1D7F5, "9"},   {0x1D7F6, "0"},   {0x1D7F7, "1"},   {0x1D7F8, "2"},
  {0x1D7F9, "3"},   {0x1D7FA, "4"},   {0x1D7FB, "5"},   {0x1D7FC, "6"},   {0x1D7FD, "7"},
  {0x1D7FE, "8"},   {0x1D7FF, "9"},   {0x1F100, "0."},   {0x1F101, "0,"},   {0x1F102, "1,"},
  {0x1F103, "2,"},   {0x1F104, "3,"},   {0x1F105, "4,"},   {0x1F106, "5,"},   {0x1F107, "6,"},
  {0x1F108, "7,"},   {0x1F109, "8,"},   {0x1F10A, "9,"},   {0x1F110, "(A)"},   {0x1F111, "(B)"},
  {0x1F112, "(C)"},   {0x1F113, "(D)"},   {0x1F114, "(E)"},   {0x1F115, "(F)"},   {0x1F116, "(G)"},
  {0x1F117, "(H)"},   {0x1F118, "(I)"},   {0x1F119, "(J)"},   {0x1F11A, "(K)"},   {0x1F11B, "(L)"},
  {0x1F11C, "(M)"},   {0x1F11D, "(N)"},   {0x1F11E, "(O)"},   {0x1F11F, "(P)"},   {0x1F120, "(Q)"},
  {0x1F121, "(R)"},   {0x1F122, "(S)"},   {0x1F123, "(T)"},   {0x1F124, "(U)"},   {0x1F125, "(V)"},
  {0x1F126, "(W)"},   {0x1F127, "(X)"},   {0x1F128, "(Y)"},   {0x1F129, "(Z)"},   {0x1F12A, "〔S〕"},
  {0x1F12B, "C"},   {0x1F12C, "R"},   {0x1F12D, "CD"},   {0x1F12E, "WZ"},   {0x1F130, "A"},
  {0x1F131, "B"},   {0x1F132, "C"},   {0x1F133, "D"},   {0x1F134, "E"},   {0x1F135, "F"},
  {0x1F136, "G"},   {0x1F137, "H"},   {0x1F138, "I"},   {0x1F139, "J"},   {0x1F13A, "K"},
  {0x1F13B, "L"},   {0x1F13C, "M"},   {0x1F13D, "N"},   {0x1F13E, "O"},   {0x1F13F, "P"},
  {0x1F140, "Q"},   {0x1F141, "R"},   {0x1F142, "S"},   {0x1F143, "T"},   {0x1F144, "U"},
  {0x1F145, "V"},   {0x1F146, "W"},   {0x1F147, "X"},   {0x1F148, "Y"},   {0x1F149, "Z"},
  {0x1F14A, "HV"},   {0x1F14B, "MV"},   {0x1F14C, "SD"},   {0x1F14D, "SS"},   {0x1F14E, "PPV"},
  {0x1F14F, "WC"},   {0x1F16A, "MC"},   {0x1F16B, "MD"},   {0x1F16C, "MR"},   {0x1F190, "DJ"},
  {0x1FBF0, "0"},   {0x1FBF1, "1"},   {0x1FBF2, "2"},   {0x1FBF3, "3"},   {0x1FBF4, "4"},
  {0x1FBF5, "5"},   {0x1FBF6, "6"},   {0x1FBF7, "7"},   {0x1FBF8, "8"},   {0x1FBF9, "9"},

};

static const char *slug_lookup(uint32_t cp) {
  size_t lo = 0, hi = sizeof(SLUG_EXP) / sizeof(SLUG_EXP[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < SLUG_EXP[mid].cp) hi = mid;
    else if (cp > SLUG_EXP[mid].cp) lo = mid + 1;
    else return SLUG_EXP[mid].exp;
  }
  return NULL;
}

/* ----------------------------------------------------------------- slugs */

/* The slug of `text`, written into `scratch` (reset on entry). Returns a
 * view into the scratch. Pure-ASCII input takes the fast scalar path. */
static Slice slug_of(Slice text, Grow *scratch) {
  scratch->len = 0;
  int gap = 0;
  size_t i = 0;
  if (!has_hi(text.p, text.n)) {
    for (; i < text.n; i++) {
      unsigned char c = text.p[i];
      char keep = 0;
      if (c >= 'a' && c <= 'z') keep = (char)c;
      else if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
      else if (c >= '0' && c <= '9') keep = (char)c;
      if (keep == 0) {
        if (scratch->len > 0) gap = 1;
        continue;
      }
      if (scratch->len >= 64) break;
      if (gap) {
        if (scratch->len >= 64) break;
        grow_addc(scratch, '-');
        gap = 0;
      }
      if (scratch->len >= 64) break;
      grow_addc(scratch, (unsigned char)keep);
    }
  } else {
    while (i < text.n) {
      uint32_t cp;
      size_t adv = u8_decode(text.p, text.n, i, &cp);
      if (adv == 0) {
        /* Not valid UTF-8: a gap, like any other non-ASCII byte. */
        if (scratch->len > 0) gap = 1;
        i++;
        continue;
      }
      i += adv;
      if (cp < 0x80) {
        unsigned char c = (unsigned char)cp;
        char keep = 0;
        if (c >= 'a' && c <= 'z') keep = (char)c;
        else if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
        else if (c >= '0' && c <= '9') keep = (char)c;
        if (keep == 0) {
          if (scratch->len > 0) gap = 1;
          continue;
        }
        if (scratch->len >= 64) break;
        if (gap) {
          grow_addc(scratch, '-');
          gap = 0;
        }
        if (scratch->len >= 64) break;
        grow_addc(scratch, (unsigned char)keep);
        continue;
      }
      if (cp_is_mark(cp)) continue; /* dropped, contributes no gap */
      const char *exp = NULL;
      char fw[2];
      if (cp >= 0xFF01 && cp <= 0xFF5E) {
        fw[0] = (char)(cp - 0xFEE0);
        fw[1] = 0;
        exp = fw;
      } else {
        exp = slug_lookup(cp);
      }
      if (!exp) {
        if (scratch->len > 0) gap = 1;
        continue;
      }
      /* Feed the expansion's characters through the same keep/gap logic. */
      size_t elen = strlen(exp);
      size_t ei = 0;
      while (ei < elen) {
        uint32_t ecp;
        size_t ea = u8_decode((const unsigned char *)exp, elen, ei, &ecp);
        if (ea == 0) {
          if (scratch->len > 0) gap = 1;
          ei++;
          continue;
        }
        ei += ea;
        if (ecp >= 0x80) {
          /* A non-ASCII character inside an expansion is a gap. */
          if (scratch->len > 0) gap = 1;
          continue;
        }
        unsigned char c = (unsigned char)ecp;
        char keep = 0;
        if (c >= 'a' && c <= 'z') keep = (char)c;
        else if (c >= 'A' && c <= 'Z') keep = (char)(c + 32);
        else if (c >= '0' && c <= '9') keep = (char)c;
        if (keep == 0) {
          if (scratch->len > 0) gap = 1;
          continue;
        }
        if (scratch->len >= 64) break;
        if (gap) {
          grow_addc(scratch, '-');
          gap = 0;
        }
        if (scratch->len >= 64) break;
        grow_addc(scratch, (unsigned char)keep);
      }
      if (scratch->len >= 64) break;
    }
  }
  /* Cut to 64 characters, a '-' left at the cut dropped. */
  if (scratch->len > 64) scratch->len = 64;
  while (scratch->len > 0 && scratch->data[scratch->len - 1] == '-') scratch->len--;
  if (scratch->len == 0) {
    grow_add_lit(scratch, "untitled");
  }
  return slice_make(scratch->data, scratch->len);
}

/* ------------------------------------------------------------- escaping */

/* Append `v` to `out` with HTML escaping (& < > " '). */
static inline __attribute__((always_inline)) int grow_add_html_esc(Grow *out, Slice v) {
  size_t i = 0;
  while (i < v.n) {
    size_t j = scan_html(v.p + i, v.n - i);
    if (j > 0) {
      if (!grow_addn(out, v.p + i, j)) return 0;
      i += j;
    }
    if (i >= v.n) break;
    unsigned char c = v.p[i++];
    const char *rep = NULL;
    size_t rlen = 0;
    switch (c) {
      case '&': rep = "&amp;"; rlen = 5; break;
      case '<': rep = "&lt;"; rlen = 4; break;
      case '>': rep = "&gt;"; rlen = 4; break;
      case '"': rep = "&quot;"; rlen = 6; break;
      default: rep = "&#39;"; rlen = 5; break;
    }
    if (!grow_addn(out, rep, rlen)) return 0;
  }
  return 1;
}

/* Append `v` to `out` with JSON string escaping (" \ and controls). */
static inline __attribute__((always_inline)) int grow_add_json_esc(Grow *out, Slice v) {
  static const char HEX[] = "0123456789abcdef";
  size_t i = 0;
  while (i < v.n) {
    size_t j = scan_jesc(v.p + i, v.n - i);
    if (j > 0) {
      if (!grow_addn(out, v.p + i, j)) return 0;
      i += j;
    }
    if (i >= v.n) break;
    unsigned char c = v.p[i++];
    char tmp[8];
    size_t tlen = 0;
    switch (c) {
      case '"': tmp[0] = '\\'; tmp[1] = '"'; tlen = 2; break;
      case '\\': tmp[0] = '\\'; tmp[1] = '\\'; tlen = 2; break;
      case '\n': tmp[0] = '\\'; tmp[1] = 'n'; tlen = 2; break;
      case '\r': tmp[0] = '\\'; tmp[1] = 'r'; tlen = 2; break;
      case '\t': tmp[0] = '\\'; tmp[1] = 't'; tlen = 2; break;
      case '\b': tmp[0] = '\\'; tmp[1] = 'b'; tlen = 2; break;
      case '\f': tmp[0] = '\\'; tmp[1] = 'f'; tlen = 2; break;
      default:
        tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
        tmp[4] = HEX[(c >> 4) & 0xF]; tmp[5] = HEX[c & 0xF];
        tlen = 6;
        break;
    }
    if (!grow_addn(out, tmp, tlen)) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------ word count */

/* Whether a code point is JavaScript whitespace (\s: White_Space plus the
 * line terminators). */
static int cp_is_js_space(uint32_t cp) {
  if (cp < 0x80) {
    return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20;
  }
  return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

/* Count the \S+ runs of `v`. Pure-ASCII 16-byte chunks go through SSE2;
 * a chunk holding a byte >= 0x80 falls back to scalar decoding. */
static long count_words(Slice v) {
  long n = 0;
  int in_word = 0;
  size_t i = 0;
  __m128i ws_tab = _mm_set1_epi8(' ');
  __m128i ws_nl = _mm_set1_epi8('\n');
  __m128i ws_cr = _mm_set1_epi8('\r');
  __m128i ws_tb = _mm_set1_epi8('\t');
  __m128i ws_vt = _mm_set1_epi8('\v');
  __m128i ws_ff = _mm_set1_epi8('\f');
  while (i + 16 <= v.n) {
    __m128i chunk = _mm_loadu_si128((const __m128i *)(v.p + i));
    if (_mm_movemask_epi8(chunk) == 0) {
      __m128i m = _mm_cmpeq_epi8(chunk, ws_tab);
      m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, ws_nl));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, ws_cr));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, ws_tb));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, ws_vt));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(chunk, ws_ff));
      unsigned mask = (unsigned)_mm_movemask_epi8(m);
      /* Word starts are non-ws bytes whose predecessor is ws (or, at the
       * chunk start, the previous chunk ended outside a word). Count them
       * with one popcount instead of a 16-step scalar loop. */
      unsigned nw = (~mask) & 0xFFFFu;
      unsigned starts = nw & ~(nw << 1);
      if (in_word) starts &= ~1u;
      n += __builtin_popcount(starts);
      in_word = (int)((nw >> 15) & 1u);
      i += 16;
    } else {
      /* Scalar over this chunk (a non-ASCII byte is inside). */
      size_t end = i + 16;
      while (i < end) {
        uint32_t cp;
        size_t adv = u8_decode(v.p, v.n, i, &cp);
        if (adv == 0) {
          if (!in_word) {
            in_word = 1;
            n++;
          }
          i++;
          continue;
        }
        if (cp_is_js_space(cp)) in_word = 0;
        else if (!in_word) {
          in_word = 1;
          n++;
        }
        i += adv;
      }
    }
  }
  while (i < v.n) {
    uint32_t cp;
    size_t adv = u8_decode(v.p, v.n, i, &cp);
    if (adv == 0) {
      if (!in_word) {
        in_word = 1;
        n++;
      }
      i++;
      continue;
    }
    if (cp_is_js_space(cp)) in_word = 0;
    else if (!in_word) {
      in_word = 1;
      n++;
    }
    i += adv;
  }
  return n;
}

/* --------------------------------------------------------- JSON streaming */

/* Skip JSON whitespace starting at i; returns the first non-ws index. */
static size_t json_ws(const unsigned char *p, size_t n, size_t i) {
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
  return i;
}

/* Skip one JSON string starting at p[i] == '"'. Returns the index just past
 * its closing quote, or SIZE_MAX on malformed input. */
static size_t json_skip_string(const unsigned char *p, size_t n, size_t i) {
  i++; /* opening quote */
  while (i < n) {
    if (n - i < 32) {
      /* Scalar tail: two SIMD broadcasts cost more than a short scan. */
      while (i < n) {
        unsigned char c = p[i];
        if (c == '"') return i + 1;
        if (c == '\\') {
          if (i + 1 < n && p[i + 1] == 'u') i += 6;
          else i += 2;
          continue;
        }
        i++;
      }
      return SIZE_MAX;
    }
    size_t j = scan_two(p + i, n - i, '"', '\\');
    i += j;
    if (i >= n) return SIZE_MAX;
    if (p[i] == '"') return i + 1;
    /* Backslash: \uXXXX is 6 bytes, every other escape 2. */
    if (i + 1 < n && p[i + 1] == 'u') i += 6;
    else i += 2;
  }
  return SIZE_MAX;
}

/* Skip one JSON value starting at i (ws already stripped). Returns the index
 * just past it, or SIZE_MAX on malformed input. */
static size_t json_skip_value(const unsigned char *p, size_t n, size_t i) {
  if (i >= n) return SIZE_MAX;
  unsigned char c = p[i];
  if (c == '"') return json_skip_string(p, n, i);
  if (c == '{' || c == '[') {
    unsigned char open = c;
    unsigned char close = c == '{' ? '}' : ']';
    int depth = 1;
    i++;
    while (i < n && depth > 0) {
      unsigned char d = p[i];
      if (d == '"') {
        i = json_skip_string(p, n, i);
        if (i == SIZE_MAX) return SIZE_MAX;
        continue;
      }
      if (d == open) depth++;
      else if (d == close) depth--;
      i++;
    }
    return depth == 0 ? i : SIZE_MAX;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    i++;
    while (i < n) {
      unsigned char d = p[i];
      if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') i++;
      else break;
    }
    return i;
  }
  if (c == 't') return (i + 4 <= n && memcmp(p + i, "true", 4) == 0) ? i + 4 : SIZE_MAX;
  if (c == 'f') return (i + 5 <= n && memcmp(p + i, "false", 5) == 0) ? i + 5 : SIZE_MAX;
  if (c == 'n') return (i + 4 <= n && memcmp(p + i, "null", 4) == 0) ? i + 4 : SIZE_MAX;
  return SIZE_MAX;
}

/* Find the field named `key` in the JSON object `obj` (including braces).
 * On success returns 1 and fills *vs/*ve with the raw value slice. */
static int json_field(Slice obj, const char *key, Slice *vs, Slice *ve) {
  size_t klen = strlen(key);
  size_t i = json_ws(obj.p, obj.n, 0);
  if (i >= obj.n || obj.p[i] != '{') return 0;
  i = json_ws(obj.p, obj.n, i + 1);
  if (i < obj.n && obj.p[i] == '}') return 0;
  while (i < obj.n) {
    if (obj.p[i] != '"') return 0;
    size_t kend = json_skip_string(obj.p, obj.n, i);
    if (kend == SIZE_MAX) return 0;
    Slice kslice = slice_make(obj.p + i + 1, kend - i - 2);
    int match = 0;
    if (kslice.n == klen && memcmp(kslice.p, key, klen) == 0) match = 1;
    i = json_ws(obj.p, obj.n, kend);
    if (i >= obj.n || obj.p[i] != ':') return 0;
    i = json_ws(obj.p, obj.n, i + 1);
    size_t vend = json_skip_value(obj.p, obj.n, i);
    if (vend == SIZE_MAX) return 0;
    if (match) {
      *vs = slice_make(obj.p + i, vend - i);
      *ve = *vs;
      return 1;
    }
    i = json_ws(obj.p, obj.n, vend);
    if (i < obj.n && obj.p[i] == ',') {
      i = json_ws(obj.p, obj.n, i + 1);
      continue;
    }
    return 0;
  }
  return 0;
}

/* Decode a JSON string value (slice INCLUDING its quotes) into the bump
 * allocator. When the raw content holds no backslash the slice is returned
 * in place, zero-copy. */
static Slice json_unescape(Bump *b, Slice raw) {
  if (raw.n < 2 || raw.p[0] != '"' || raw.p[raw.n - 1] != '"') return slice_make(NULL, 0);
  Slice inner = slice_make(raw.p + 1, raw.n - 2);
  if (find_byte(inner.p, inner.n, '\\') >= inner.n) {
    return inner;
  }
  unsigned char *out = (unsigned char *)bump_alloc(b, inner.n + 1);
  if (!out) return slice_make(NULL, 0);
  size_t o = 0;
  size_t i = 0;
  while (i < inner.n) {
    /* Bulk-copy the run up to the next backslash (memchr + memcpy), then
     * handle the one escape. File contents are mostly plain text with
     * sparse \n escapes, so this avoids a per-byte loop. */
    const unsigned char *bs = (const unsigned char *)memchr(inner.p + i, '\\', inner.n - i);
    size_t run = bs ? (size_t)(bs - (inner.p + i)) : inner.n - i;
    if (run > 0) {
      memcpy(out + o, inner.p + i, run);
      o += run;
      i += run;
    }
    if (i >= inner.n) break;
    if (i + 1 >= inner.n) break;
    unsigned char e = inner.p[i + 1];
    if (e == 'u') {
      if (i + 5 >= inner.n) break;
      uint32_t cp = 0;
      int ok = 1;
      for (int k = 0; k < 4; k++) {
        unsigned char h = inner.p[i + 2 + k];
        cp <<= 4;
        if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
        else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
        else ok = 0;
      }
      if (!ok) break;
      i += 6;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < inner.n && inner.p[i] == '\\' && inner.p[i + 1] == 'u') {
        uint32_t lo = 0;
        int ok2 = 1;
        for (int k = 0; k < 4; k++) {
          unsigned char h = inner.p[i + 2 + k];
          lo <<= 4;
          if (h >= '0' && h <= '9') lo |= (uint32_t)(h - '0');
          else if (h >= 'a' && h <= 'f') lo |= (uint32_t)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') lo |= (uint32_t)(h - 'A' + 10);
          else ok2 = 0;
        }
        if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
      }
      o += u8_encode(cp, out + o);
      continue;
    }
    switch (e) {
      case '"': out[o++] = '"'; break;
      case '\\': out[o++] = '\\'; break;
      case '/': out[o++] = '/'; break;
      case 'b': out[o++] = '\b'; break;
      case 'f': out[o++] = '\f'; break;
      case 'n': out[o++] = '\n'; break;
      case 'r': out[o++] = '\r'; break;
      case 't': out[o++] = '\t'; break;
      default: out[o++] = e; break;
    }
    i += 2;
  }
  return slice_make(out, o);
}

/* Whether the JSON string value `raw` (with quotes) equals the literal. */
static int json_str_eq(Bump *b, Slice raw, const char *lit) {
  Slice d = json_unescape(b, raw);
  size_t ll = strlen(lit);
  return d.n == ll && (ll == 0 || memcmp(d.p, lit, ll) == 0);
}

/* ------------------------------------------------------------ front matter */

typedef struct {
  Slice title;
  Slice date;
  Slice summary;
  Slice layout;
  Slice slug;
  Slice *tags;
  size_t ntags;
  int has_title;
  int has_date;
  int has_summary;
  int has_layout;
  int has_slug;
  int has_tags;
  int draft; /* 1 when draft: true */
  int has_draft;
  int error; /* 1 when refused */
  Slice err; /* the refusal reason, arena-owned */
} FrontMatter;

static int fm_days_in(int y, int m) {
  if (m == 2) return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28;
  if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
  return 31;
}

/* Parse one quoted-or-bare string value (frontmatter's stringOf). On
 * success returns 1 and fills *out with a bump-owned slice; on failure
 * returns 0 and fills *err. */
static int fm_string_of(Bump *b, Slice raw, Slice *out, const char **err) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      *err = "a quoted string is not closed";
      return 0;
    }
    /* The decoded text is at most raw.n bytes; write it straight into the
     * bump allocator. Only \" and \\ are escapes here. */
    unsigned char *dst = (unsigned char *)bump_alloc(b, raw.n == 0 ? 1 : raw.n);
    if (!dst) {
      *err = "a quoted string is not closed";
      return 0;
    }
    size_t o = 0;
    for (size_t i = 1; i + 1 < raw.n; i++) {
      unsigned char c = raw.p[i];
      if (c == '\\') {
        if (i + 1 >= raw.n - 1) {
          *err = "a quoted string holds an escape other than \\\" or \\\\";
          return 0;
        }
        unsigned char nx = raw.p[i + 1];
        if (nx != '"' && nx != '\\') {
          *err = "a quoted string holds an escape other than \\\" or \\\\";
          return 0;
        }
        dst[o++] = nx;
        i++;
      } else if (c == '"') {
        *err = "a quoted string holds a stray quote";
        return 0;
      } else {
        dst[o++] = c;
      }
    }
    *out = slice_make(dst, o);
    return 1;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    *err = "a bare string opens on [";
    return 0;
  }
  *out = raw;
  return 1;
}

/* Whether a tag name is valid: no " [ or ]. */
static int fm_tag_ok(Slice t) {
  if (t.n == 0) return 0;
  for (size_t i = 0; i < t.n; i++) {
    if (t.p[i] == '"' || t.p[i] == '[' || t.p[i] == ']') return 0;
  }
  return 1;
}

/* JS .trim() on a slice: drop JS whitespace at both ends. */
static Slice js_trim(Slice s) {
  size_t a = 0, b = s.n;
  while (a < b) {
    uint32_t cp;
    size_t adv = u8_decode(s.p, s.n, a, &cp);
    if (adv == 0) break;
    if (!cp_is_js_space(cp)) break;
    a += adv;
  }
  while (b > a) {
    /* Walk back one code point. */
    size_t k = b - 1;
    while (k > a && (s.p[k] & 0xC0) == 0x80) k--;
    uint32_t cp;
    size_t adv = u8_decode(s.p, s.n, k, &cp);
    if (adv == 0 || k + adv != b) break;
    if (!cp_is_js_space(cp)) break;
    b = k;
  }
  return slice_make(s.p + a, b - a);
}

static int fm_key_known(Slice k, const char **known) {
  for (int i = 0; known[i]; i++) {
    if (k.n == strlen(known[i]) && memcmp(k.p, known[i], k.n) == 0) return 1;
  }
  return 0;
}

/* Parse the front matter of `text` (already CRLF-normalised). Fills fm and
 * returns the body slice (everything after the closing --- line) on
 * success; on refusal returns 0 and fm->err holds the reason. */
static int fm_parse(Slice text, Bump *b, FrontMatter *fm, Slice *body) {
  static const char *KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary", NULL};
  memset(fm, 0, sizeof(*fm));
  /* Split into lines without copying. */
  size_t line0_end = find_byte(text.p, text.n, '\n');
  Slice first = slice_make(text.p, line0_end);
  if (first.n != 3 || memcmp(first.p, "---", 3) != 0) {
    fm->error = 1;
    fm->err = slice_make((const unsigned char *)"no front matter", 15);
    return 0;
  }
  /* Find the closing --- line. */
  size_t pos = line0_end + 1; /* start of line 2 (index 1) */
  size_t line_idx = 1;        /* 0-based index of the line starting at pos */
  size_t close_idx = SIZE_MAX;
  size_t close_start = 0, close_end = 0;
  while (pos <= text.n) {
    size_t e = find_byte(text.p + pos, text.n - pos, '\n');
    size_t lend = pos + e;
    Slice ln = slice_make(text.p + pos, lend - pos);
    if (ln.n == 3 && memcmp(ln.p, "---", 3) == 0) {
      close_idx = line_idx;
      close_start = pos;
      close_end = lend;
      break;
    }
    if (e >= text.n - pos) break;
    pos = lend + 1;
    line_idx++;
  }
  if (close_idx == SIZE_MAX) {
    fm->error = 1;
    fm->err = slice_make((const unsigned char *)"the front matter is not closed", 30);
    return 0;
  }
  /* Walk the front-matter lines (indices 1 .. close_idx-1). */
  size_t p2 = line0_end + 1;
  for (size_t li = 1; li < close_idx; li++) {
    size_t e = find_byte(text.p + p2, text.n - p2, '\n');
    size_t lend = p2 + e;
    Slice line = slice_make(text.p + p2, lend - p2);
    p2 = lend + 1;
    /* Blank (spaces/tabs only) or a comment: skip. */
    int blank = 1;
    for (size_t k = 0; k < line.n; k++) {
      if (line.p[k] != ' ' && line.p[k] != '\t') { blank = 0; break; }
    }
    if (blank) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    /* LINE = /^([a-z_]+):(?: (.*))?$/ */
    size_t ki = 0;
    while (ki < line.n && ((line.p[ki] >= 'a' && line.p[ki] <= 'z') || line.p[ki] == '_')) ki++;
    if (ki == 0 || ki >= line.n || line.p[ki] != ':') {
      fm->error = 1;
      char *msg = (char *)bump_alloc(b, 64);
      int ml = snprintf(msg, 64, "line %zu is not \"key: value\"", li + 1);
      fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
      return 0;
    }
    Slice key = slice_make(line.p, ki);
    Slice raw;
    if (ki + 1 >= line.n) {
      raw = slice_make(line.p + line.n, 0);
    } else if (line.p[ki + 1] == ' ') {
      raw = slice_make(line.p + ki + 2, line.n - (ki + 2));
    } else {
      /* ':' not followed by a space: the regex's optional group is absent,
       * so raw is empty -- but only if the rest is empty; otherwise the
       * line cannot match at all. */
      fm->error = 1;
      char *msg = (char *)bump_alloc(b, 64);
      int ml = snprintf(msg, 64, "line %zu is not \"key: value\"", li + 1);
      fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
      return 0;
    }
    /* Strip trailing spaces/tabs from raw. */
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    if (!fm_key_known(key, KEYS)) {
      fm->error = 1;
      char *msg = (char *)bump_alloc(b, 128);
      int ml = snprintf(msg, 128, "unknown key \"%.*s\"", (int)key.n, key.p);
      fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
      return 0;
    }
    int dup = 0;
    if (slice_eq(key, slice_make((const unsigned char *)"title", 5))) dup = fm->has_title;
    else if (slice_eq(key, slice_make((const unsigned char *)"date", 4))) dup = fm->has_date;
    else if (slice_eq(key, slice_make((const unsigned char *)"tags", 4))) dup = fm->has_tags;
    else if (slice_eq(key, slice_make((const unsigned char *)"draft", 5))) dup = fm->has_draft;
    else if (slice_eq(key, slice_make((const unsigned char *)"layout", 6))) dup = fm->has_layout;
    else if (slice_eq(key, slice_make((const unsigned char *)"slug", 4))) dup = fm->has_slug;
    else if (slice_eq(key, slice_make((const unsigned char *)"summary", 7))) dup = fm->has_summary;
    if (dup) {
      fm->error = 1;
      char *msg = (char *)bump_alloc(b, 128);
      int ml = snprintf(msg, 128, "key \"%.*s\" is given twice", (int)key.n, key.p);
      fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
      return 0;
    }
    if (raw.n == 0) {
      fm->error = 1;
      char *msg = (char *)bump_alloc(b, 128);
      int ml = snprintf(msg, 128, "key \"%.*s\" has no value", (int)key.n, key.p);
      fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
      return 0;
    }
    if (slice_eq(key, slice_make((const unsigned char *)"title", 5)) ||
        slice_eq(key, slice_make((const unsigned char *)"summary", 7))) {
      const char *err = NULL;
      Slice v;
      if (!fm_string_of(b, raw, &v, &err)) {
        fm->error = 1;
        char *msg = (char *)bump_alloc(b, 256);
        int ml = snprintf(msg, 256, "key \"%.*s\": %s", (int)key.n, key.p, err);
        fm->err = slice_make((const unsigned char *)msg, (size_t)ml);
        return 0;
      }
      if (slice_eq(key, slice_make((const unsigned char *)"title", 5))) {
        fm->title = v;
        fm->has_title = 1;
      } else {
        fm->summary = v;
        fm->has_summary = 1;
      }
    } else if (slice_eq(key, slice_make((const unsigned char *)"date", 4))) {
      int ok = 0;
      if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
        int dig = 1;
        for (int k = 0; k < 10; k++) {
          if (k == 4 || k == 7) continue;
          if (raw.p[k] < '0' || raw.p[k] > '9') dig = 0;
        }
        if (dig) {
          int y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
          int mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
          int d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
          if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= fm_days_in(y, mo)) ok = 1;
        }
      }
      if (!ok) {
        fm->error = 1;
        fm->err = SLIT("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
        return 0;
      }
      fm->date = raw;
      fm->has_date = 1;
    } else if (slice_eq(key, slice_make((const unsigned char *)"tags", 4))) {
      if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
        fm->error = 1;
        fm->err = SLIT("key \"tags\": not a list [a, b]");
        return 0;
      }
      Slice inner = slice_make(raw.p + 1, raw.n - 2);
      Slice t = js_trim(inner);
      if (t.n == 0) {
        fm->tags = NULL;
        fm->ntags = 0;
        fm->has_tags = 1;
      } else {
        /* Split on commas; each part trimmed. */
        size_t cap = 4, cnt = 0;
        Slice *arr = (Slice *)bump_alloc(b, sizeof(Slice) * cap);
        size_t start = 0;
        int bad = 0;
        for (size_t k = 0; k <= inner.n; k++) {
          if (k == inner.n || inner.p[k] == ',') {
            Slice part = js_trim(slice_make(inner.p + start, k - start));
            if (!fm_tag_ok(part)) { bad = 1; break; }
            if (cnt == cap) {
              cap *= 2;
              Slice *na = (Slice *)bump_alloc(b, sizeof(Slice) * cap);
              memcpy(na, arr, sizeof(Slice) * cnt);
              arr = na;
            }
            arr[cnt++] = part;
            start = k + 1;
          }
        }
        if (bad) {
          fm->error = 1;
          fm->err = SLIT("key \"tags\": an empty tag, or one holding \" [ or ]");
          return 0;
        }
        fm->tags = arr;
        fm->ntags = cnt;
        fm->has_tags = 1;
      }
    } else if (slice_eq(key, slice_make((const unsigned char *)"draft", 5))) {
      if (slice_eq(raw, slice_make((const unsigned char *)"true", 4))) fm->draft = 1;
      else if (slice_eq(raw, slice_make((const unsigned char *)"false", 5))) fm->draft = 0;
      else {
        fm->error = 1;
        fm->err = SLIT("key \"draft\": true or false");
        return 0;
      }
      fm->has_draft = 1;
    } else if (slice_eq(key, slice_make((const unsigned char *)"layout", 6))) {
      int ok = raw.n > 0;
      for (size_t k = 0; k < raw.n; k++) {
        unsigned char c = raw.p[k];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
      }
      if (!ok) {
        fm->error = 1;
        fm->err = SLIT("key \"layout\": not a template name");
        return 0;
      }
      fm->layout = raw;
      fm->has_layout = 1;
    } else if (slice_eq(key, slice_make((const unsigned char *)"slug", 4))) {
      int ok = raw.n > 0 && raw.n <= 64;
      if (ok) {
        /* /^[a-z0-9]+(?:-[a-z0-9]+)*$/ */
        int state = 0; /* 0: need alnum, 1: in run, 2: after '-' */
        for (size_t k = 0; k < raw.n; k++) {
          unsigned char c = raw.p[k];
          int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
          if (state == 0) {
            if (!alnum) { ok = 0; break; }
            state = 1;
          } else if (state == 1) {
            if (c == '-') state = 2;
            else if (!alnum) { ok = 0; break; }
          } else {
            if (!alnum) { ok = 0; break; }
            state = 1;
          }
        }
        if (state == 2) ok = 0;
      }
      if (!ok) {
        fm->error = 1;
        fm->err = SLIT("key \"slug\": not a slug");
        return 0;
      }
      fm->slug = raw;
      fm->has_slug = 1;
    }
  }
  if (!fm->has_title) {
    fm->error = 1;
    fm->err = slice_make((const unsigned char *)"no title", 8);
    return 0;
  }
  size_t body_start = close_end + 1;
  if (body_start > text.n) body_start = text.n;
  *body = slice_make(text.p + body_start, text.n - body_start);
  return 1;
}

/* ------------------------------------------------- string sets and maps */

/* An open-addressing set of slices. The bytes a slice points at must outlive
 * the set (input buffer or bump memory). */
typedef struct {
  Slice *keys;
  size_t cap;
  size_t n;
} StrSet;

static int strset_grow(StrSet *s, Bump *b) {
  size_t nc = s->cap == 0 ? 64 : s->cap * 2;
  Slice *nk = (Slice *)bump_alloc(b, sizeof(Slice) * nc);
  if (!nk) return 0;
  for (size_t i = 0; i < nc; i++) nk[i] = slice_make(NULL, 0);
  for (size_t i = 0; i < s->cap; i++) {
    if (s->keys[i].p == NULL) continue;
    size_t h = (size_t)slice_hash(s->keys[i]) & (nc - 1);
    while (nk[h].p != NULL) h = (h + 1) & (nc - 1);
    nk[h] = s->keys[i];
  }
  s->keys = nk;
  s->cap = nc;
  return 1;
}

/* Adds k; returns 1 when it was not already present. */
static int strset_add(StrSet *s, Slice k, Bump *b) {
  if (s->cap == 0 || (s->n + 1) * 10 > s->cap * 7) {
    if (!strset_grow(s, b)) return 0;
  }
  size_t h = (size_t)slice_hash(k) & (s->cap - 1);
  while (s->keys[h].p != NULL) {
    if (slice_eq(s->keys[h], k)) return 0;
    h = (h + 1) & (s->cap - 1);
  }
  s->keys[h] = k;
  s->n++;
  return 1;
}

static int strset_has(StrSet *s, Slice k) {
  if (s->cap == 0) return 0;
  size_t h = (size_t)slice_hash(k) & (s->cap - 1);
  while (s->keys[h].p != NULL) {
    if (slice_eq(s->keys[h], k)) return 1;
    h = (h + 1) & (s->cap - 1);
  }
  return 0;
}

typedef struct {
  Slice *keys;
  void **vals;
  size_t cap;
  size_t n;
} StrMap;

static int strmap_grow(StrMap *m, Bump *b) {
  size_t nc = m->cap == 0 ? 64 : m->cap * 2;
  Slice *nk = (Slice *)bump_alloc(b, sizeof(Slice) * nc);
  void **nv = (void **)bump_alloc(b, sizeof(void *) * nc);
  if (!nk || !nv) return 0;
  for (size_t i = 0; i < nc; i++) nk[i] = slice_make(NULL, 0);
  for (size_t i = 0; i < m->cap; i++) {
    if (m->keys[i].p == NULL) continue;
    size_t h = (size_t)slice_hash(m->keys[i]) & (nc - 1);
    while (nk[h].p != NULL) h = (h + 1) & (nc - 1);
    nk[h] = m->keys[i];
    nv[h] = m->vals[i];
  }
  m->keys = nk;
  m->vals = nv;
  m->cap = nc;
  return 1;
}

static void *strmap_get(StrMap *m, Slice k) {
  if (m->cap == 0) return NULL;
  size_t h = (size_t)slice_hash(k) & (m->cap - 1);
  while (m->keys[h].p != NULL) {
    if (slice_eq(m->keys[h], k)) return m->vals[h];
    h = (h + 1) & (m->cap - 1);
  }
  return NULL;
}

static int strmap_put(StrMap *m, Slice k, void *v, Bump *b) {
  if (m->cap == 0 || (m->n + 1) * 10 > m->cap * 7) {
    if (!strmap_grow(m, b)) return 0;
  }
  size_t h = (size_t)slice_hash(k) & (m->cap - 1);
  while (m->keys[h].p != NULL) {
    if (slice_eq(m->keys[h], k)) {
      m->vals[h] = v;
      return 1;
    }
    h = (h + 1) & (m->cap - 1);
  }
  m->keys[h] = k;
  m->vals[h] = v;
  m->n++;
  return 1;
}

/* ------------------------------------------------------------ value model */

/* A template value: null, bool, integer, string, array or record. Records
 * hold fields by interned field id. */
#define VAL_NULL 0
#define VAL_BOOL 1
#define VAL_NUM 2
#define VAL_STR 3
#define VAL_ARR 4
#define VAL_REC 5

typedef struct Val Val;
struct Val {
  uint8_t kind;
  union {
    int b;
    int64_t num;
    Slice str;
    struct {
      Val **items;
      size_t n;
    } arr;
    struct {
      int *ids;
      Val **vals;
      size_t n;
      size_t cap;
    } rec;
  } u;
};

static Val *val_new(Bump *b, uint8_t kind) {
  Val *v = (Val *)bump_alloc(b, sizeof(Val));
  if (!v) return NULL;
  memset(v, 0, sizeof(*v));
  v->kind = kind;
  return v;
}

static Val *val_bool(Bump *b, int x) {
  Val *v = val_new(b, VAL_BOOL);
  if (v) v->u.b = x ? 1 : 0;
  return v;
}

static Val *val_num(Bump *b, int64_t x) {
  Val *v = val_new(b, VAL_NUM);
  if (v) v->u.num = x;
  return v;
}

static Val *val_str(Bump *b, Slice s) {
  Val *v = val_new(b, VAL_STR);
  if (v) v->u.str = s;
  return v;
}

static Val *val_arr(Bump *b, Val **items, size_t n) {
  Val *v = val_new(b, VAL_ARR);
  if (v) {
    v->u.arr.items = items;
    v->u.arr.n = n;
  }
  return v;
}

static Val *val_rec(Bump *b) { return val_new(b, VAL_REC); }

/* Set (or replace) a record field. */
static void val_rec_set(Bump *b, Val *rec, int id, Val *v) {
  for (size_t i = 0; i < rec->u.rec.n; i++) {
    if (rec->u.rec.ids[i] == id) {
      rec->u.rec.vals[i] = v;
      return;
    }
  }
  if (rec->u.rec.n == rec->u.rec.cap) {
    size_t nc = rec->u.rec.cap == 0 ? 4 : rec->u.rec.cap * 2;
    int *ni = (int *)bump_alloc(b, sizeof(int) * nc);
    Val **nv = (Val **)bump_alloc(b, sizeof(Val *) * nc);
    if (!ni || !nv) return;
    memcpy(ni, rec->u.rec.ids, sizeof(int) * rec->u.rec.n);
    memcpy(nv, rec->u.rec.vals, sizeof(Val *) * rec->u.rec.n);
    rec->u.rec.ids = ni;
    rec->u.rec.vals = nv;
    rec->u.rec.cap = nc;
  }
  rec->u.rec.ids[rec->u.rec.n] = id;
  rec->u.rec.vals[rec->u.rec.n] = v;
  rec->u.rec.n++;
}

/* The value of a record field, or NULL when absent. */
static Val *val_rec_get(Val *rec, int id) {
  for (size_t i = 0; i < rec->u.rec.n; i++) {
    if (rec->u.rec.ids[i] == id) return rec->u.rec.vals[i];
  }
  return NULL;
}

/* ---------------------------------------------------------- field interning */

enum {
  F_site, F_tags, F_recent, F_page, F_posts, F_pager, F_tag, F_loop,
  F_title, F_url, F_date, F_date_long, F_summary, F_excerpt,
  F_reading_minutes, F_content, F_toc, F_slug, F_newer, F_older,
  F_is_post, F_name, F_count, F_number, F_total, F_newer_url,
  F_older_url, F_level, F_id, F_html, F_index, F_first, F_last,
  F_length, F_base_url,
  F_KNOWN_COUNT
};

static const char *KNOWN_NAMES[] = {
  "site", "tags", "recent", "page", "posts", "pager", "tag", "loop",
  "title", "url", "date", "date_long", "summary", "excerpt",
  "reading_minutes", "content", "toc", "slug", "newer", "older",
  "is_post", "name", "count", "number", "total", "newer_url",
  "older_url", "level", "id", "html", "index", "first", "last",
  "length", "base_url",
};

typedef struct {
  Slice *names;
  size_t n;
  size_t cap;
} Intern;

static void intern_init(Intern *in, Bump *b) {
  in->cap = 128;
  in->names = (Slice *)bump_alloc(b, sizeof(Slice) * in->cap);
  in->n = 0;
  for (int i = 0; i < F_KNOWN_COUNT; i++) {
    in->names[in->n++] = slice_make((const unsigned char *)KNOWN_NAMES[i], strlen(KNOWN_NAMES[i]));
  }
}

/* The id of a name, interning it when new. */
static int intern_id(Intern *in, Slice name, Bump *b) {
  for (size_t i = 0; i < in->n; i++) {
    if (slice_eq(in->names[i], name)) return (int)i;
  }
  if (in->n == in->cap) {
    size_t nc = in->cap * 2;
    Slice *nn = (Slice *)bump_alloc(b, sizeof(Slice) * nc);
    memcpy(nn, in->names, sizeof(Slice) * in->n);
    in->names = nn;
    in->cap = nc;
  }
  in->names[in->n] = bump_dup(b, name);
  return (int)(in->n++);
}

/* ------------------------------------------------------------------ scopes */

typedef struct Scope {
  struct Scope *parent;
  int n;
  int ids[8];
  Val *vals[8];
} Scope;

/* A record property: a record's field, or an array's `length`. */
static Val *val_prop(Bump *b, Val *v, int id) {
  if (!v) return NULL;
  if (v->kind == VAL_REC) return val_rec_get(v, id);
  if (v->kind == VAL_ARR && id == F_length) return val_num(b, (int64_t)v->u.arr.n);
  return NULL;
}

/* The value at `path` (interned ids) in the scope chain, or NULL. */
static Val *scope_lookup(Bump *b, Scope *s, const int *path, size_t plen) {
  Scope *cur = s;
  Val *v = NULL;
  while (cur != NULL) {
    for (int i = 0; i < cur->n; i++) {
      if (cur->ids[i] == path[0]) {
        v = cur->vals[i];
        goto found;
      }
    }
    cur = cur->parent;
  }
  return NULL;
found:
  for (size_t k = 1; k < plen; k++) {
    v = val_prop(b, v, path[k]);
    if (!v) return NULL;
  }
  return v;
}

/* Append a value as text (textOf), HTML-escaping strings when esc is set. */
static int val_text(Grow *out, Bump *b, Val *v, int esc) {
  if (!v) return 1;
  switch (v->kind) {
    case VAL_STR:
      return esc ? grow_add_html_esc(out, v->u.str) : grow_add_slice(out, v->u.str);
    case VAL_NUM: {
      char tmp[32];
      int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.num);
      return grow_addn(out, tmp, (size_t)n);
    }
    case VAL_BOOL:
      return grow_add_lit(out, v->u.b ? "true" : "false");
    case VAL_ARR:
      for (size_t i = 0; i < v->u.arr.n; i++) {
        if (i > 0 && !grow_add_lit(out, ", ")) return 0;
        if (!val_text(out, b, v->u.arr.items[i], esc)) return 0;
      }
      return 1;
    default:
      return 1;
  }
}

/* Whether a value counts as true in an if. */
static int val_truthy(Val *v) {
  if (!v) return 0;
  switch (v->kind) {
    case VAL_NULL: return 0;
    case VAL_ARR: return v->u.arr.n > 0;
    case VAL_BOOL: return v->u.b;
    case VAL_STR: return v->u.str.n > 0;
    case VAL_NUM: return v->u.num != 0;
    default: return 1; /* a record is truthy */
  }
}

/* -------------------------------------------------------------- markdown */

typedef struct {
  Slice (*link_fn)(void *ud, Slice target, Grow *scratch);
  void *link_ud;
  StrSet *ids; /* heading ids already used on this page */
  Grow *slug_scratch;
  Grow *link_scratch;
} MdCtx;

/* `slug` made unique against `taken` (and added to it): the first of
 * slug, slug-2, slug-3 ... not taken. The result is bump-owned, and so is
 * the key stored in the set: a candidate built in the scratch buffer must
 * never be stored as-is, because the scratch is reused and the set's key
 * would then read as whatever the scratch holds next. */
static Slice unique_slug(Slice slug, StrSet *taken, Bump *b, Grow *scratch) {
  Slice base = bump_dup(b, slug);
  if (strset_add(taken, base, b)) return base;
  for (int n = 2;; n++) {
    scratch->len = 0;
    grow_add_slice(scratch, base);
    grow_addc(scratch, '-');
    char tmp[24];
    int tl = snprintf(tmp, sizeof(tmp), "%d", n);
    grow_addn(scratch, tmp, (size_t)tl);
    Slice cand = bump_dup(b, slice_make(scratch->data, scratch->len));
    if (strset_add(taken, cand, b)) return cand;
  }
}

static int line_blank(Slice l) {
  for (size_t i = 0; i < l.n; i++) {
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  }
  return 1;
}

static int line_is_lang_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}

/* Fence open: ``` + lang chars + spaces/tabs to the end. */
static int line_fence_open(Slice l, Slice *lang) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < l.n && line_is_lang_char(l.p[i])) i++;
  *lang = slice_make(l.p + 3, i - 3);
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

/* Fence close: ``` + spaces/tabs to the end. */
static int line_fence_close(Slice l) {
  if (l.n < 3 || memcmp(l.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

/* ATX heading with non-empty text: depth and text (trailing spaces/tabs
 * stripped). */
static int line_heading(Slice l, int *depth, Slice *text) {
  size_t h = 0;
  while (h < l.n && l.p[h] == '#') h++;
  if (h < 1 || h > 6) return 0;
  if (h >= l.n || (l.p[h] != ' ' && l.p[h] != '\t')) return 0;
  size_t i = h;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  size_t end = l.n;
  while (end > i && (l.p[end - 1] == ' ' || l.p[end - 1] == '\t')) end--;
  if (end == i) return 0;
  *depth = (int)h;
  *text = slice_make(l.p + i, end - i);
  return 1;
}

static int line_hr(Slice l) {
  if (l.n < 3) return 0;
  int dash = memcmp(l.p, "---", 3) == 0;
  int star = memcmp(l.p, "***", 3) == 0;
  if (!dash && !star) return 0;
  size_t i = 3;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

static int line_bullet(Slice l, Slice *item) {
  if (l.n < 2) return 0;
  if ((l.p[0] != '-' && l.p[0] != '*') || l.p[1] != ' ') return 0;
  *item = slice_make(l.p + 2, l.n - 2);
  return 1;
}

static int line_ordered(Slice l, int64_t *num, Slice *item) {
  size_t i = 0;
  while (i < l.n && l.p[i] >= '0' && l.p[i] <= '9') i++;
  if (i < 1 || i > 9) return 0;
  if (i + 1 >= l.n || l.p[i] != '.' || l.p[i + 1] != ' ') return 0;
  int64_t v = 0;
  for (size_t k = 0; k < i; k++) v = v * 10 + (l.p[k] - '0');
  *num = v;
  *item = slice_make(l.p + i + 2, l.n - (i + 2));
  return 1;
}

/* A continued list item: two or more spaces, then a non-whitespace char. */
static int line_continued(Slice l) {
  size_t i = 0;
  while (i < l.n && l.p[i] == ' ') i++;
  if (i < 2 || i >= l.n) return 0;
  unsigned char c = l.p[i];
  if (c < 0x80) return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
  uint32_t cp;
  size_t adv = u8_decode(l.p, l.n, i, &cp);
  return adv != 0 && !cp_is_js_space(cp);
}

/* A line with spaces and tabs at either end dropped. */
static Slice trim_line(Slice l) {
  size_t a = 0, b = l.n;
  while (a < b && (l.p[a] == ' ' || l.p[a] == '\t')) a++;
  while (b > a && (l.p[b - 1] == ' ' || l.p[b - 1] == '\t')) b--;
  return slice_make(l.p + a, b - a);
}

static int md_escapable(unsigned char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']':
    case '(': case ')': case '#': case '+': case '-': case '.':
    case '!': case '>':
      return 1;
    default:
      return 0;
  }
}

/* The first index from `from` where `s` holds `what` outside a backslash
 * escape, or SIZE_MAX. */
static size_t find_unescaped(Slice s, const char *what, size_t wlen, size_t from) {
  if (wlen == 0 || from > s.n) return SIZE_MAX;
  for (size_t j = from; j + wlen <= s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && md_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.p + j, what, wlen) == 0) return j;
  }
  return SIZE_MAX;
}

/* The first single * from `from` (one whose run is not **), outside an
 * escape, or SIZE_MAX. */
static size_t find_single_star(Slice s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && md_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (s.p[j] != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return j;
  }
  return SIZE_MAX;
}

/* Whether any character of s is JavaScript whitespace. */
static int slice_has_js_ws(Slice s) {
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = s.p[i];
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') return 1;
      i++;
      continue;
    }
    uint32_t cp;
    size_t adv = u8_decode(s.p, s.n, i, &cp);
    if (adv == 0) {
      i++;
      continue;
    }
    if (cp_is_js_space(cp)) return 1;
    i += adv;
  }
  return 0;
}

static void md_inline(Grow *out, Slice s, MdCtx *ctx) {
  size_t i = 0;
  while (i < s.n) {
    size_t j = scan_inline(s.p + i, s.n - i);
    if (j > 0) {
      grow_addn(out, s.p + i, j);
      i += j;
    }
    if (i >= s.n) break;
    unsigned char c = s.p[i];
    if (c == '\\') {
      if (i + 1 < s.n && md_escapable(s.p[i + 1])) {
        unsigned char e = s.p[i + 1];
        Slice one = slice_make(&e, 1);
        grow_add_html_esc(out, one);
        i += 2;
      } else {
        grow_addc(out, '\\');
        i++;
      }
      continue;
    }
    if (c == '`') {
      size_t k = find_byte(s.p + i + 1, s.n - (i + 1), '`');
      if (k < s.n - (i + 1) && k > 0) {
        size_t close = i + 1 + k;
        grow_add_lit(out, "<code>");
        grow_add_html_esc(out, slice_make(s.p + i + 1, close - (i + 1)));
        grow_add_lit(out, "</code>");
        i = close + 1;
      } else {
        grow_addc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.n && s.p[i + 1] == '*') {
        size_t k = find_unescaped(s, "**", 2, i + 2);
        if (k != SIZE_MAX && k > i + 2) {
          grow_add_lit(out, "<strong>");
          md_inline(out, slice_make(s.p + i + 2, k - (i + 2)), ctx);
          grow_add_lit(out, "</strong>");
          i = k + 2;
        } else {
          grow_add_lit(out, "**");
          i += 2;
        }
      } else {
        size_t k = find_single_star(s, i + 1);
        if (k != SIZE_MAX && k > i + 1) {
          grow_add_lit(out, "<em>");
          md_inline(out, slice_make(s.p + i + 1, k - (i + 1)), ctx);
          grow_add_lit(out, "</em>");
          i = k + 1;
        } else {
          grow_addc(out, '*');
          i++;
        }
      }
      continue;
    }
    if (c == '[') {
      size_t k = find_unescaped(s, "]", 1, i + 1);
      size_t m = SIZE_MAX;
      if (k != SIZE_MAX && k + 1 < s.n && s.p[k + 1] == '(') {
        size_t r = find_byte(s.p + k + 2, s.n - (k + 2), ')');
        if (r < s.n - (k + 2)) m = k + 2 + r;
      }
      if (m != SIZE_MAX) {
        Slice target = slice_make(s.p + k + 2, m - (k + 2));
        if (target.n > 0 && !slice_has_js_ws(target)) {
          ctx->link_scratch->len = 0;
          Slice href = ctx->link_fn(ctx->link_ud, target, ctx->link_scratch);
          grow_add_lit(out, "<a href=\"");
          grow_add_html_esc(out, href);
          grow_add_lit(out, "\">");
          md_inline(out, slice_make(s.p + i + 1, k - (i + 1)), ctx);
          grow_add_lit(out, "</a>");
          i = m + 1;
          continue;
        }
      }
      grow_addc(out, '[');
      i++;
      continue;
    }
    /* One of & < > " ' : its HTML entity. */
    switch (c) {
      case '&': grow_add_lit(out, "&amp;"); break;
      case '<': grow_add_lit(out, "&lt;"); break;
      case '>': grow_add_lit(out, "&gt;"); break;
      case '"': grow_add_lit(out, "&quot;"); break;
      default: grow_add_lit(out, "&#39;"); break;
    }
    i++;
  }
}

/* Whether a line ends a paragraph: blank, fence open, heading or hr. */
static int ends_paragraph(Slice l) {
  if (line_blank(l)) return 1;
  Slice lang;
  if (line_fence_open(l, &lang)) return 1;
  int depth;
  Slice text;
  if (line_heading(l, &depth, &text)) return 1;
  if (line_hr(l)) return 1;
  return 0;
}

typedef struct {
  Slice html;
  Slice excerpt;
  Val *toc; /* array of {level, id, html} */
} MdOut;

typedef struct {
  int level;
  Slice id;
  size_t off;
  size_t len;
} TocEntry;

static MdOut md_render(Bump *b, Slice body, MdCtx *ctx) {
  MdOut res;
  res.html = slice_make(NULL, 0);
  res.excerpt = slice_make(NULL, 0);
  res.toc = NULL;
  /* Split the body into lines (bump-owned). */
  size_t cap = 64, nlines = 0;
  Slice *lines = (Slice *)bump_alloc(b, sizeof(Slice) * cap);
  size_t pos = 0;
  while (pos <= body.n) {
    size_t e = find_byte(body.p + pos, body.n - pos, '\n');
    size_t end = pos + e;
    if (nlines == cap) {
      size_t nc = cap * 2;
      Slice *nl = (Slice *)bump_alloc(b, sizeof(Slice) * nc);
      memcpy(nl, lines, sizeof(Slice) * nlines);
      lines = nl;
      cap = nc;
    }
    lines[nlines++] = slice_make(body.p + pos, end - pos);
    pos = end + 1;
  }
  Grow html = {0};
  TocEntry *toc = NULL;
  size_t ntoc = 0, tocap = 0;
  size_t excerpt_off = 0, excerpt_len = 0;
  int have_excerpt = 0;
  size_t i = 0;
  while (i < nlines) {
    Slice line = lines[i];
    if (line_blank(line)) {
      i++;
      continue;
    }
    Slice lang;
    if (line_fence_open(line, &lang)) {
      i++;
      grow_add_lit(&html, "<pre><code");
      if (lang.n > 0) {
        grow_add_lit(&html, " class=\"language-");
        grow_add_html_esc(&html, lang);
        grow_addc(&html, '"');
      }
      grow_addc(&html, '>');
      while (i < nlines && !line_fence_close(lines[i])) {
        grow_add_html_esc(&html, lines[i]);
        grow_addc(&html, '\n');
        i++;
      }
      i++; /* past the closing fence (or past the end) */
      grow_add_lit(&html, "</code></pre>\n");
      continue;
    }
    int depth = 0;
    Slice htext;
    if (line_heading(line, &depth, &htext)) {
      Slice slug = slug_of(htext, ctx->slug_scratch);
      Slice id = unique_slug(slug, ctx->ids, b, ctx->slug_scratch);
      size_t h_off = html.len;
      grow_add_lit(&html, "<h");
      char dbuf[4];
      int dl = snprintf(dbuf, sizeof(dbuf), "%d", depth);
      grow_addn(&html, dbuf, (size_t)dl);
      grow_add_lit(&html, " id=\"");
      grow_add_slice(&html, id);
      grow_add_lit(&html, "\">");
      size_t inner_off = html.len;
      md_inline(&html, htext, ctx);
      size_t inner_len = html.len - inner_off;
      grow_add_lit(&html, "</h");
      grow_addn(&html, dbuf, (size_t)dl);
      grow_add_lit(&html, ">\n");
      (void)h_off;
      if (depth == 2 || depth == 3) {
        if (ntoc == tocap) {
          size_t nc = tocap == 0 ? 8 : tocap * 2;
          TocEntry *nt = (TocEntry *)bump_alloc(b, sizeof(TocEntry) * nc);
          if (toc) memcpy(nt, toc, sizeof(TocEntry) * ntoc);
          toc = nt;
          tocap = nc;
        }
        toc[ntoc].level = depth;
        toc[ntoc].id = id;
        toc[ntoc].off = inner_off;
        toc[ntoc].len = inner_len;
        ntoc++;
      }
      i++;
      continue;
    }
    if (line_hr(line)) {
      grow_add_lit(&html, "<hr>\n");
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      Grow qtext = {0};
      int first = 1;
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        Slice raw = lines[i];
        size_t st = 1;
        if (st < raw.n && raw.p[st] == ' ') st++;
        Slice stripped = slice_make(raw.p + st, raw.n - st);
        Slice t = trim_line(stripped);
        if (t.n > 0) {
          if (!first) grow_addc(&qtext, '\n');
          first = 0;
          grow_add_slice(&qtext, t);
        }
        i++;
      }
      if (qtext.len == 0) {
        grow_add_lit(&html, "<blockquote>\n</blockquote>\n");
      } else {
        grow_add_lit(&html, "<blockquote>\n<p>");
        md_inline(&html, slice_make(qtext.data, qtext.len), ctx);
        grow_add_lit(&html, "</p>\n</blockquote>\n");
      }
      free(qtext.data);
      continue;
    }
    Slice bitem;
    int64_t onum = 0;
    Slice oitem;
    int is_bullet = line_bullet(line, &bitem);
    int is_ordered = !is_bullet && line_ordered(line, &onum, &oitem);
    if (is_bullet || is_ordered) {
      int ordered = is_ordered;
      int64_t start = ordered ? onum : 1;
      Grow items = {0};
      /* Items are stored as (offset,len) pairs into `items` plus a count. */
      size_t icap = 8, nitems = 0;
      size_t *ioff = (size_t *)bump_alloc(b, sizeof(size_t) * icap);
      size_t *ilen = (size_t *)bump_alloc(b, sizeof(size_t) * icap);
      while (i < nlines) {
        Slice cur = lines[i];
        Slice it;
        int64_t num2 = 0;
        Slice o2;
        int hit = 0;
        if (ordered) {
          if (line_ordered(cur, &num2, &o2)) {
            it = o2;
            hit = 1;
          }
        } else {
          if (line_bullet(cur, &it)) hit = 1;
        }
        if (hit) {
          Slice t = trim_line(it);
          if (nitems == icap) {
            size_t nc = icap * 2;
            size_t *no = (size_t *)bump_alloc(b, sizeof(size_t) * nc);
            size_t *nl2 = (size_t *)bump_alloc(b, sizeof(size_t) * nc);
            memcpy(no, ioff, sizeof(size_t) * nitems);
            memcpy(nl2, ilen, sizeof(size_t) * nitems);
            ioff = no;
            ilen = nl2;
            icap = nc;
          }
          ioff[nitems] = items.len;
          grow_add_slice(&items, t);
          ilen[nitems] = items.len - ioff[nitems];
          nitems++;
          i++;
        } else if (line_continued(cur)) {
          Slice t = trim_line(cur);
          grow_addc(&items, '\n');
          grow_add_slice(&items, t);
          ilen[nitems - 1] = items.len - ioff[nitems - 1];
          i++;
        } else {
          break;
        }
      }
      if (ordered && start != 1) {
        char sb[32];
        int sl = snprintf(sb, sizeof(sb), "<ol start=\"%lld\">", (long long)start);
        grow_addn(&html, sb, (size_t)sl);
      } else if (ordered) {
        grow_add_lit(&html, "<ol>");
      } else {
        grow_add_lit(&html, "<ul>");
      }
      grow_addc(&html, '\n');
      for (size_t k = 0; k < nitems; k++) {
        grow_add_lit(&html, "<li>");
        md_inline(&html, slice_make(items.data + ioff[k], ilen[k]), ctx);
        grow_add_lit(&html, "</li>\n");
      }
      grow_add_lit(&html, ordered ? "</ol>\n" : "</ul>\n");
      free(items.data);
      continue;
    }
    /* Paragraph: lines until one that ends it. */
    {
      Grow ptext = {0};
      grow_add_slice(&ptext, trim_line(line));
      i++;
      while (i < nlines && !ends_paragraph(lines[i])) {
        grow_addc(&ptext, '\n');
        grow_add_slice(&ptext, trim_line(lines[i]));
        i++;
      }
      size_t p_off = html.len;
      grow_add_lit(&html, "<p>");
      size_t inner_off = html.len;
      md_inline(&html, slice_make(ptext.data, ptext.len), ctx);
      size_t inner_len = html.len - inner_off;
      grow_add_lit(&html, "</p>\n");
      (void)p_off;
      if (!have_excerpt) {
        have_excerpt = 1;
        excerpt_off = inner_off;
        excerpt_len = inner_len;
      }
      free(ptext.data);
    }
  }
  res.html = slice_make(html.data, html.len);
  res.excerpt = slice_make(html.data + excerpt_off, excerpt_len);
  Val **titems = (Val **)bump_alloc(b, sizeof(Val *) * (ntoc == 0 ? 1 : ntoc));
  for (size_t k = 0; k < ntoc; k++) {
    Val *rec = val_rec(b);
    val_rec_set(b, rec, F_level, val_num(b, toc[k].level));
    val_rec_set(b, rec, F_id, val_str(b, toc[k].id));
    val_rec_set(b, rec, F_html, val_str(b, slice_make(html.data + toc[k].off, toc[k].len)));
    titems[k] = rec;
  }
  res.toc = val_arr(b, titems, ntoc);
  return res;
}

/* -------------------------------------------------------------- templates */

#define OP_TEXT 0
#define OP_VAR 1
#define OP_RAW 2
#define OP_IF 3
#define OP_ELSE 4
#define OP_FOR 5
#define OP_END 6
#define OP_INCLUDE 7

typedef struct {
  uint8_t kind;
  Slice text; /* TEXT: literal; INCLUDE: template name */
  int *path;  /* VAR/RAW/IF/FOR: interned path ids */
  size_t plen;
  int not_flag; /* IF */
  int name_id;  /* FOR: loop variable id */
  size_t else_idx; /* IF: ELSE op index, or SIZE_MAX */
  size_t end_idx;  /* IF/FOR: END op index */
} Op;

typedef struct {
  Op *ops;
  size_t nops;
  size_t cap;
} Tpl;

static int tpl_emit(Bump *b, Tpl *t, Op op) {
  if (t->nops == t->cap) {
    size_t nc = t->cap == 0 ? 32 : t->cap * 2;
    Op *no = (Op *)bump_alloc(b, sizeof(Op) * nc);
    if (!no) return 0;
    if (t->ops) memcpy(no, t->ops, sizeof(Op) * t->nops);
    t->ops = no;
    t->cap = nc;
  }
  t->ops[t->nops++] = op;
  return 1;
}

static int is_ident_start(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_ident_char(unsigned char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Find `needle` in `hay` starting at `from`; SIZE_MAX when absent. */
static size_t find_substr(Slice hay, size_t from, const char *needle, size_t nlen) {
  if (nlen == 0 || from >= hay.n) return SIZE_MAX;
  for (size_t i = from; i + nlen <= hay.n; i++) {
    if (hay.p[i] == (unsigned char)needle[0] && memcmp(hay.p + i, needle, nlen) == 0) return i;
  }
  return SIZE_MAX;
}

static int slice_all_space_tab(Slice s, size_t a, size_t b) {
  for (size_t i = a; i < b; i++) {
    if (s.p[i] != ' ' && s.p[i] != '\t') return 0;
  }
  return 1;
}

/* Parse and intern a dotted path (PATH regex). Returns 0 when invalid. */
static int path_parse(Bump *b, Intern *in, Slice text, int **out_ids, size_t *out_n) {
  if (text.n == 0) return 0;
  size_t ncomp = 1;
  for (size_t i = 0; i < text.n; i++) {
    if (text.p[i] == '.') ncomp++;
  }
  int *ids = (int *)bump_alloc(b, sizeof(int) * ncomp);
  if (!ids) return 0;
  size_t nc = 0;
  size_t start = 0;
  for (size_t i = 0; i <= text.n; i++) {
    if (i == text.n || text.p[i] == '.') {
      Slice comp = slice_make(text.p + start, i - start);
      if (comp.n == 0 || !is_ident_start(comp.p[0])) return 0;
      for (size_t k = 1; k < comp.n; k++) {
        if (!is_ident_char(comp.p[k])) return 0;
      }
      ids[nc++] = intern_id(in, comp, b);
      start = i + 1;
    }
  }
  *out_ids = ids;
  *out_n = nc;
  return 1;
}

/* Append a JSON-quoted string to `out` (for the "not a path" error). */
static void grow_add_json_quote(Grow *out, Slice s) {
  static const char HEX[] = "0123456789abcdef";
  grow_addc(out, '"');
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = s.p[i];
    switch (c) {
      case '"': grow_add_lit(out, "\\\""); break;
      case '\\': grow_add_lit(out, "\\\\"); break;
      case '\n': grow_add_lit(out, "\\n"); break;
      case '\r': grow_add_lit(out, "\\r"); break;
      case '\t': grow_add_lit(out, "\\t"); break;
      case '\b': grow_add_lit(out, "\\b"); break;
      case '\f': grow_add_lit(out, "\\f"); break;
      default:
        if (c < 0x20) {
          char tmp[8];
          tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
          tmp[4] = HEX[(c >> 4) & 0xF]; tmp[5] = HEX[c & 0xF];
          grow_addn(out, tmp, 6);
        } else {
          grow_addc(out, c);
        }
        break;
    }
  }
  grow_addc(out, '"');
}

static void tpl_err_prefix(Grow *err, Slice name) {
  grow_add_lit(err, "template \"");
  grow_add_slice(err, name);
  grow_add_lit(err, "\": ");
}

typedef struct {
  int kind; /* 0 = for, 1 = if */
  size_t op_idx;
  size_t else_idx; /* SIZE_MAX when the if has no else yet */
} Frame;

/* Compile a template into flat ops. Returns 1 on success; on failure
 * returns 0 and `err` holds `template "<name>": <reason>`. */
static int tpl_compile(Bump *b, Intern *in, Slice name, Slice src, Tpl *tpl, Grow *err) {
  memset(tpl, 0, sizeof(*tpl));
  Frame stack[128];
  int depth = 0;
  size_t i = 0;
  size_t text_begin = 0;
  while (i < src.n) {
    size_t rel = find_byte(src.p + i, src.n - i, '{');
    if (rel >= src.n - i || i + rel >= src.n - 1) break; /* no '{', or a trailing '{' */
    size_t open = i + rel;
    unsigned char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      i = open + 1;
      continue;
    }
    if (next == '{') {
      int raw = (open + 2 < src.n && src.p[open + 2] == '{');
      const char *close_str = raw ? "}}}" : "}}";
      size_t close_len = raw ? 3 : 2;
      size_t close = find_substr(src, open + close_len, close_str, close_len);
      if (close == SIZE_MAX) {
        tpl_err_prefix(err, name);
        grow_add_lit(err, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      if (open > text_begin) {
        Op op;
        memset(&op, 0, sizeof(op));
        op.kind = OP_TEXT;
        op.text = slice_make(src.p + text_begin, open - text_begin);
        op.else_idx = SIZE_MAX;
        if (!tpl_emit(b, tpl, op)) return 0;
      }
      Slice inner = js_trim(slice_make(src.p + open + close_len, close - (open + close_len)));
      int *path = NULL;
      size_t plen = 0;
      if (!path_parse(b, in, inner, &path, &plen)) {
        tpl_err_prefix(err, name);
        grow_add_json_quote(err, inner);
        grow_add_lit(err, " is not a path");
        return 0;
      }
      Op op;
      memset(&op, 0, sizeof(op));
      op.kind = raw ? OP_RAW : OP_VAR;
      op.path = path;
      op.plen = plen;
      op.else_idx = SIZE_MAX;
      if (!tpl_emit(b, tpl, op)) return 0;
      i = close + close_len;
      text_begin = i;
      continue;
    }
    /* A {% %} tag. */
    size_t close = find_substr(src, open + 2, "%}", 2);
    if (close == SIZE_MAX) {
      tpl_err_prefix(err, name);
      grow_add_lit(err, "a {% is not closed");
      return 0;
    }
    size_t end = close + 2;
    size_t line_start = 0;
    if (open > 0) {
      size_t k = open - 1;
      while (k > 0 && src.p[k] != '\n') k--;
      line_start = src.p[k] == '\n' ? k + 1 : 0;
    }
    size_t nl_rel = find_byte(src.p + end, src.n - end, '\n');
    size_t nl = nl_rel >= src.n - end ? SIZE_MAX : end + nl_rel;
    size_t line_end = nl == SIZE_MAX ? src.n : nl;
    int standalone = slice_all_space_tab(src, line_start, open) && slice_all_space_tab(src, end, line_end);
    size_t text_end = standalone ? line_start : open;
    if (text_end > text_begin) {
      Op op;
      memset(&op, 0, sizeof(op));
      op.kind = OP_TEXT;
      op.text = slice_make(src.p + text_begin, text_end - text_begin);
      op.else_idx = SIZE_MAX;
      if (!tpl_emit(b, tpl, op)) return 0;
    }
    Slice tag = js_trim(slice_make(src.p + open + 2, close - (open + 2)));
    int handled = 0;
    /* {% for x in path %} */
    if (tag.n >= 4 && memcmp(tag.p, "for ", 4) == 0) {
      size_t p = 4;
      if (p < tag.n && is_ident_start(tag.p[p])) {
        size_t ns = p;
        while (p < tag.n && is_ident_char(tag.p[p])) p++;
        Slice nm = slice_make(tag.p + ns, p - ns);
        if (p + 4 <= tag.n && memcmp(tag.p + p, " in ", 4) == 0) {
          Slice pth = slice_make(tag.p + p + 4, tag.n - (p + 4));
          if (pth.n > 0 && !slice_has_js_ws(pth)) {
            int *path = NULL;
            size_t plen = 0;
            if (!path_parse(b, in, pth, &path, &plen)) {
              tpl_err_prefix(err, name);
              grow_add_json_quote(err, pth);
              grow_add_lit(err, " is not a path");
              return 0;
            }
            Op op;
            memset(&op, 0, sizeof(op));
            op.kind = OP_FOR;
            op.path = path;
            op.plen = plen;
            op.name_id = intern_id(in, nm, b);
            op.else_idx = SIZE_MAX;
            op.end_idx = SIZE_MAX;
            size_t idx = tpl->nops;
            if (!tpl_emit(b, tpl, op)) return 0;
            if (depth >= 128) return 0;
            stack[depth].kind = 0;
            stack[depth].op_idx = idx;
            stack[depth].else_idx = SIZE_MAX;
            depth++;
            handled = 1;
          }
        }
      }
    }
    /* {% if [not] path %} */
    if (!handled && tag.n >= 3 && memcmp(tag.p, "if ", 3) == 0) {
      Slice rest = slice_make(tag.p + 3, tag.n - 3);
      int not_flag = 0;
      if (rest.n >= 4 && memcmp(rest.p, "not ", 4) == 0) {
        not_flag = 1;
        rest = slice_make(rest.p + 4, rest.n - 4);
      }
      if (rest.n > 0 && !slice_has_js_ws(rest)) {
        int *path = NULL;
        size_t plen = 0;
        if (!path_parse(b, in, rest, &path, &plen)) {
          tpl_err_prefix(err, name);
          grow_add_json_quote(err, rest);
          grow_add_lit(err, " is not a path");
          return 0;
        }
        Op op;
        memset(&op, 0, sizeof(op));
        op.kind = OP_IF;
        op.path = path;
        op.plen = plen;
        op.not_flag = not_flag;
        op.else_idx = SIZE_MAX;
        op.end_idx = SIZE_MAX;
        size_t idx = tpl->nops;
        if (!tpl_emit(b, tpl, op)) return 0;
        if (depth >= 128) return 0;
        stack[depth].kind = 1;
        stack[depth].op_idx = idx;
        stack[depth].else_idx = SIZE_MAX;
        depth++;
        handled = 1;
      }
    }
    /* {% else %} */
    if (!handled && tag.n == 4 && memcmp(tag.p, "else", 4) == 0) {
      if (depth == 0 || stack[depth - 1].kind != 1 || stack[depth - 1].else_idx != SIZE_MAX) {
        tpl_err_prefix(err, name);
        grow_add_lit(err, "an else outside an if");
        return 0;
      }
      Op op;
      memset(&op, 0, sizeof(op));
      op.kind = OP_ELSE;
      op.else_idx = SIZE_MAX;
      op.end_idx = SIZE_MAX;
      size_t idx = tpl->nops;
      if (!tpl_emit(b, tpl, op)) return 0;
      tpl->ops[stack[depth - 1].op_idx].else_idx = idx;
      stack[depth - 1].else_idx = idx;
      handled = 1;
    }
    /* {% endfor %} / {% endif %} */
    if (!handled && (tag.n == 6 || tag.n == 5)) {
      int is_for = tag.n == 6 && memcmp(tag.p, "endfor", 6) == 0;
      int is_if = tag.n == 5 && memcmp(tag.p, "endif", 5) == 0;
      if (is_for || is_if) {
        int want = is_for ? 0 : 1;
        if (depth == 0 || stack[depth - 1].kind != want) {
          tpl_err_prefix(err, name);
          grow_add_lit(err, is_for ? "an endfor with no open for" : "an endif with no open if");
          return 0;
        }
        Op op;
        memset(&op, 0, sizeof(op));
        op.kind = OP_END;
        op.else_idx = SIZE_MAX;
        op.end_idx = SIZE_MAX;
        size_t idx = tpl->nops;
        if (!tpl_emit(b, tpl, op)) return 0;
        tpl->ops[stack[depth - 1].op_idx].end_idx = idx;
        depth--;
        handled = 1;
      }
    }
    /* {% include "name" %} */
    if (!handled && tag.n >= 10 && memcmp(tag.p, "include \"", 9) == 0 && tag.p[tag.n - 1] == '"') {
      Slice nm = slice_make(tag.p + 9, tag.n - 10);
      int ok = nm.n > 0;
      for (size_t k = 0; k < nm.n; k++) {
        unsigned char c = nm.p[k];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
      }
      if (ok) {
        Op op;
        memset(&op, 0, sizeof(op));
        op.kind = OP_INCLUDE;
        op.text = nm;
        op.else_idx = SIZE_MAX;
        if (!tpl_emit(b, tpl, op)) return 0;
        handled = 1;
      }
    }
    if (!handled) {
      tpl_err_prefix(err, name);
      grow_add_lit(err, "{% ");
      grow_add_slice(err, tag);
      grow_add_lit(err, " %} is not a tag");
      return 0;
    }
    i = standalone ? (nl == SIZE_MAX ? src.n : nl + 1) : end;
    text_begin = i;
  }
  if (text_begin < src.n) {
    Op op;
    memset(&op, 0, sizeof(op));
    op.kind = OP_TEXT;
    op.text = slice_make(src.p + text_begin, src.n - text_begin);
    op.else_idx = SIZE_MAX;
    if (!tpl_emit(b, tpl, op)) return 0;
  }
  if (depth > 0) {
    tpl_err_prefix(err, name);
    grow_add_lit(err, stack[depth - 1].kind == 0 ? "a for is not closed" : "a if is not closed");
    return 0;
  }
  return 1;
}

/* Render ops[start..end) in scope; depth is the include depth. */
static void tpl_render_ops(Bump *b, Tpl *tpl, size_t start, size_t end, Scope *scope, int depth, Grow *out, StrMap *templates) {
  for (size_t pc = start; pc < end; pc++) {
    Op *op = &tpl->ops[pc];
    switch (op->kind) {
      case OP_TEXT:
        grow_add_slice(out, op->text);
        break;
      case OP_VAR: {
        Val *v = scope_lookup(b, scope, op->path, op->plen);
        val_text(out, b, v, 1);
        break;
      }
      case OP_RAW: {
        Val *v = scope_lookup(b, scope, op->path, op->plen);
        val_text(out, b, v, 0);
        break;
      }
      case OP_IF: {
        Val *v = scope_lookup(b, scope, op->path, op->plen);
        int cond = val_truthy(v) != op->not_flag;
        if (cond) {
          size_t sub_end = op->else_idx != SIZE_MAX ? op->else_idx : op->end_idx;
          tpl_render_ops(b, tpl, pc + 1, sub_end, scope, depth, out, templates);
        } else if (op->else_idx != SIZE_MAX) {
          tpl_render_ops(b, tpl, op->else_idx + 1, op->end_idx, scope, depth, out, templates);
        }
        pc = op->end_idx;
        break;
      }
      case OP_FOR: {
        Val *v = scope_lookup(b, scope, op->path, op->plen);
        if (v && v->kind == VAL_ARR) {
          for (size_t k = 0; k < v->u.arr.n; k++) {
            Scope child;
            child.parent = scope;
            child.n = 2;
            child.ids[0] = op->name_id;
            child.vals[0] = v->u.arr.items[k];
            Val *loop = val_rec(b);
            val_rec_set(b, loop, F_index, val_num(b, (int64_t)k + 1));
            val_rec_set(b, loop, F_first, val_bool(b, k == 0));
            val_rec_set(b, loop, F_last, val_bool(b, k == v->u.arr.n - 1));
            val_rec_set(b, loop, F_length, val_num(b, (int64_t)v->u.arr.n));
            child.ids[1] = F_loop;
            child.vals[1] = loop;
            tpl_render_ops(b, tpl, pc + 1, op->end_idx, &child, depth, out, templates);
          }
        }
        pc = op->end_idx;
        break;
      }
      case OP_INCLUDE: {
        Tpl *t = (Tpl *)strmap_get(templates, op->text);
        if (t && depth < 8) tpl_render_ops(b, t, 0, t->nops, scope, depth + 1, out, templates);
        break;
      }
      default:
        break; /* OP_ELSE / OP_END are skipped by their IF/FOR. */
    }
  }
}

/* ------------------------------------------------------- JSON iteration */

static int json_array_next(Slice arr, size_t *pos, Slice *elem) {
  size_t i = json_ws(arr.p, arr.n, *pos);
  if (i >= arr.n) return 0;
  if (arr.p[i] == ']') {
    *pos = i + 1;
    return 0;
  }
  if (arr.p[i] == ',') i = json_ws(arr.p, arr.n, i + 1);
  size_t end = json_skip_value(arr.p, arr.n, i);
  if (end == SIZE_MAX) return 0;
  *elem = slice_make(arr.p + i, end - i);
  *pos = end;
  return 1;
}

static int json_object_next(Slice obj, size_t *pos, Slice *key, Slice *val) {
  size_t i = json_ws(obj.p, obj.n, *pos);
  if (i >= obj.n) return 0;
  if (obj.p[i] == '}') {
    *pos = i + 1;
    return 0;
  }
  if (obj.p[i] == ',') i = json_ws(obj.p, obj.n, i + 1);
  if (i >= obj.n || obj.p[i] != '"') return 0;
  size_t kend = json_skip_string(obj.p, obj.n, i);
  if (kend == SIZE_MAX) return 0;
  *key = slice_make(obj.p + i, kend - i);
  i = json_ws(obj.p, obj.n, kend);
  if (i >= obj.n || obj.p[i] != ':') return 0;
  i = json_ws(obj.p, obj.n, i + 1);
  size_t vend = json_skip_value(obj.p, obj.n, i);
  if (vend == SIZE_MAX) return 0;
  *val = slice_make(obj.p + i, vend - i);
  *pos = vend;
  return 1;
}

static int64_t json_number(Slice raw) {
  int64_t v = 0;
  int neg = 0;
  size_t i = 0;
  if (i < raw.n && raw.p[i] == '-') {
    neg = 1;
    i++;
  }
  for (; i < raw.n && raw.p[i] >= '0' && raw.p[i] <= '9'; i++) {
    v = v * 10 + (raw.p[i] - '0');
  }
  return neg ? -v : v;
}

/* ------------------------------------------------------------ the pipeline */

typedef struct Post Post;
typedef struct Tag Tag;

typedef struct {
  Slice name;
  Slice slug;
  Slice url;
  Tag *tag; /* NULL for a page-only tag reference */
} TagRef;

struct Tag {
  Slice name;
  Slice slug;
  Slice url;
  Post **posts;
  size_t n_posts;
  size_t cap_posts;
};

struct Post {
  Slice path;
  Slice text;
  FrontMatter fm;
  Slice body;
  int is_post;
  Slice layout;
  Slice slug;
  Slice out_dir;
  Slice url;
  Slice html;
  Slice excerpt;
  Val *toc;
  Post *newer;
  Post *older;
  TagRef *tag_refs;
  size_t n_tag_refs;
  Val *card;
  Val *page;
};

typedef struct {
  Slice *lines;
  size_t n;
  size_t cap;
} LineVec;

static void linevec_push(Bump *b, LineVec *v, Slice line) {
  if (v->n == v->cap) {
    size_t nc = v->cap == 0 ? 32 : v->cap * 2;
    Slice *nl = (Slice *)bump_alloc(b, sizeof(Slice) * nc);
    if (v->lines) memcpy(nl, v->lines, sizeof(Slice) * v->n);
    v->lines = nl;
    v->cap = nc;
  }
  v->lines[v->n++] = line;
}

typedef struct {
  Slice path;
  Slice content;
} OutEntry;

typedef struct {
  OutEntry *entries;
  size_t n;
  size_t cap;
  StrMap index; /* path -> (index+1) as a pointer */
} OutList;

static void out_set(Bump *b, OutList *o, Slice path, Slice content) {
  void *hit = strmap_get(&o->index, path);
  if (hit) {
    size_t idx = (size_t)((uintptr_t)hit - 1);
    o->entries[idx].content = content;
    return;
  }
  if (o->n == o->cap) {
    size_t nc = o->cap == 0 ? 64 : o->cap * 2;
    OutEntry *ne = (OutEntry *)bump_alloc(b, sizeof(OutEntry) * nc);
    if (o->entries) memcpy(ne, o->entries, sizeof(OutEntry) * o->n);
    o->entries = ne;
    o->cap = nc;
  }
  o->entries[o->n].path = path;
  o->entries[o->n].content = content;
  strmap_put(&o->index, path, (void *)(uintptr_t)(o->n + 1), b);
  o->n++;
}

typedef struct {
  Bump bump;
  Intern intern;
  Slice base;
  Slice site_title;
  int64_t per_page;
  int64_t recent;
  StrMap templates; /* name -> Tpl* */
  LineVec report;
  StrSet broken_seen;
  Grow scratch;
  Grow slug_scratch;
  Grow link_scratch;
  Post **sources;
  size_t n_sources;
  size_t cap_sources;
  Post **posts;
  size_t n_posts;
  Tag **tags;
  size_t n_tags;
  StrMap by_source; /* path -> Post* */
  StrSet static_paths;
  StrMap static_contents; /* static path -> Slice* (bump-owned content) */
  OutList out;
  StrMap taken_dirs; /* dir -> StrSet* */
  Slice *static_files; /* paths, in file order */
  size_t n_statics;
} Case;

static int cmp_slice_ptr(const void *a, const void *b) {
  Slice x = *(Slice *)a, y = *(Slice *)b;
  return slice_cmp(x, y);
}

static int cmp_post_ptr(const void *a, const void *b) {
  Post *x = *(Post **)a, *y = *(Post **)b;
  return slice_cmp(x->path, y->path);
}

/* `text` with every \r\n or lone \r turned into \n (the reference's
 * readSources normalisation). Zero-copy when there is no \r. */
static Slice crlf_normalize(Bump *b, Slice text) {
  size_t r = find_byte(text.p, text.n, '\r');
  if (r >= text.n) return text;
  unsigned char *out = (unsigned char *)bump_alloc(b, text.n + 1);
  if (!out) return text;
  size_t o = 0, i = 0;
  while (i < text.n) {
    size_t j = find_byte(text.p + i, text.n - i, '\r');
    if (j >= text.n - i) {
      memcpy(out + o, text.p + i, text.n - i);
      o += text.n - i;
      break;
    }
    memcpy(out + o, text.p + i, j);
    o += j;
    i += j;
    out[o++] = '\n';
    i++; /* the \r */
    if (i < text.n && text.p[i] == '\n') i++; /* the \n of a \r\n pair */
  }
  return slice_make(out, o);
}

static Slice dir_of(Slice path) {
  size_t slash = SIZE_MAX;
  for (size_t i = 0; i < path.n; i++) {
    if (path.p[i] == '/') slash = i;
  }
  if (slash == SIZE_MAX) return slice_make(path.p, 0);
  return slice_make(path.p, slash);
}

static int is_alpha_byte(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_alnum_byte(unsigned char c) {
  return is_alpha_byte(c) || (c >= '0' && c <= '9');
}

/* `rel` resolved from `dir` (both '/'-separated), or NULL when it climbs
 * out of the root. The result is written into `scratch`. */
static int resolve_path(Slice dir, Slice rel, Grow *scratch, Slice *out) {
  Slice segs[64];
  size_t nseg = 0;
  if (dir.n > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (nseg < 64) segs[nseg++] = slice_make(dir.p + start, i - start);
        start = i + 1;
      }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Slice seg = slice_make(rel.p + start, i - start);
      start = i + 1;
      if (seg.n == 0) continue;
      if (seg.n == 1 && seg.p[0] == '.') continue;
      if (seg.n == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
        if (nseg == 0) return 0;
        nseg--;
        continue;
      }
      if (nseg < 64) segs[nseg++] = seg;
    }
  }
  scratch->len = 0;
  for (size_t k = 0; k < nseg; k++) {
    if (k > 0) grow_addc(scratch, '/');
    grow_add_slice(scratch, segs[k]);
  }
  *out = slice_make(scratch->data, scratch->len);
  return 1;
}

typedef struct {
  Case *c;
  Post *src;
  Slice dir;
} LinkUd;

static Slice link_resolve(void *ud, Slice target, Grow *scratch) {
  LinkUd *lu = (LinkUd *)ud;
  Case *c = lu->c;
  if (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#')) return target;
  if (target.n >= 2 && is_alpha_byte(target.p[0])) {
    size_t k = 1;
    while (k < target.n && (is_alnum_byte(target.p[k]) || target.p[k] == '+' || target.p[k] == '.' || target.p[k] == '-')) k++;
    if (k < target.n && target.p[k] == ':') return target;
  }
  size_t hash = find_byte(target.p, target.n, '#');
  Slice rel = hash >= target.n ? target : slice_make(target.p, hash);
  Slice frag = hash >= target.n ? slice_make(target.p + target.n, 0) : slice_make(target.p + hash, target.n - hash);
  Slice path;
  int have_path = 0;
  Grow tmp = {0};
  if (rel.n > 0) {
    if (resolve_path(lu->dir, rel, &tmp, &path)) have_path = 1;
  }
  if (have_path) {
    Post *hit = (Post *)strmap_get(&c->by_source, path);
    if (hit) {
      scratch->len = 0;
      grow_add_slice(scratch, hit->url);
      grow_add_slice(scratch, frag);
      free(tmp.data);
      return slice_make(scratch->data, scratch->len);
    }
    if (strset_has(&c->static_paths, path)) {
      scratch->len = 0;
      grow_add_slice(scratch, c->base);
      grow_addc(scratch, '/');
      grow_add_slice(scratch, path);
      grow_add_slice(scratch, frag);
      free(tmp.data);
      return slice_make(scratch->data, scratch->len);
    }
  }
  free(tmp.data);
  /* A broken link: report it once, return the target unchanged. */
  c->scratch.len = 0;
  grow_add_lit(&c->scratch, "broken-link ");
  grow_add_slice(&c->scratch, lu->src->path);
  grow_add_lit(&c->scratch, " -> ");
  grow_add_slice(&c->scratch, target);
  Slice line = bump_dup(&c->bump, slice_make(c->scratch.data, c->scratch.len));
  if (strset_add(&c->broken_seen, line, &c->bump)) linevec_push(&c->bump, &c->report, line);
  return target;
}

static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

/* "2026-11-09" -> "9 November 2026", from the string alone. */
static Slice long_date(Bump *b, Slice date) {
  char buf[64];
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  int n = snprintf(buf, sizeof(buf), "%d %s %.*s", d, MONTHS[m - 1], 4, date.p);
  char *out = (char *)bump_alloc(b, (size_t)n + 1);
  memcpy(out, buf, (size_t)n);
  return slice_make((const unsigned char *)out, (size_t)n);
}

/* A post's card, as lists show it. */
static Val *card_of(Case *c, Post *s) {
  Bump *b = &c->bump;
  Val *card = val_rec(b);
  val_rec_set(b, card, F_title, val_str(b, s->fm.title));
  val_rec_set(b, card, F_url, val_str(b, s->url));
  val_rec_set(b, card, F_date, val_str(b, s->fm.has_date ? s->fm.date : slice_make((const unsigned char *)"", 0)));
  val_rec_set(b, card, F_date_long, val_str(b, s->fm.has_date ? long_date(b, s->fm.date) : slice_make((const unsigned char *)"", 0)));
  val_rec_set(b, card, F_summary, val_str(b, s->fm.has_summary ? s->fm.summary : slice_make((const unsigned char *)"", 0)));
  val_rec_set(b, card, F_excerpt, val_str(b, s->excerpt));
  long words = count_words(s->body);
  long minutes = (words + 199) / 200;
  if (minutes < 1) minutes = 1;
  val_rec_set(b, card, F_reading_minutes, val_num(b, minutes));
  Val **titems = (Val **)bump_alloc(b, sizeof(Val *) * (s->n_tag_refs == 0 ? 1 : s->n_tag_refs));
  for (size_t k = 0; k < s->n_tag_refs; k++) {
    Val *t = val_rec(b);
    val_rec_set(b, t, F_name, val_str(b, s->tag_refs[k].name));
    val_rec_set(b, t, F_slug, val_str(b, s->tag_refs[k].slug));
    val_rec_set(b, t, F_url, val_str(b, s->tag_refs[k].url));
    titems[k] = t;
  }
  val_rec_set(b, card, F_tags, val_arr(b, titems, s->n_tag_refs));
  return card;
}

static Val *link_of(Case *c, Post *s) {
  if (!s) return NULL;
  Bump *b = &c->bump;
  Val *rec = val_rec(b);
  val_rec_set(b, rec, F_title, val_str(b, s->fm.title));
  val_rec_set(b, rec, F_url, val_str(b, s->url));
  val_rec_set(b, rec, F_date, val_str(b, s->fm.has_date ? s->fm.date : slice_make((const unsigned char *)"", 0)));
  val_rec_set(b, rec, F_date_long, val_str(b, s->fm.has_date ? long_date(b, s->fm.date) : slice_make((const unsigned char *)"", 0)));
  return rec;
}

static Val *page_of(Case *c, Post *s) {
  Bump *b = &c->bump;
  /* A fresh card (the reference spreads a new cardOf into the page record;
   * the list card stored on the post must not gain the page-only fields). */
  Val *page = card_of(c, s);
  val_rec_set(b, page, F_slug, val_str(b, s->slug));
  val_rec_set(b, page, F_content, val_str(b, s->html));
  val_rec_set(b, page, F_toc, s->toc);
  val_rec_set(b, page, F_newer, link_of(c, s->newer));
  val_rec_set(b, page, F_older, link_of(c, s->older));
  val_rec_set(b, page, F_is_post, val_bool(b, s->is_post));
  return page;
}

/* Render a template by name with root vars; returns 1 and fills `out` when
 * the template exists, else 0. */
static int render_named(Case *c, Slice name, Scope *root, Grow *out) {
  Tpl *t = (Tpl *)strmap_get(&c->templates, name);
  if (!t) return 0;
  out->len = 0;
  tpl_render_ops(&c->bump, t, 0, t->nops, root, 0, out, &c->templates);
  return 1;
}

static int cmp_post_date_slug(const void *a, const void *b) {
  Post *x = *(Post **)a, *y = *(Post **)b;
  int d = slice_cmp(y->fm.date, x->fm.date); /* newest first */
  if (d != 0) return d;
  return slice_cmp(x->slug, y->slug);
}

static int cmp_tag_ptr(const void *a, const void *b) {
  Tag *x = *(Tag **)a, *y = *(Tag **)b;
  if (y->n_posts != x->n_posts) return y->n_posts > x->n_posts ? 1 : -1;
  return slice_cmp(x->slug, y->slug);
}

typedef struct {
  Slice name;
  Slice src;
} NameSrc;

static int cmp_namesrc(const void *a, const void *b) {
  const NameSrc *x = (const NameSrc *)a, *y = (const NameSrc *)b;
  return slice_cmp(x->name, y->name);
}

typedef struct {
  Slice path;
  Slice content;
} FileEnt;

static int cmp_fileent(const void *a, const void *b) {
  const FileEnt *x = (const FileEnt *)a, *y = (const FileEnt *)b;
  return slice_cmp(x->path, y->path);
}


/* Build one case's input object. Appends the JSON object body (the
 * `"path":"content"` pairs, comma-separated, no braces) to `answers`. */
static void build_case(Slice input, Grow *answers) {
  Case c;
  memset(&c, 0, sizeof(c));
  intern_init(&c.intern, &c.bump);
  Grow render_buf = {0};
  /* --- the site --- */
  Slice site, templates, files;
  if (!json_field(input, "site", &site, &site)) site = slice_make(NULL, 0);
  if (!json_field(input, "templates", &templates, &templates)) templates = slice_make(NULL, 0);
  if (!json_field(input, "files", &files, &files)) files = slice_make(NULL, 0);
  Slice v;
  if (site.p && json_field(site, "title", &v, &v)) c.site_title = json_unescape(&c.bump, v);
  if (site.p && json_field(site, "base_url", &v, &v)) {
    Slice raw = json_unescape(&c.bump, v);
    size_t end = raw.n;
    while (end > 0 && raw.p[end - 1] == '/') end--;
    c.base = slice_make(raw.p, end);
  }
  if (site.p && json_field(site, "per_page", &v, &v)) c.per_page = json_number(v);
  if (site.p && json_field(site, "recent", &v, &v)) c.recent = json_number(v);
  if (c.per_page < 1) c.per_page = 1;
  /* --- the templates, parsed in name order --- */
  {
    size_t cap = 16, n = 0;
    NameSrc *arr = (NameSrc *)bump_alloc(&c.bump, sizeof(NameSrc) * cap);
    size_t pos = 1;
    Slice k, val;
    while (json_object_next(templates, &pos, &k, &val)) {
      if (n == cap) {
        size_t nc = cap * 2;
        NameSrc *na = (NameSrc *)bump_alloc(&c.bump, sizeof(NameSrc) * nc);
        memcpy(na, arr, sizeof(NameSrc) * n);
        arr = na;
        cap = nc;
      }
      arr[n].name = json_unescape(&c.bump, k);
      arr[n].src = json_unescape(&c.bump, val);
      n++;
    }
    qsort(arr, n, sizeof(NameSrc), cmp_namesrc);
    for (size_t k2 = 0; k2 < n; k2++) {
      Tpl *tpl = (Tpl *)bump_alloc(&c.bump, sizeof(Tpl));
      Grow err = {0};
      if (tpl_compile(&c.bump, &c.intern, arr[k2].name, arr[k2].src, tpl, &err)) {
        strmap_put(&c.templates, arr[k2].name, tpl, &c.bump);
      } else {
        Slice line = bump_dup(&c.bump, slice_make(err.data, err.len));
        linevec_push(&c.bump, &c.report, line);
      }
      free(err.data);
    }
  }
  /* --- the files, in path order --- */
  size_t n_all = 0, cap_all = 0;
  Post **all_sources = NULL;
  {
    size_t cap = 64, n = 0;
    FileEnt *arr = (FileEnt *)bump_alloc(&c.bump, sizeof(FileEnt) * cap);
    size_t pos = 1;
    Slice elem;
    while (json_array_next(files, &pos, &elem)) {
      /* One streaming pass over the file object (keys arrive with quotes);
       * two json_field calls would re-scan it twice. */
      Slice p = slice_make(NULL, 0), ct = slice_make(NULL, 0);
      size_t fpos = 1;
      Slice fk, fv;
      while (json_object_next(elem, &fpos, &fk, &fv)) {
        if (fk.n == 6 && memcmp(fk.p, "\"path\"", 6) == 0) p = fv;
        else if (fk.n == 9 && memcmp(fk.p, "\"content\"", 9) == 0) ct = fv;
      }
      if (!p.p || !ct.p) continue;
      if (n == cap) {
        size_t nc = cap * 2;
        FileEnt *na = (FileEnt *)bump_alloc(&c.bump, sizeof(FileEnt) * nc);
        memcpy(na, arr, sizeof(FileEnt) * n);
        arr = na;
        cap = nc;
      }
      arr[n].path = json_unescape(&c.bump, p);
      arr[n].content = json_unescape(&c.bump, ct);
      n++;
    }
    qsort(arr, n, sizeof(FileEnt), cmp_fileent);
    c.static_files = (Slice *)bump_alloc(&c.bump, sizeof(Slice) * (n == 0 ? 1 : n));
    cap_all = n == 0 ? 1 : n;
    all_sources = (Post **)bump_alloc(&c.bump, sizeof(Post *) * cap_all);
    for (size_t k2 = 0; k2 < n; k2++) {
      Slice path = arr[k2].path;
      if (path.n >= 3 && memcmp(path.p + path.n - 3, ".md", 3) == 0) {
        Post *s = (Post *)bump_alloc(&c.bump, sizeof(Post));
        memset(s, 0, sizeof(*s));
        s->path = path;
        s->text = crlf_normalize(&c.bump, arr[k2].content);
        all_sources[n_all++] = s;
      } else {
        strset_add(&c.static_paths, path, &c.bump);
        Slice *content_slot = (Slice *)bump_alloc(&c.bump, sizeof(Slice));
        *content_slot = arr[k2].content;
        strmap_put(&c.static_contents, path, content_slot, &c.bump);
        c.static_files[c.n_statics++] = path;
      }
    }
  }
  /* --- front matter --- */
  c.sources = (Post **)bump_alloc(&c.bump, sizeof(Post *) * (n_all == 0 ? 1 : n_all));
  for (size_t k2 = 0; k2 < n_all; k2++) {
    Post *s = all_sources[k2];
    Slice body;
    if (!fm_parse(s->text, &c.bump, &s->fm, &body)) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "refused ");
      grow_add_slice(&c.scratch, s->path);
      grow_add_lit(&c.scratch, ": ");
      grow_add_slice(&c.scratch, s->fm.err);
      linevec_push(&c.bump, &c.report, bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len)));
      continue;
    }
    s->body = body;
    s->is_post = s->path.n >= 6 && memcmp(s->path.p, "posts/", 6) == 0;
    const char *own = NULL;
    if (s->path.n >= 5 && memcmp(s->path.p, "tags/", 5) == 0) own = "tags";
    else if (s->path.n >= 5 && memcmp(s->path.p, "page/", 5) == 0) own = "page";
    if (own) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "refused ");
      grow_add_slice(&c.scratch, s->path);
      grow_add_lit(&c.scratch, ": ");
      grow_add_lit(&c.scratch, own);
      grow_add_lit(&c.scratch, "/ is the generator's own");
      linevec_push(&c.bump, &c.report, bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len)));
      continue;
    }
    if (s->is_post && !s->fm.has_date) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "refused ");
      grow_add_slice(&c.scratch, s->path);
      grow_add_lit(&c.scratch, ": a post needs a date");
      linevec_push(&c.bump, &c.report, bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len)));
      continue;
    }
    Slice layout = s->fm.has_layout ? s->fm.layout : (s->is_post ? slice_make((const unsigned char *)"post", 4) : slice_make((const unsigned char *)"page", 4));
    if (!strmap_get(&c.templates, layout)) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "refused ");
      grow_add_slice(&c.scratch, s->path);
      grow_add_lit(&c.scratch, ": layout \"");
      grow_add_slice(&c.scratch, layout);
      grow_add_lit(&c.scratch, "\" is no template that parses");
      linevec_push(&c.bump, &c.report, bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len)));
      continue;
    }
    s->layout = layout;
    if (s->fm.draft) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "draft ");
      grow_add_slice(&c.scratch, s->path);
      linevec_push(&c.bump, &c.report, bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len)));
      continue;
    }
    c.sources[c.n_sources++] = s;
  }
  /* --- permalinks --- */
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    Post *s = c.sources[k2];
    Slice dir = s->is_post ? slice_make((const unsigned char *)"posts", 5) : dir_of(s->path);
    StrSet *taken = (StrSet *)strmap_get(&c.taken_dirs, dir);
    if (!taken) {
      taken = (StrSet *)bump_alloc(&c.bump, sizeof(StrSet));
      memset(taken, 0, sizeof(*taken));
      if (dir.n == 0) {
        strset_add(taken, slice_make((const unsigned char *)"posts", 5), &c.bump);
        strset_add(taken, slice_make((const unsigned char *)"tags", 4), &c.bump);
        strset_add(taken, slice_make((const unsigned char *)"page", 4), &c.bump);
      }
      strmap_put(&c.taken_dirs, dir, taken, &c.bump);
    }
    Slice base_slug = s->fm.has_slug ? s->fm.slug : slug_of(s->fm.title, &c.slug_scratch);
    s->slug = unique_slug(base_slug, taken, &c.bump, &c.slug_scratch);
    c.scratch.len = 0;
    if (dir.n > 0) {
      grow_add_slice(&c.scratch, dir);
      grow_addc(&c.scratch, '/');
    }
    grow_add_slice(&c.scratch, s->slug);
    s->out_dir = bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len));
    c.scratch.len = 0;
    grow_add_slice(&c.scratch, c.base);
    grow_addc(&c.scratch, '/');
    grow_add_slice(&c.scratch, s->out_dir);
    grow_addc(&c.scratch, '/');
    s->url = bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len));
    strmap_put(&c.by_source, s->path, s, &c.bump);
  }
  /* --- markdown --- */
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    Post *s = c.sources[k2];
    MdCtx mctx;
    StrSet ids = {0};
    LinkUd lu;
    lu.c = &c;
    lu.src = s;
    lu.dir = dir_of(s->path);
    mctx.link_fn = link_resolve;
    mctx.link_ud = &lu;
    mctx.ids = &ids;
    mctx.slug_scratch = &c.slug_scratch;
    mctx.link_scratch = &c.link_scratch;
    MdOut md = md_render(&c.bump, s->body, &mctx);
    s->html = md.html;
    s->excerpt = md.excerpt;
    s->toc = md.toc;
  }
  /* --- collections --- */
  c.posts = (Post **)bump_alloc(&c.bump, sizeof(Post *) * (c.n_sources == 0 ? 1 : c.n_sources));
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    if (c.sources[k2]->is_post) c.posts[c.n_posts++] = c.sources[k2];
  }
  qsort(c.posts, c.n_posts, sizeof(Post *), cmp_post_date_slug);
  for (size_t k2 = 0; k2 < c.n_posts; k2++) {
    c.posts[k2]->newer = k2 == 0 ? NULL : c.posts[k2 - 1];
    c.posts[k2]->older = k2 == c.n_posts - 1 ? NULL : c.posts[k2 + 1];
  }
  /* --- tags --- */
  StrMap by_slug = {0};
  Tag **tag_list = (Tag **)bump_alloc(&c.bump, sizeof(Tag *) * (c.n_posts == 0 ? 1 : c.n_posts * 4));
  size_t n_tag_list = 0;
  for (size_t k2 = 0; k2 < c.n_posts; k2++) {
    Post *p = c.posts[k2];
    size_t capr = 4, nrefs = 0;
    TagRef *refs = (TagRef *)bump_alloc(&c.bump, sizeof(TagRef) * capr);
    for (size_t t = 0; t < p->fm.ntags; t++) {
      Slice name = p->fm.tags[t];
      Slice slug = bump_dup(&c.bump, slug_of(name, &c.slug_scratch));
      int seen = 0;
      for (size_t r = 0; r < nrefs; r++) {
        if (slice_eq(refs[r].slug, slug)) { seen = 1; break; }
      }
      if (seen) continue;
      Tag *tg = (Tag *)strmap_get(&by_slug, slug);
      if (!tg) {
        tg = (Tag *)bump_alloc(&c.bump, sizeof(Tag));
        if (!tg) continue;
        memset(tg, 0, sizeof(*tg));
        tg->name = name;
        tg->slug = slug;
        c.scratch.len = 0;
        grow_add_slice(&c.scratch, c.base);
        grow_add_lit(&c.scratch, "/tags/");
        grow_add_slice(&c.scratch, slug);
        grow_addc(&c.scratch, '/');
        tg->url = bump_dup(&c.bump, slice_make(c.scratch.data, c.scratch.len));
        strmap_put(&by_slug, slug, tg, &c.bump);
        tag_list[n_tag_list++] = tg;
      }
      if (tg->n_posts == tg->cap_posts) {
        size_t nc = tg->cap_posts == 0 ? 8 : tg->cap_posts * 2;
        Post **np = (Post **)bump_alloc(&c.bump, sizeof(Post *) * nc);
        if (tg->posts) memcpy(np, tg->posts, sizeof(Post *) * tg->n_posts);
        tg->posts = np;
        tg->cap_posts = nc;
      }
      tg->posts[tg->n_posts++] = p;
      if (nrefs == capr) {
        size_t nc = capr * 2;
        TagRef *nr = (TagRef *)bump_alloc(&c.bump, sizeof(TagRef) * nc);
        memcpy(nr, refs, sizeof(TagRef) * nrefs);
        refs = nr;
        capr = nc;
      }
      /* The tag reference is the tag object itself: its name is the first
       * spelling met in the posts' order, not this post's own spelling. */
      refs[nrefs].name = tg->name;
      refs[nrefs].slug = slug;
      refs[nrefs].url = tg->url;
      refs[nrefs].tag = tg;
      nrefs++;
    }
    p->tag_refs = refs;
    p->n_tag_refs = nrefs;
  }
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    Post *s = c.sources[k2];
    if (s->is_post) continue;
    size_t capr = 4, nrefs = 0;
    TagRef *refs = (TagRef *)bump_alloc(&c.bump, sizeof(TagRef) * capr);
    for (size_t t = 0; t < s->fm.ntags; t++) {
      Slice name = s->fm.tags[t];
      Slice slug = bump_dup(&c.bump, slug_of(name, &c.slug_scratch));
      int seen = 0;
      for (size_t r = 0; r < nrefs; r++) {
        if (slice_eq(refs[r].slug, slug)) { seen = 1; break; }
      }
      if (seen) continue;
      Tag *tg = (Tag *)strmap_get(&by_slug, slug);
      if (nrefs == capr) {
        size_t nc = capr * 2;
        TagRef *nr = (TagRef *)bump_alloc(&c.bump, sizeof(TagRef) * nc);
        memcpy(nr, refs, sizeof(TagRef) * nrefs);
        refs = nr;
        capr = nc;
      }
      /* A page's tag links to the posts' tag of the same slug (with that
       * tag's first-met name), and to nothing where no post has it. */
      refs[nrefs].name = tg ? tg->name : name;
      refs[nrefs].slug = slug;
      refs[nrefs].url = tg ? tg->url : slice_make((const unsigned char *)"", 0);
      refs[nrefs].tag = tg;
      nrefs++;
    }
    s->tag_refs = refs;
    s->n_tag_refs = nrefs;
  }
  c.tags = tag_list;
  c.n_tags = n_tag_list;
  qsort(c.tags, c.n_tags, sizeof(Tag *), cmp_tag_ptr);
  /* --- layouts --- */
  Bump *b = &c.bump;
  for (size_t k2 = 0; k2 < c.n_posts; k2++) c.posts[k2]->card = card_of(&c, c.posts[k2]);
  Val *site_val = val_rec(b);
  val_rec_set(b, site_val, F_title, val_str(b, c.site_title));
  val_rec_set(b, site_val, F_base_url, val_str(b, c.base));
  Val **tag_items = (Val **)bump_alloc(b, sizeof(Val *) * (c.n_tags == 0 ? 1 : c.n_tags));
  for (size_t k2 = 0; k2 < c.n_tags; k2++) {
    Val *t = val_rec(b);
    val_rec_set(b, t, F_name, val_str(b, c.tags[k2]->name));
    val_rec_set(b, t, F_slug, val_str(b, c.tags[k2]->slug));
    val_rec_set(b, t, F_url, val_str(b, c.tags[k2]->url));
    val_rec_set(b, t, F_count, val_num(b, (int64_t)c.tags[k2]->n_posts));
    tag_items[k2] = t;
  }
  Val *all_tags = val_arr(b, tag_items, c.n_tags);
  size_t n_recent = c.recent < 0 ? 0 : (size_t)c.recent;
  if (n_recent > c.n_posts) n_recent = c.n_posts;
  Val **rec_items = (Val **)bump_alloc(b, sizeof(Val *) * (n_recent == 0 ? 1 : n_recent));
  for (size_t k2 = 0; k2 < n_recent; k2++) rec_items[k2] = c.posts[k2]->card;
  Val *recent_val = val_arr(b, rec_items, n_recent);
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    Post *s = c.sources[k2];
    s->page = page_of(&c, s);
    Scope root;
    root.parent = NULL;
    root.n = 4;
    root.ids[0] = F_site; root.vals[0] = site_val;
    root.ids[1] = F_tags; root.vals[1] = all_tags;
    root.ids[2] = F_recent; root.vals[2] = recent_val;
    root.ids[3] = F_page; root.vals[3] = s->page;
    if (render_named(&c, s->layout, &root, &render_buf)) {
      Slice html = bump_dup(b, slice_make(render_buf.data, render_buf.len));
      c.scratch.len = 0;
      grow_add_slice(&c.scratch, s->out_dir);
      grow_add_lit(&c.scratch, "/index.html");
      Slice opath = bump_dup(b, slice_make(c.scratch.data, c.scratch.len));
      out_set(b, &c.out, opath, html);
    }
  }
  /* Index pages. */
  long per = c.per_page;
  long npages = (long)((c.n_posts + (size_t)per - 1) / (size_t)per);
  if (npages < 1) npages = 1;
  for (long k = 1; k <= npages; k++) {
    size_t from = (size_t)(k - 1) * (size_t)per;
    size_t to = (size_t)k * (size_t)per;
    if (to > c.n_posts) to = c.n_posts;
    size_t cnt = to > from ? to - from : 0;
    Val **items = (Val **)bump_alloc(b, sizeof(Val *) * (cnt == 0 ? 1 : cnt));
    for (size_t j = 0; j < cnt; j++) items[j] = c.posts[from + j]->card;
    Val *posts_val = val_arr(b, items, cnt);
    Val *pager = val_rec(b);
    val_rec_set(b, pager, F_number, val_num(b, k));
    val_rec_set(b, pager, F_total, val_num(b, npages));
    /* indexUrl */
    c.scratch.len = 0;
    grow_add_slice(&c.scratch, c.base);
    if (k == 1) {
      grow_addc(&c.scratch, '/');
    } else {
      grow_add_lit(&c.scratch, "/page/");
      char tmp[32];
      int tl = snprintf(tmp, sizeof(tmp), "%ld", k);
      grow_addn(&c.scratch, tmp, (size_t)tl);
      grow_addc(&c.scratch, '/');
    }
    Slice my_url = bump_dup(b, slice_make(c.scratch.data, c.scratch.len));
    val_rec_set(b, pager, F_url, val_str(b, my_url));
    if (k == 1) {
      val_rec_set(b, pager, F_newer_url, val_str(b, slice_make((const unsigned char *)"", 0)));
    } else {
      /* indexUrl(k-1): page 1 is the root index, base + '/'; later pages
       * are base + '/page/' + (k-1) + '/'. */
      c.scratch.len = 0;
      grow_add_slice(&c.scratch, c.base);
      if (k - 1 == 1) {
        grow_addc(&c.scratch, '/');
      } else {
        grow_add_lit(&c.scratch, "/page/");
        char tmp[32];
        int tl = snprintf(tmp, sizeof(tmp), "%ld", k - 1);
        grow_addn(&c.scratch, tmp, (size_t)tl);
        grow_addc(&c.scratch, '/');
      }
      val_rec_set(b, pager, F_newer_url, val_str(b, bump_dup(b, slice_make(c.scratch.data, c.scratch.len))));
    }
    if (k == npages) {
      val_rec_set(b, pager, F_older_url, val_str(b, slice_make((const unsigned char *)"", 0)));
    } else {
      c.scratch.len = 0;
      grow_add_slice(&c.scratch, c.base);
      grow_add_lit(&c.scratch, "/page/");
      char tmp[32];
      int tl = snprintf(tmp, sizeof(tmp), "%ld", k + 1);
      grow_addn(&c.scratch, tmp, (size_t)tl);
      grow_addc(&c.scratch, '/');
      val_rec_set(b, pager, F_older_url, val_str(b, bump_dup(b, slice_make(c.scratch.data, c.scratch.len))));
    }
    Scope root;
    root.parent = NULL;
    root.n = 5;
    root.ids[0] = F_site; root.vals[0] = site_val;
    root.ids[1] = F_tags; root.vals[1] = all_tags;
    root.ids[2] = F_recent; root.vals[2] = recent_val;
    root.ids[3] = F_posts; root.vals[3] = posts_val;
    root.ids[4] = F_pager; root.vals[4] = pager;
    if (render_named(&c, slice_make((const unsigned char *)"index", 5), &root, &render_buf)) {
      Slice html = bump_dup(b, slice_make(render_buf.data, render_buf.len));
      Slice opath;
      if (k == 1) {
        opath = slice_make((const unsigned char *)"index.html", 10);
      } else {
        c.scratch.len = 0;
        grow_add_lit(&c.scratch, "page/");
        char tmp[32];
        int tl = snprintf(tmp, sizeof(tmp), "%ld", k);
        grow_addn(&c.scratch, tmp, (size_t)tl);
        grow_add_lit(&c.scratch, "/index.html");
        opath = bump_dup(b, slice_make(c.scratch.data, c.scratch.len));
      }
      out_set(b, &c.out, opath, html);
    }
  }
  /* Tag pages. */
  for (size_t k2 = 0; k2 < c.n_tags; k2++) {
    Tag *t = c.tags[k2];
    Val *tag_val = val_rec(b);
    val_rec_set(b, tag_val, F_name, val_str(b, t->name));
    val_rec_set(b, tag_val, F_slug, val_str(b, t->slug));
    val_rec_set(b, tag_val, F_url, val_str(b, t->url));
    val_rec_set(b, tag_val, F_count, val_num(b, (int64_t)t->n_posts));
    Val **items = (Val **)bump_alloc(b, sizeof(Val *) * (t->n_posts == 0 ? 1 : t->n_posts));
    for (size_t j = 0; j < t->n_posts; j++) items[j] = t->posts[j]->card;
    val_rec_set(b, tag_val, F_posts, val_arr(b, items, t->n_posts));
    Scope root;
    root.parent = NULL;
    root.n = 4;
    root.ids[0] = F_site; root.vals[0] = site_val;
    root.ids[1] = F_tags; root.vals[1] = all_tags;
    root.ids[2] = F_recent; root.vals[2] = recent_val;
    root.ids[3] = F_tag; root.vals[3] = tag_val;
    if (render_named(&c, slice_make((const unsigned char *)"tag", 3), &root, &render_buf)) {
      Slice html = bump_dup(b, slice_make(render_buf.data, render_buf.len));
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "tags/");
      grow_add_slice(&c.scratch, t->slug);
      grow_add_lit(&c.scratch, "/index.html");
      Slice opath = bump_dup(b, slice_make(c.scratch.data, c.scratch.len));
      out_set(b, &c.out, opath, html);
    }
  }
  /* The tags index page. */
  {
    Scope root;
    root.parent = NULL;
    root.n = 3;
    root.ids[0] = F_site; root.vals[0] = site_val;
    root.ids[1] = F_tags; root.vals[1] = all_tags;
    root.ids[2] = F_recent; root.vals[2] = recent_val;
    if (render_named(&c, slice_make((const unsigned char *)"tags", 4), &root, &render_buf)) {
      Slice html = bump_dup(b, slice_make(render_buf.data, render_buf.len));
      out_set(b, &c.out, slice_make((const unsigned char *)"tags/index.html", 15), html);
    }
  }
  /* Statics: a built page with the same path shadows the file. */
  for (size_t k2 = 0; k2 < c.n_statics; k2++) {
    Slice path = c.static_files[k2];
    Slice *content = (Slice *)strmap_get(&c.static_contents, path);
    if (strmap_get(&c.out.index, path)) {
      c.scratch.len = 0;
      grow_add_lit(&c.scratch, "shadowed ");
      grow_add_slice(&c.scratch, path);
      grow_add_lit(&c.scratch, ": a built page has its path");
      linevec_push(b, &c.report, bump_dup(b, slice_make(c.scratch.data, c.scratch.len)));
    } else if (content) {
      out_set(b, &c.out, path, *content);
    }
  }
  /* --- the report --- */
  qsort(c.report.lines, c.report.n, sizeof(Slice), cmp_slice_ptr);
  size_t n_pages = 0;
  for (size_t k2 = 0; k2 < c.n_sources; k2++) {
    if (!c.sources[k2]->is_post) n_pages++;
  }
  c.scratch.len = 0;
  {
    char tmp[128];
    int tl = snprintf(tmp, sizeof(tmp), "built %zu pages, %zu posts, %zu tags and %zu static files", n_pages, c.n_posts, c.n_tags, c.n_statics);
    grow_addn(&c.scratch, tmp, (size_t)tl);
  }
  linevec_push(b, &c.report, bump_dup(b, slice_make(c.scratch.data, c.scratch.len)));
  Grow rep = {0};
  for (size_t k2 = 0; k2 < c.report.n; k2++) {
    grow_add_slice(&rep, c.report.lines[k2]);
    grow_addc(&rep, '\n');
  }
  out_set(b, &c.out, slice_make((const unsigned char *)"build-report.txt", 16), bump_dup(b, slice_make(rep.data, rep.len)));
  free(rep.data);
  /* --- serialise the output tree into the answers array --- */
  int first = 1;
  for (size_t k2 = 0; k2 < c.out.n; k2++) {
    if (!first) grow_addc(answers, ',');
    first = 0;
    grow_addc(answers, '"');
    grow_add_json_esc(answers, c.out.entries[k2].path);
    grow_add_lit(answers, "\":\"");
    grow_add_json_esc(answers, c.out.entries[k2].content);
    grow_addc(answers, '"');
  }
  free(render_buf.data);
  free(c.scratch.data);
  free(c.slug_scratch.data);
  free(c.link_scratch.data);
  bump_free_all(&c.bump);
}

/* The entry point: `data` is the raw cases.json bytes; the answers.json
 * bytes are appended to `out`. */
static void build_all(const unsigned char *data, size_t len, Grow *out) {
  Slice top = slice_make(data, len);
  size_t pos = json_ws(top.p, top.n, 0);
  if (pos < top.n && top.p[pos] == '[') pos++;
  grow_addc(out, '[');
  int first = 1;
  Slice elem;
  while (json_array_next(top, &pos, &elem)) {
    Slice input;
    if (!json_field(elem, "input", &input, &input)) continue;
    if (!first) grow_addc(out, ',');
    first = 0;
    grow_addc(out, '{');
    build_case(input, out);
    grow_addc(out, '}');
  }
  grow_addc(out, ']');
}

/* The public entry for the N-API glue: the answers.json bytes for `data`,
 * malloc'd; the caller frees the buffer. */
unsigned char *build_all_alloc(const unsigned char *data, size_t len, size_t *out_len) {
  Grow out = {0};
  build_all(data, len, &out);
  *out_len = out.len;
  return out.data;
}
