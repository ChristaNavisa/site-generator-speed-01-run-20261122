// sitegen.c — a fresh, from-scratch C implementation of makefun-site 1.0.0's
// build (see the kit's vendor README + lib/*.js, the reference). Single-pass,
// zero-copy JSON reader, bump arena, fused Markdown, SIMD-free scalar core.
// Written fresh in this session; it shares concepts with the r3 engine
// (streaming, arena, single-pass) but no line is copied from it.
//
// Usage: sitegen-cli reads $MAKEFUN_INPUT_DIR/cases.json (default /input) and
// writes $MAKEFUN_OUTPUT_DIR/answers.json (via a temp file + rename).
// Exit 0 on success, non-zero on any failure (the driver then falls back).
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "slugtab.h"

// ---------------------------------------------------------------- arena
#define ARENA_BLOCK (1u << 24) // 16 MiB
typedef struct ABlock { struct ABlock *next; size_t used, cap; char data[]; } ABlock;
static ABlock *arena_head;
static void *arena_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (!arena_head || arena_head->used + n > arena_head->cap) {
    size_t cap = ARENA_BLOCK > n ? ARENA_BLOCK : n;
    ABlock *b = (ABlock *)malloc(sizeof(ABlock) + cap);
    if (!b) { fprintf(stderr, "sitegen: out of memory\n"); exit(1); }
    b->next = arena_head; b->used = 0; b->cap = cap; arena_head = b;
  }
  void *p = arena_head->data + arena_head->used;
  arena_head->used += n;
  return p;
}
static char *arena_dup(const char *p, size_t n) {
  char *d = (char *)arena_alloc(n + 1);
  memcpy(d, p, n); d[n] = 0;
  return d;
}

// ---------------------------------------------------------------- Str (byte string, UTF-8/CESU-8, not necessarily NUL-terminated)
typedef struct { const char *p; size_t n; } Str;
static Str str_new(const char *p, size_t n) { Str s; s.p = p; s.n = n; return s; }
static Str str_lit(const char *p) { return str_new(p, strlen(p)); }
static int str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static int str_eq_lit(Str a, const char *b) { size_t n = strlen(b); return a.n == n && memcmp(a.p, b, n) == 0; }
static int str_has_prefix(Str s, const char *pre) {
  size_t n = strlen(pre);
  return s.n >= n && memcmp(s.p, pre, n) == 0;
}
static int str_has_suffix(Str s, const char *suf) {
  size_t n = strlen(suf);
  return s.n >= n && memcmp(s.p + s.n - n, suf, n) == 0;
}

// ---------------------------------------------------------------- StrBuf (malloc-backed, grows)
typedef struct { char *buf; size_t len, cap; } StrBuf;
static void sb_reserve(StrBuf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    b->buf = (char *)realloc(b->buf, nc);
    if (!b->buf) { fprintf(stderr, "sitegen: out of memory\n"); exit(1); }
    b->cap = nc;
  }
}
static void sb_putn(StrBuf *b, const char *p, size_t n) {
  sb_reserve(b, n);
  memcpy(b->buf + b->len, p, n);
  b->len += n;
  b->buf[b->len] = 0;
}
static void sb_put(StrBuf *b, Str s) { sb_putn(b, s.p, s.n); }
static void sb_putc(StrBuf *b, char c) { sb_putn(b, &c, 1); }
static void sb_puts(StrBuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
static Str sb_str(StrBuf *b) { return str_new(b->buf ? b->buf : "", b->len); }
static void sb_free(StrBuf *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }

// ---------------------------------------------------------------- UTF-8 / CESU-8
// Decode one code point; CESU-8 surrogate triples decode to 0xD800-0xDFFF.
// Invalid bytes decode as their own byte value (single-unit fallback).
static uint32_t u8dec(const unsigned char *s, size_t n, size_t *adv) {
  unsigned char c = s[0];
  if (c < 0x80) { *adv = 1; return c; }
  if (c >= 0xC2 && c <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
    *adv = 2; return ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
  }
  if (c >= 0xE0 && c <= 0xEF && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *adv = 3;
    return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  if (c >= 0xF0 && c <= 0xF4 && n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    *adv = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  }
  *adv = 1;
  return c;
}
static size_t u8enc(uint32_t cp, char *out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) { // CESU-8: lone surrogate as 3 bytes
    out[0] = (char)0xED; out[1] = (char)(0xA0 | ((cp >> 6) & 0x0F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

// JS \s (WhiteSpace + LineTerminator) on a code point.
static int js_space(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
      return 1;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

// Compare two strings in UTF-16 code-unit order (JS `<` on strings).
static int cmp_u16(Str a, Str b) {
  size_t ia = 0, ib = 0;
  while (ia < a.n && ib < b.n) {
    uint32_t ca, cb;
    size_t aa, ab;
    ca = u8dec((const unsigned char *)a.p + ia, a.n - ia, &aa);
    cb = u8dec((const unsigned char *)b.p + ib, b.n - ib, &ab);
    uint16_t ua[2], ub[2];
    int na, nb;
    if (ca >= 0x10000) { ua[0] = (uint16_t)(0xD800 + ((ca - 0x10000) >> 10)); ua[1] = (uint16_t)(0xDC00 + ((ca - 0x10000) & 0x3FF)); na = 2; }
    else { ua[0] = (uint16_t)ca; na = 1; }
    if (cb >= 0x10000) { ub[0] = (uint16_t)(0xD800 + ((cb - 0x10000) >> 10)); ub[1] = (uint16_t)(0xDC00 + ((cb - 0x10000) & 0x3FF)); nb = 2; }
    else { ub[0] = (uint16_t)cb; nb = 1; }
    for (int k = 0; k < na && k < nb; k++) {
      if (ua[k] != ub[k]) return ua[k] < ub[k] ? -1 : 1;
    }
    if (na != nb) return na < nb ? -1 : 1;
    ia += aa; ib += ab;
  }
  if (ia < a.n) return 1;
  if (ib < b.n) return -1;
  return 0;
}

// ---------------------------------------------------------------- HTML escape (& < > " ' only)
static void escape_html(StrBuf *out, Str s) {
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    switch (c) {
      case '&': sb_puts(out, "&amp;"); break;
      case '<': sb_puts(out, "&lt;"); break;
      case '>': sb_puts(out, "&gt;"); break;
      case '"': sb_puts(out, "&quot;"); break;
      case '\'': sb_puts(out, "&#39;"); break;
      default: sb_putc(out, c); break;
    }
  }
}
static void escape_html_cstr(StrBuf *out, const char *s) { escape_html(out, str_lit(s)); }

// ---------------------------------------------------------------- slugify (JS text.js, via slugtab.h)
static const char *slug_plain(uint32_t cp) {
  size_t lo = 0, hi = sizeof(SLUG_TAB) / sizeof(SLUG_TAB[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (SLUG_TAB[mid].cp < cp) lo = mid + 1;
    else if (SLUG_TAB[mid].cp > cp) hi = mid;
    else return SLUG_TAB[mid].plain;
  }
  return NULL;
}
static void slug_feed(StrBuf *out, int *gap, const char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint32_t cp;
    size_t adv;
    cp = u8dec((const unsigned char *)p + i, n - i, &adv);
    int keep = -1;
    if (cp >= 'A' && cp <= 'Z') keep = (int)(cp - 'A' + 'a');
    else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) keep = (int)cp;
    if (keep < 0) {
      if (out->len > 0) *gap = 1;
    } else {
      if (*gap) { sb_putc(out, '-'); *gap = 0; }
      sb_putc(out, (char)keep);
    }
    i += adv;
  }
}
static Str slugify(Str text) {
  StrBuf b = {0};
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    uint32_t cp;
    size_t adv;
    cp = u8dec((const unsigned char *)text.p + i, text.n - i, &adv);
    if (cp < 128) {
      char c = (char)cp;
      int keep = -1;
      if (c >= 'A' && c <= 'Z') keep = c - 'A' + 'a';
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
      if (keep < 0) {
        if (b.len > 0) gap = 1;
      } else {
        if (gap) { sb_putc(&b, '-'); gap = 0; }
        sb_putc(&b, (char)keep);
      }
    } else {
      const char *plain = slug_plain(cp);
      if (plain) slug_feed(&b, &gap, plain, strlen(plain));
      else if (b.len > 0) gap = 1;
    }
    i += adv;
  }
  if (b.len > 64) b.len = 64;
  while (b.len > 0 && b.buf[b.len - 1] == '-') b.len--;
  if (b.len == 0) { sb_free(&b); return str_lit("untitled"); }
  return sb_str(&b);
}

// ---------------------------------------------------------------- JSON.stringify string escaping
static void json_escape(StrBuf *out, Str s) {
  static const char HEX[] = "0123456789abcdef";
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = (unsigned char)s.p[i];
    if (c < 0x20) {
      switch (c) {
        case '\b': sb_puts(out, "\\b"); break;
        case '\f': sb_puts(out, "\\f"); break;
        case '\n': sb_puts(out, "\\n"); break;
        case '\r': sb_puts(out, "\\r"); break;
        case '\t': sb_puts(out, "\\t"); break;
        default: {
          char tmp[7] = { '\\', 'u', '0', '0', HEX[(c >> 4) & 0xF], HEX[c & 0xF], 0 };
          sb_puts(out, tmp);
          break;
        }
      }
      i++;
    } else if (c == '"') { sb_puts(out, "\\\""); i++; }
    else if (c == '\\') { sb_puts(out, "\\\\"); i++; }
    else if (c == 0xED && i + 2 < s.n && (unsigned char)s.p[i + 1] >= 0xA0 && (unsigned char)s.p[i + 1] <= 0xBF && ((unsigned char)s.p[i + 2] & 0xC0) == 0x80) {
      // CESU-8 lone surrogate -> \udXXX (JSON.stringify escapes lone surrogates)
      uint32_t cp = 0xD800 + (((unsigned char)s.p[i + 1] & 0x0F) << 6) + ((unsigned char)s.p[i + 2] & 0x3F);
      char tmp[7] = { '\\', 'u', 'd', HEX[(cp >> 8) & 0xF], HEX[(cp >> 4) & 0xF], HEX[cp & 0xF], 0 };
      sb_puts(out, tmp);
      i += 3;
    } else {
      sb_putc(out, (char)c);
      i++;
    }
  }
}

// ---------------------------------------------------------------- JS-like values (arena)
typedef enum { V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VType;
typedef struct Val Val;
typedef struct { Str key; Val *val; } KV;
struct Val {
  VType t;
  union {
    int boolean;
    struct { double d; long long i; int isInt; } num;
    Str s;
    struct { Val **items; size_t len, cap; } arr;
    struct { KV *kvs; size_t len, cap; } obj;
  } u;
};
static Val *val_new(VType t) {
  Val *v = (Val *)arena_alloc(sizeof(Val));
  memset(v, 0, sizeof(*v));
  v->t = t;
  return v;
}
static Val *val_null(void) { return val_new(V_NULL); }
static Val *val_bool(int b) { Val *v = val_new(V_BOOL); v->u.boolean = !!b; return v; }
static Val *val_int(long long i) { Val *v = val_new(V_NUM); v->u.num.i = i; v->u.num.d = (double)i; v->u.num.isInt = 1; return v; }
static Val *val_dbl(double d) { Val *v = val_new(V_NUM); v->u.num.d = d; v->u.num.isInt = 0; return v; }
static Val *val_str(Str s) { Val *v = val_new(V_STR); v->u.s = s; return v; }
static Val *val_arr(void) { return val_new(V_ARR); }
static Val *val_obj(void) { return val_new(V_OBJ); }
static void val_arr_push(Val *a, Val *item) {
  if (a->u.arr.len == a->u.arr.cap) {
    size_t nc = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
    Val **ni = (Val **)arena_alloc(nc * sizeof(Val *));
    if (a->u.arr.items) memcpy(ni, a->u.arr.items, a->u.arr.len * sizeof(Val *));
    a->u.arr.items = ni; a->u.arr.cap = nc;
  }
  a->u.arr.items[a->u.arr.len++] = item;
}
static void val_obj_set(Val *o, Str key, Val *v) {
  if (o->u.obj.len == o->u.obj.cap) {
    size_t nc = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
    KV *nk = (KV *)arena_alloc(nc * sizeof(KV));
    if (o->u.obj.kvs) memcpy(nk, o->u.obj.kvs, o->u.obj.len * sizeof(KV));
    o->u.obj.kvs = nk; o->u.obj.cap = nc;
  }
  o->u.obj.kvs[o->u.obj.len].key = key;
  o->u.obj.kvs[o->u.obj.len].val = v;
  o->u.obj.len++;
}
// Last matching key wins (JS object semantics).
static Val *val_obj_get(Val *o, Str key) {
  Val *found = NULL;
  if (!o || o->t != V_OBJ) return NULL;
  for (size_t i = 0; i < o->u.obj.len; i++) {
    if (str_eq(o->u.obj.kvs[i].key, key)) found = o->u.obj.kvs[i].val;
  }
  return found;
}
static Val *val_obj_get_lit(Val *o, const char *key) { return val_obj_get(o, str_lit(key)); }
static int val_truthy(Val *v) {
  if (!v || v->t == V_NULL) return 0;
  switch (v->t) {
    case V_BOOL: return v->u.boolean;
    case V_NUM: return v->u.num.d != 0;
    case V_STR: return v->u.s.n > 0;
    case V_ARR: return v->u.arr.len > 0;
    case V_OBJ: return 1;
    default: return 0;
  }
}
// JS String(v) for the values templates can see (all numbers here are ints).
static void val_text(StrBuf *out, Val *v) {
  if (!v || v->t == V_NULL) return;
  switch (v->t) {
    case V_STR: sb_put(out, v->u.s); break;
    case V_BOOL: sb_puts(out, v->u.boolean ? "true" : "false"); break;
    case V_NUM: {
      if (v->u.num.isInt) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", v->u.num.i);
        sb_puts(out, tmp);
      } else {
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%.17g", v->u.num.d);
        sb_puts(out, tmp);
      }
      break;
    }
    case V_ARR:
      for (size_t i = 0; i < v->u.arr.len; i++) {
        if (i) sb_puts(out, ", ");
        val_text(out, v->u.arr.items[i]);
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- JSON parser (single pass, arena DOM)
typedef struct { const char *p; size_t n; size_t i; int err; } JParser;
static void j_skip(JParser *j) {
  while (j->i < j->n && (j->p[j->i] == ' ' || j->p[j->i] == '\t' || j->p[j->i] == '\n' || j->p[j->i] == '\r')) j->i++;
}
static int j_hex4(JParser *j, uint32_t *out) {
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    if (j->i >= j->n) return 0;
    char c = j->p[j->i++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}
static Str j_parse_string(JParser *j) {
  // j->p[j->i] == '"'. Returns decoded string (arena). On error sets j->err.
  j->i++; // opening quote
  const char *start = j->p + j->i;
  size_t k = j->i;
  int has_esc = 0;
  while (k < j->n) {
    unsigned char c = (unsigned char)j->p[k];
    if (c == '"') break;
    if (c == '\\') { has_esc = 1; k += 2; continue; }
    if (c < 0x20) { j->err = 1; return str_new("", 0); }
    k++;
  }
  if (k >= j->n) { j->err = 1; return str_new("", 0); }
  if (!has_esc) {
    Str s = str_new(start, k - j->i);
    j->i = k + 1;
    return s;
  }
  StrBuf b = {0};
  while (j->i < k) {
    unsigned char c = (unsigned char)j->p[j->i];
    if (c != '\\') { sb_putc(&b, (char)c); j->i++; continue; }
    j->i++; // backslash
    if (j->i >= k) { j->err = 1; break; }
    char e = j->p[j->i++];
    switch (e) {
      case '"': sb_putc(&b, '"'); break;
      case '\\': sb_putc(&b, '\\'); break;
      case '/': sb_putc(&b, '/'); break;
      case 'b': sb_putc(&b, '\b'); break;
      case 'f': sb_putc(&b, '\f'); break;
      case 'n': sb_putc(&b, '\n'); break;
      case 'r': sb_putc(&b, '\r'); break;
      case 't': sb_putc(&b, '\t'); break;
      case 'u': {
        uint32_t cp;
        if (!j_hex4(j, &cp)) { j->err = 1; break; }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // possible surrogate pair
          if (j->i + 6 <= k && j->p[j->i] == '\\' && j->p[j->i + 1] == 'u') {
            size_t save = j->i;
            j->i += 2;
            uint32_t lo;
            if (j_hex4(j, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              j->i = save; // lone high surrogate -> CESU-8
            }
          }
        }
        char tmp[4];
        size_t w = u8enc(cp, tmp);
        sb_putn(&b, tmp, w);
        break;
      }
      default: j->err = 1; break;
    }
    if (j->err) break;
  }
  j->i = k + 1; // closing quote
  Str s = str_new(b.buf ? b.buf : "", b.len);
  return s;
}
static Val *j_parse_value(JParser *j);
static Val *j_parse_number(JParser *j) {
  size_t start = j->i;
  if (j->i < j->n && j->p[j->i] == '-') j->i++;
  while (j->i < j->n && isdigit((unsigned char)j->p[j->i])) j->i++;
  int isInt = 1;
  if (j->i < j->n && j->p[j->i] == '.') {
    isInt = 0;
    j->i++;
    while (j->i < j->n && isdigit((unsigned char)j->p[j->i])) j->i++;
  }
  if (j->i < j->n && (j->p[j->i] == 'e' || j->p[j->i] == 'E')) {
    isInt = 0;
    j->i++;
    if (j->i < j->n && (j->p[j->i] == '+' || j->p[j->i] == '-')) j->i++;
    while (j->i < j->n && isdigit((unsigned char)j->p[j->i])) j->i++;
  }
  char tmp[64];
  size_t len = j->i - start;
  if (len >= sizeof(tmp)) { j->err = 1; return val_null(); }
  memcpy(tmp, j->p + start, len);
  tmp[len] = 0;
  if (isInt) {
    char *end = NULL;
    long long v = strtoll(tmp, &end, 10);
    if (end && *end == 0) return val_int(v);
  }
  return val_dbl(strtod(tmp, NULL));
}
static Val *j_parse_value(JParser *j) {
  j_skip(j);
  if (j->i >= j->n) { j->err = 1; return val_null(); }
  char c = j->p[j->i];
  if (c == '{') {
    j->i++;
    Val *o = val_obj();
    j_skip(j);
    if (j->i < j->n && j->p[j->i] == '}') { j->i++; return o; }
    while (1) {
      j_skip(j);
      if (j->i >= j->n || j->p[j->i] != '"') { j->err = 1; return o; }
      Str key = j_parse_string(j);
      if (j->err) return o;
      j_skip(j);
      if (j->i >= j->n || j->p[j->i] != ':') { j->err = 1; return o; }
      j->i++;
      Val *v = j_parse_value(j);
      if (j->err) return o;
      val_obj_set(o, key, v);
      j_skip(j);
      if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
      if (j->i < j->n && j->p[j->i] == '}') { j->i++; return o; }
      j->err = 1;
      return o;
    }
  }
  if (c == '[') {
    j->i++;
    Val *a = val_arr();
    j_skip(j);
    if (j->i < j->n && j->p[j->i] == ']') { j->i++; return a; }
    while (1) {
      Val *v = j_parse_value(j);
      if (j->err) return a;
      val_arr_push(a, v);
      j_skip(j);
      if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
      if (j->i < j->n && j->p[j->i] == ']') { j->i++; return a; }
      j->err = 1;
      return a;
    }
  }
  if (c == '"') {
    Str s = j_parse_string(j);
    return val_str(s);
  }
  if (c == 't') {
    if (j->i + 4 <= j->n && memcmp(j->p + j->i, "true", 4) == 0) { j->i += 4; return val_bool(1); }
    j->err = 1; return val_null();
  }
  if (c == 'f') {
    if (j->i + 5 <= j->n && memcmp(j->p + j->i, "false", 5) == 0) { j->i += 5; return val_bool(0); }
    j->err = 1; return val_null();
  }
  if (c == 'n') {
    if (j->i + 4 <= j->n && memcmp(j->p + j->i, "null", 4) == 0) { j->i += 4; return val_null(); }
    j->err = 1; return val_null();
  }
  if (c == '-' || isdigit((unsigned char)c)) return j_parse_number(j);
  j->err = 1;
  return val_null();
}

// ---------------------------------------------------------------- lines
typedef struct { Str *lines; size_t n; } Lines;
static Lines split_lines(Str text) {
  size_t count = 1;
  for (size_t i = 0; i < text.n; i++) if (text.p[i] == '\n') count++;
  Str *ls = (Str *)arena_alloc(count * sizeof(Str));
  size_t n = 0, start = 0;
  for (size_t i = 0; i <= text.n; i++) {
    if (i == text.n || text.p[i] == '\n') {
      ls[n++] = str_new(text.p + start, i - start);
      start = i + 1;
    }
  }
  Lines r; r.lines = ls; r.n = n;
  return r;
}
// JS trim(): strip \s (JS whitespace) from both ends of a byte string.
static Str js_trim(Str s) {
  size_t i = 0, j = s.n;
  while (i < j) {
    uint32_t cp; size_t adv;
    cp = u8dec((const unsigned char *)s.p + i, j - i, &adv);
    if (!js_space(cp)) break;
    i += adv;
  }
  while (j > i) {
    // walk back one code point: find start of last char
    size_t k = j - 1;
    while (k > i && ((unsigned char)s.p[k] & 0xC0) == 0x80) k--;
    uint32_t cp; size_t adv;
    cp = u8dec((const unsigned char *)s.p + k, j - k, &adv);
    (void)adv;
    if (!js_space(cp)) break;
    j = k;
  }
  return str_new(s.p + i, j - i);
}
static int js_is_blank(Str s) { return js_trim(s).n == 0; }
// trimLine / BLANK in markdown.js: spaces and tabs ONLY (not full JS whitespace).
static Str st_trim(Str s) {
  size_t i = 0, j = s.n;
  while (i < j && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
  while (j > i && (s.p[j - 1] == ' ' || s.p[j - 1] == '\t')) j--;
  return str_new(s.p + i, j - i);
}
static int st_blank(Str s) { return st_trim(s).n == 0; }

// ---------------------------------------------------------------- dates (text.js)
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int y, int m) {
  static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2) return is_leap(y) ? 29 : 28;
  return D[m - 1];
}
static int valid_date(int y, int m, int d) {
  return y >= 1970 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m);
}
static const char *MONTHS[12] = { "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December" };
// "2026-11-09" -> "9 November 2026" (from the string alone).
static Str date_long(Str date) {
  int y = 0, m = 0, d = 0;
  if (date.n == 10) {
    y = (date.p[0] - '0') * 1000 + (date.p[1] - '0') * 100 + (date.p[2] - '0') * 10 + (date.p[3] - '0');
    m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
    d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  }
  char *buf = (char *)arena_alloc(64);
  int w = snprintf(buf, 64, "%d %s %d", d, (m >= 1 && m <= 12) ? MONTHS[m - 1] : "", y);
  return str_new(buf, (size_t)w);
}

// ---------------------------------------------------------------- front matter (lib/frontmatter.js)
typedef struct {
  Val *data;
  Str body;
  int ok;
  char err[1024];
} FMResult;

static const char *FM_KEYS[7] = { "title", "summary", "date", "tags", "draft", "layout", "slug" };
static int fm_key_index(Str k) {
  for (int i = 0; i < 7; i++) if (str_eq_lit(k, FM_KEYS[i])) return i;
  return -1;
}

// stringOf: returns 1 ok / 0 error (err filled with the reason, no key prefix).
static int fm_string_of(Str raw, Str *out, char *err, size_t errn) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      snprintf(err, errn, "a quoted string is not closed");
      return 0;
    }
    StrBuf b = {0};
    size_t i = 1;
    while (i < raw.n - 1) {
      char c = raw.p[i];
      if (c == '\\') {
        char nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.n - 1) {
          snprintf(err, errn, "a quoted string holds an escape other than \\\" or \\\\");
          sb_free(&b);
          return 0;
        }
        sb_putc(&b, nx);
        i += 2;
      } else if (c == '"') {
        snprintf(err, errn, "a quoted string holds a stray quote");
        sb_free(&b);
        return 0;
      } else {
        sb_putc(&b, c);
        i++;
      }
    }
    *out = sb_str(&b);
    return 1;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    snprintf(err, errn, "a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}

static FMResult parse_front_matter(Str text) {
  FMResult r;
  memset(&r, 0, sizeof(r));
  Lines ls = split_lines(text);
  if (ls.n == 0 || !str_eq_lit(ls.lines[0], "---")) {
    snprintf(r.err, sizeof(r.err), "no front matter");
    return r;
  }
  size_t close = (size_t)-1;
  for (size_t i = 1; i < ls.n; i++) {
    if (str_eq_lit(ls.lines[i], "---")) { close = i; break; }
  }
  if (close == (size_t)-1) {
    snprintf(r.err, sizeof(r.err), "the front matter is not closed");
    return r;
  }
  Val *data = val_obj();
  int seen[7] = { 0, 0, 0, 0, 0, 0, 0 };
  int has_title = 0;
  for (size_t i = 1; i < close; i++) {
    Str line = ls.lines[i];
    // blank (spaces/tabs only) or comment (opens on '#')
    int blank = 1;
    for (size_t k = 0; k < line.n; k++) {
      if (line.p[k] != ' ' && line.p[k] != '\t') { blank = 0; break; }
    }
    if (blank) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    // LINE /^([a-z_]+):(?: (.*))?$/: after the colon, either end of line or one
    // space + rest; anything else (e.g. "key:x") fails the line.
    size_t k = 0;
    while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_')) k++;
    if (k == 0 || k >= line.n || line.p[k] != ':' || (k + 1 < line.n && line.p[k + 1] != ' ')) {
      snprintf(r.err, sizeof(r.err), "line %zu is not \"key: value\"", i + 1);
      return r;
    }
    Str key = str_new(line.p, k);
    Str raw;
    if (k + 1 < line.n && line.p[k + 1] == ' ') raw = str_new(line.p + k + 2, line.n - (k + 2));
    else raw = str_new(line.p + line.n, 0);
    // drop trailing spaces/tabs
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    int ki = fm_key_index(key);
    if (ki < 0) {
      snprintf(r.err, sizeof(r.err), "unknown key \"%.*s\"", (int)key.n, key.p);
      return r;
    }
    if (seen[ki]) {
      snprintf(r.err, sizeof(r.err), "key \"%.*s\" is given twice", (int)key.n, key.p);
      return r;
    }
    seen[ki] = 1;
    if (raw.n == 0) {
      snprintf(r.err, sizeof(r.err), "key \"%.*s\" has no value", (int)key.n, key.p);
      return r;
    }
    char verr[512] = { 0 };
    if (ki == 0 || ki == 1) { // title, summary
      Str v;
      if (!fm_string_of(raw, &v, verr, sizeof(verr))) {
        snprintf(r.err, sizeof(r.err), "key \"%.*s\": %s", (int)key.n, key.p, verr);
        return r;
      }
      val_obj_set(data, key, val_str(v));
      if (ki == 0) has_title = 1;
    } else if (ki == 2) { // date
      int ok = 0, y = 0, m = 0, d = 0;
      if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-' &&
          isdigit((unsigned char)raw.p[0]) && isdigit((unsigned char)raw.p[1]) &&
          isdigit((unsigned char)raw.p[2]) && isdigit((unsigned char)raw.p[3]) &&
          isdigit((unsigned char)raw.p[5]) && isdigit((unsigned char)raw.p[6]) &&
          isdigit((unsigned char)raw.p[8]) && isdigit((unsigned char)raw.p[9])) {
        y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
        m = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
        d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        ok = valid_date(y, m, d);
      }
      if (!ok) {
        snprintf(r.err, sizeof(r.err), "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
        return r;
      }
      val_obj_set(data, key, val_str(raw));
    } else if (ki == 3) { // tags
      if (!(raw.n >= 2 && raw.p[0] == '[' && raw.p[raw.n - 1] == ']')) {
        snprintf(r.err, sizeof(r.err), "key \"tags\": not a list [a, b]");
        return r;
      }
      Str inner = str_new(raw.p + 1, raw.n - 2);
      Val *arr = val_arr();
      if (js_trim(inner).n > 0) {
        size_t start = 0;
        for (size_t j = 0; j <= inner.n; j++) {
          if (j == inner.n || inner.p[j] == ',') {
            Str piece = js_trim(str_new(inner.p + start, j - start));
            int bad = piece.n == 0;
            for (size_t q = 0; q < piece.n && !bad; q++) {
              char c = piece.p[q];
              if (c == '"' || c == '[' || c == ']') bad = 1;
            }
            if (bad) {
              snprintf(r.err, sizeof(r.err), "key \"tags\": an empty tag, or one holding \" [ or ]");
              return r;
            }
            val_arr_push(arr, val_str(piece));
            start = j + 1;
          }
        }
      }
      val_obj_set(data, key, arr);
    } else if (ki == 4) { // draft
      if (str_eq_lit(raw, "true")) val_obj_set(data, key, val_bool(1));
      else if (str_eq_lit(raw, "false")) val_obj_set(data, key, val_bool(0));
      else {
        snprintf(r.err, sizeof(r.err), "key \"draft\": true or false");
        return r;
      }
    } else if (ki == 5) { // layout
      int ok = raw.n > 0;
      for (size_t q = 0; q < raw.n && ok; q++) {
        char c = raw.p[q];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
      }
      if (!ok) {
        snprintf(r.err, sizeof(r.err), "key \"layout\": not a template name");
        return r;
      }
      val_obj_set(data, key, val_str(raw));
    } else { // slug
      int ok = raw.n > 0 && raw.n <= 64;
      if (ok) {
        size_t q = 0;
        // ^[a-z0-9]+(?:-[a-z0-9]+)*$
        while (q < raw.n && ((raw.p[q] >= 'a' && raw.p[q] <= 'z') || (raw.p[q] >= '0' && raw.p[q] <= '9'))) q++;
        while (q < raw.n && ok) {
          if (raw.p[q] != '-') { ok = 0; break; }
          q++;
          size_t start = q;
          while (q < raw.n && ((raw.p[q] >= 'a' && raw.p[q] <= 'z') || (raw.p[q] >= '0' && raw.p[q] <= '9'))) q++;
          if (q == start) ok = 0;
        }
      }
      if (!ok) {
        snprintf(r.err, sizeof(r.err), "key \"slug\": not a slug");
        return r;
      }
      val_obj_set(data, key, val_str(raw));
    }
  }
  if (!has_title) {
    snprintf(r.err, sizeof(r.err), "no title");
    return r;
  }
  // body = lines after the closing '---', joined by '\n'
  StrBuf b = {0};
  for (size_t i = close + 1; i < ls.n; i++) {
    if (i > close + 1) sb_putc(&b, '\n');
    sb_put(&b, ls.lines[i]);
  }
  r.data = data;
  r.body = sb_str(&b);
  r.ok = 1;
  return r;
}

// ---------------------------------------------------------------- Markdown (lib/markdown.js)
typedef Str (*LinkFn)(void *ctx, Str target);

typedef struct { int level; Str id; Str html; } TocEnt;
typedef struct {
  Str html;
  Str excerpt;
  int have_excerpt;
  TocEnt *toc;
  size_t ntoc, captoc;
  Str *used_ids;
  size_t nused, capused;
} MdResult;

static int is_escapable(char c) {
  return c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == ']' ||
         c == '(' || c == ')' || c == '#' || c == '+' || c == '-' || c == '.' ||
         c == '!' || c == '>';
}
static void esc_char(StrBuf *out, char c) {
  char tmp[2] = { c, 0 };
  escape_html(out, str_new(tmp, 1));
}
#define MD_NONE ((size_t)-1)
static size_t find_unescaped(Str s, size_t from, const char *needle, size_t nlen) {
  for (size_t i = from; i < s.n; i++) {
    if (s.p[i] == '\\' && i + 1 < s.n && is_escapable(s.p[i + 1])) { i++; continue; }
    if (i + nlen <= s.n && memcmp(s.p + i, needle, nlen) == 0) return i;
  }
  return MD_NONE;
}
static size_t find_single_star(Str s, size_t from) {
  // JS: only the NEXT char matters; a '*' followed by '*' is skipped as a pair.
  for (size_t i = from; i < s.n; i++) {
    if (s.p[i] == '\\' && i + 1 < s.n && is_escapable(s.p[i + 1])) { i++; continue; }
    if (s.p[i] != '*') continue;
    if (i + 1 < s.n && s.p[i + 1] == '*') { i++; continue; }
    return i;
  }
  return MD_NONE;
}
static void md_inline(Str s, LinkFn link, void *lctx, StrBuf *out);

static void md_inline(Str s, LinkFn link, void *lctx, StrBuf *out) {
  size_t i = 0;
  while (i < s.n) {
    char c = s.p[i];
    if (c == '\\' && i + 1 < s.n && is_escapable(s.p[i + 1])) {
      esc_char(out, s.p[i + 1]);
      i += 2;
      continue;
    }
    if (c == '`') {
      size_t j = i + 1;
      while (j < s.n && s.p[j] != '`') j++;
      if (j < s.n && j > i + 1) {
        sb_puts(out, "<code>");
        escape_html(out, str_new(s.p + i + 1, j - (i + 1)));
        sb_puts(out, "</code>");
        i = j + 1;
        continue;
      }
      esc_char(out, c);
      i++;
      continue;
    }
    if (c == '*' && i + 1 < s.n && s.p[i + 1] == '*') {
      size_t j = find_unescaped(s, i + 2, "**", 2);
      if (j != MD_NONE && j > i + 2) {
        sb_puts(out, "<strong>");
        md_inline(str_new(s.p + i + 2, j - (i + 2)), link, lctx, out);
        sb_puts(out, "</strong>");
        i = j + 2;
        continue;
      }
      sb_puts(out, "**");
      i += 2;
      continue;
    }
    if (c == '*') {
      size_t j = find_single_star(s, i + 1);
      if (j != MD_NONE && j > i + 1) {
        sb_puts(out, "<em>");
        md_inline(str_new(s.p + i + 1, j - (i + 1)), link, lctx, out);
        sb_puts(out, "</em>");
        i = j + 1;
        continue;
      }
      esc_char(out, c);
      i++;
      continue;
    }
    if (c == '[') {
      size_t j = find_unescaped(s, i + 1, "]", 1);
      if (j != MD_NONE && j + 1 < s.n && s.p[j + 1] == '(') {
        size_t k = j + 2;
        while (k < s.n && s.p[k] != ')') k++;
        if (k < s.n) {
          Str target = str_new(s.p + j + 2, k - (j + 2));
          int ok = target.n > 0;
          for (size_t q = 0; q < target.n && ok; ) {
            uint32_t cp;
            size_t adv;
            cp = u8dec((const unsigned char *)target.p + q, target.n - q, &adv);
            if (js_space(cp)) ok = 0;
            q += adv;
          }
          if (ok) {
            Str href = link(lctx, target);
            sb_puts(out, "<a href=\"");
            escape_html(out, href);
            sb_puts(out, "\">");
            md_inline(str_new(s.p + i + 1, j - (i + 1)), link, lctx, out);
            sb_puts(out, "</a>");
            i = k + 1;
            continue;
          }
        }
      }
      esc_char(out, c);
      i++;
      continue;
    }
    esc_char(out, c);
    i++;
  }
}

static int is_lang_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
}
static int fence_open(Str line, Str *lang) {
  if (line.n < 3 || memcmp(line.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < line.n && is_lang_char(line.p[i])) i++;
  Str l = str_new(line.p + 3, i - 3);
  while (i < line.n && (line.p[i] == ' ' || line.p[i] == '\t')) i++;
  if (i != line.n) return 0;
  if (lang) *lang = l;
  return 1;
}
static int fence_close(Str line) {
  if (line.n < 3 || memcmp(line.p, "```", 3) != 0) return 0;
  size_t i = 3;
  while (i < line.n && (line.p[i] == ' ' || line.p[i] == '\t')) i++;
  return i == line.n;
}
static int heading_of(Str line, int *level, Str *text) {
  size_t i = 0;
  while (i < line.n && line.p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i >= line.n || (line.p[i] != ' ' && line.p[i] != '\t')) return 0;
  size_t j = i;
  while (j < line.n && (line.p[j] == ' ' || line.p[j] == '\t')) j++;
  Str t = str_new(line.p + j, line.n - j);
  while (t.n > 0 && (t.p[t.n - 1] == ' ' || t.p[t.n - 1] == '\t')) t.n--;
  if (t.n == 0) return 0;
  if (level) *level = (int)i;
  if (text) *text = t;
  return 1;
}
static int is_hr(Str line) {
  if (line.n < 3) return 0;
  if (memcmp(line.p, "---", 3) != 0 && memcmp(line.p, "***", 3) != 0) return 0;
  size_t i = 3;
  while (i < line.n && (line.p[i] == ' ' || line.p[i] == '\t')) i++;
  return i == line.n;
}
typedef enum { L_NONE, L_UL, L_OL } ListKind;
static int list_open(Str line, ListKind *kind, long *start, Str *item) {
  if (line.n >= 2 && (line.p[0] == '-' || line.p[0] == '*') && line.p[1] == ' ') {
    *kind = L_UL;
    *item = st_trim(str_new(line.p + 2, line.n - 2));
    return 1;
  }
  if (line.n >= 2 && isdigit((unsigned char)line.p[0])) {
    size_t d = 0;
    while (d < line.n && isdigit((unsigned char)line.p[d])) d++;
    if (d >= 1 && d <= 9 && d + 1 < line.n && line.p[d] == '.' && line.p[d + 1] == ' ') {
      char tmp[16];
      memcpy(tmp, line.p, d);
      tmp[d] = 0;
      *kind = L_OL;
      *start = strtol(tmp, NULL, 10);
      *item = st_trim(str_new(line.p + d + 2, line.n - (d + 2)));
      return 1;
    }
  }
  return 0;
}
static int list_continued(Str line, Str *item) {
  size_t i = 0;
  while (i < line.n && line.p[i] == ' ') i++;
  if (i < 2 || i >= line.n) return 0;
  uint32_t cp;
  size_t adv;
  cp = u8dec((const unsigned char *)line.p + i, line.n - i, &adv);
  if (js_space(cp)) return 0;
  *item = st_trim(str_new(line.p + i, line.n - i));
  return 1;
}
static Str unique_heading_id(MdResult *r, Str text) {
  Str base = slugify(text);
  for (long n = 1;; n++) {
    Str cand;
    if (n == 1) cand = base;
    else {
      StrBuf b = {0};
      sb_put(&b, base);
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "-%ld", n);
      sb_puts(&b, tmp);
      cand = sb_str(&b);
    }
    int taken = 0;
    for (size_t k = 0; k < r->nused; k++) if (str_eq(r->used_ids[k], cand)) { taken = 1; break; }
    if (!taken) {
      if (r->nused == r->capused) {
        size_t nc = r->capused ? r->capused * 2 : 16;
        Str *ni = (Str *)arena_alloc(nc * sizeof(Str));
        if (r->used_ids) memcpy(ni, r->used_ids, r->nused * sizeof(Str));
        r->used_ids = ni; r->capused = nc;
      }
      r->used_ids[r->nused++] = cand;
      return cand;
    }
  }
}
static void toc_add(MdResult *r, int level, Str id, Str html) {
  if (r->ntoc == r->captoc) {
    size_t nc = r->captoc ? r->captoc * 2 : 8;
    TocEnt *ni = (TocEnt *)arena_alloc(nc * sizeof(TocEnt));
    if (r->toc) memcpy(ni, r->toc, r->ntoc * sizeof(TocEnt));
    r->toc = ni; r->captoc = nc;
  }
  r->toc[r->ntoc].level = level;
  r->toc[r->ntoc].id = id;
  r->toc[r->ntoc].html = html;
  r->ntoc++;
}

static MdResult md_render(Str body, LinkFn link, void *lctx) {
  MdResult r;
  memset(&r, 0, sizeof(r));
  Lines ls = split_lines(body);
  StrBuf out = {0};
  size_t i = 0;
  while (i < ls.n) {
    Str line = ls.lines[i];
    if (st_blank(line)) { i++; continue; }
    // fence
    Str flang;
    if (fence_open(line, &flang)) {
      StrBuf code = {0};
      i++;
      while (i < ls.n && !fence_close(ls.lines[i])) {
        escape_html(&code, ls.lines[i]);
        sb_putc(&code, '\n');
        i++;
      }
      if (i < ls.n) i++; // closing fence
      sb_puts(&out, "<pre><code");
      if (flang.n > 0) {
        sb_puts(&out, " class=\"language-");
        escape_html(&out, flang);
        sb_puts(&out, "\"");
      }
      sb_puts(&out, ">");
      sb_put(&out, sb_str(&code));
      sb_puts(&out, "</code></pre>\n");
      sb_free(&code);
      continue;
    }
    // heading
    int hlevel = 0;
    Str htext;
    if (heading_of(line, &hlevel, &htext)) {
      Str id = unique_heading_id(&r, htext);
      StrBuf hb = {0};
      md_inline(htext, link, lctx, &hb);
      Str hhtml = sb_str(&hb);
      if (hlevel == 2 || hlevel == 3) toc_add(&r, hlevel, id, hhtml);
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "<h%d id=\"", hlevel);
      sb_puts(&out, tmp);
      escape_html(&out, id);
      sb_puts(&out, "\">");
      sb_put(&out, hhtml);
      snprintf(tmp, sizeof(tmp), "</h%d>\n", hlevel);
      sb_puts(&out, tmp);
      i++;
      continue;
    }
    // thematic break
    if (is_hr(line)) { sb_puts(&out, "<hr>\n"); i++; continue; }
    // blockquote
    if (line.n > 0 && line.p[0] == '>') {
      StrBuf inner = {0};
      int first = 1;
      while (i < ls.n && ls.lines[i].n > 0 && ls.lines[i].p[0] == '>') {
        Str l = ls.lines[i];
        size_t st = 1;
        if (st < l.n && l.p[st] == ' ') st++;
        Str t = st_trim(str_new(l.p + st, l.n - st));
        if (t.n > 0) {
          if (!first) sb_putc(&inner, '\n');
          sb_put(&inner, t);
          first = 0;
        }
        i++;
      }
      sb_puts(&out, "<blockquote>\n");
      if (inner.len > 0) {
        sb_puts(&out, "<p>");
        md_inline(sb_str(&inner), link, lctx, &out);
        sb_puts(&out, "</p>\n");
      }
      sb_puts(&out, "</blockquote>\n");
      sb_free(&inner);
      continue;
    }
    // list
    ListKind kind = L_NONE;
    long startnum = 0;
    Str first_item;
    if (list_open(line, &kind, &startnum, &first_item)) {
      sb_puts(&out, kind == L_OL ? "<ol" : "<ul");
      if (kind == L_OL && startnum != 1) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), " start=\"%ld\"", startnum);
        sb_puts(&out, tmp);
      }
      sb_puts(&out, ">\n");
      StrBuf item = {0};
      sb_put(&item, first_item);
      i++;
      while (i < ls.n) {
        Str l2 = ls.lines[i];
        ListKind k2 = L_NONE;
        long s2 = 0;
        Str it2;
        if (list_open(l2, &k2, &s2, &it2) && k2 == kind) {
          sb_puts(&out, "<li>");
          md_inline(sb_str(&item), link, lctx, &out);
          sb_puts(&out, "</li>\n");
          sb_free(&item);
          item = (StrBuf){0};
          sb_put(&item, it2);
          i++;
          continue;
        }
        Str cont;
        if (list_continued(l2, &cont)) {
          sb_putc(&item, '\n');
          sb_put(&item, cont);
          i++;
          continue;
        }
        break;
      }
      sb_puts(&out, "<li>");
      md_inline(sb_str(&item), link, lctx, &out);
      sb_puts(&out, "</li>\n");
      sb_free(&item);
      sb_puts(&out, kind == L_OL ? "</ol>\n" : "</ul>\n");
      continue;
    }
    // paragraph
    StrBuf para = {0};
    int first = 1;
    while (i < ls.n) {
      Str l2 = ls.lines[i];
      if (st_blank(l2)) break;
      if (fence_open(l2, NULL) || heading_of(l2, NULL, NULL) || is_hr(l2)) break;
      Str t = st_trim(l2);
      if (!first) sb_putc(&para, '\n');
      sb_put(&para, t);
      first = 0;
      i++;
    }
    Str paraStr = sb_str(&para);
    sb_puts(&out, "<p>");
    md_inline(paraStr, link, lctx, &out);
    sb_puts(&out, "</p>\n");
    if (!r.have_excerpt) {
      StrBuf ex = {0};
      md_inline(paraStr, link, lctx, &ex);
      r.excerpt = sb_str(&ex);
      r.have_excerpt = 1;
    }
    sb_free(&para);
  }
  r.html = sb_str(&out);
  return r;
}

// ---------------------------------------------------------------- Templates (lib/template.js)
#define INCLUDE_DEPTH 8
typedef enum { TN_TEXT, TN_VAR, TN_RAW, TN_IF, TN_FOR, TN_INCLUDE } TKind;
typedef struct TNode TNode;
struct TNode {
  TKind kind;
  Str text;                    // TN_TEXT
  Str *path; int npath;        // TN_VAR / TN_RAW / TN_IF / TN_FOR
  int isNot;                   // TN_IF
  Str eltName;                 // TN_FOR
  TNode **kids; size_t nkids, capkids;       // then / body
  TNode **elseKids; size_t nelse, capelse;   // TN_IF else (NULL when no else)
  Str incName;                 // TN_INCLUDE
};
typedef struct { Str name; TNode **nodes; size_t nnodes; } Template;

static size_t find_substr(Str s, size_t from, const char *pat, size_t plen) {
  if (from > s.n) return MD_NONE;
  for (size_t i = from; i + plen <= s.n; i++) {
    if (memcmp(s.p + i, pat, plen) == 0) return i;
  }
  return MD_NONE;
}
static int is_name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

// pathOf: validate PATH ^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$
// and split at dots. On failure err = `template "<name>": <json> is not a path`.
static int tpl_parse_path(Str name, Str text, Str **out_path, int *out_n, char *err, size_t errn) {
  if (text.n == 0) goto bad;
  {
    size_t i = 0;
    int nparts = 1;
    for (size_t q = 0; q < text.n; q++) if (text.p[q] == '.') nparts++;
    Str *parts = (Str *)arena_alloc((size_t)nparts * sizeof(Str));
    int pi = 0;
    size_t start = 0;
    for (size_t q = 0; q <= text.n; q++) {
      if (q == text.n || text.p[q] == '.') {
        Str part = str_new(text.p + start, q - start);
        if (part.n == 0 || !is_name_start(part.p[0])) goto bad;
        for (size_t r = 1; r < part.n; r++) if (!is_name_char(part.p[r])) goto bad;
        parts[pi++] = part;
        start = q + 1;
      }
    }
    (void)i;
    *out_path = parts;
    *out_n = nparts;
    return 1;
  }
bad: {
    StrBuf b = {0};
    char nb[64];
    snprintf(nb, sizeof(nb), "template \"%.*s\": ", (int)name.n, name.p);
    sb_puts(&b, nb);
    sb_putc(&b, '"');
    json_escape(&b, text);
    sb_putc(&b, '"');
    sb_puts(&b, " is not a path");
    snprintf(err, errn, "%.*s", (int)b.len, b.buf ? b.buf : "");
    sb_free(&b);
    return 0;
  }
}

typedef enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG } TKKind;
typedef struct { TKKind kind; Str text; Str *path; int npath; Str tag; } TToken;
typedef struct { TToken *toks; size_t n, cap; } TokList;
static void tok_push(TokList *l, TKKind kind, Str text, Str *path, int npath, Str tag) {
  if (l->n == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 16;
    TToken *ni = (TToken *)arena_alloc(nc * sizeof(TToken));
    if (l->toks) memcpy(ni, l->toks, l->n * sizeof(TToken));
    l->toks = ni; l->cap = nc;
  }
  l->toks[l->n].kind = kind;
  l->toks[l->n].text = text;
  l->toks[l->n].path = path;
  l->toks[l->n].npath = npath;
  l->toks[l->n].tag = tag;
  l->n++;
}
static int tokenize(Str name, Str src, TokList *out, char *err, size_t errn) {
  StrBuf text = {0};
  size_t i = 0;
  while (i < src.n) {
    size_t open = i;
    while (open < src.n && src.p[open] != '{') open++;
    if (open >= src.n || open == src.n - 1) {
      sb_putn(&text, src.p + i, src.n - i);
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      sb_putn(&text, src.p + i, open + 1 - i);
      i = open + 1;
      continue;
    }
    sb_putn(&text, src.p + i, open - i);
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      size_t clen = raw ? 3 : 2;
      size_t close = find_substr(src, open + clen, raw ? "}}}" : "}}", clen);
      if (close == MD_NONE) {
        snprintf(err, errn, "template \"%.*s\": a %s is not closed", (int)name.n, name.p, raw ? "{{{" : "{{");
        return 0;
      }
      if (text.len > 0) { tok_push(out, TK_TEXT, sb_str(&text), NULL, 0, str_new("", 0)); text = (StrBuf){0}; }
      Str inner = str_new(src.p + open + clen, close - (open + clen));
      Str *path = NULL;
      int npath = 0;
      if (!tpl_parse_path(name, js_trim(inner), &path, &npath, err, errn)) return 0;
      tok_push(out, raw ? TK_RAW : TK_VAR, str_new("", 0), path, npath, str_new("", 0));
      i = close + clen;
      continue;
    }
    size_t close = find_substr(src, open + 2, "%}", 2);
    if (close == MD_NONE) {
      snprintf(err, errn, "template \"%.*s\": a {%% is not closed", (int)name.n, name.p);
      return 0;
    }
    size_t end = close + 2;
    size_t lineStart = 0;
    for (size_t q = open; q > 0; q--) if (src.p[q - 1] == '\n') { lineStart = q; break; }
    size_t nl = end;
    while (nl < src.n && src.p[nl] != '\n') nl++;
    size_t lineEnd = nl;
    int standalone = 1;
    for (size_t q = lineStart; q < open; q++) if (src.p[q] != ' ' && src.p[q] != '\t') { standalone = 0; break; }
    if (standalone) for (size_t q = end; q < lineEnd; q++) if (src.p[q] != ' ' && src.p[q] != '\t') { standalone = 0; break; }
    if (standalone) {
      size_t drop = open - lineStart;
      if (drop > text.len) text.len = 0; else text.len -= drop;
    }
    if (text.len > 0) { tok_push(out, TK_TEXT, sb_str(&text), NULL, 0, str_new("", 0)); text = (StrBuf){0}; }
    Str tag = js_trim(str_new(src.p + open + 2, close - (open + 2)));
    tok_push(out, TK_TAG, str_new("", 0), NULL, 0, tag);
    i = standalone ? (nl >= src.n ? src.n : nl + 1) : end;
  }
  if (text.len > 0) tok_push(out, TK_TEXT, sb_str(&text), NULL, 0, str_new("", 0));
  return 1;
}

static int tpl_match_for(Str tag, Str *elt, Str *pathText) {
  if (tag.n < 4 || memcmp(tag.p, "for ", 4) != 0) return 0;
  size_t i = 4;
  if (i >= tag.n || !is_name_start(tag.p[i])) return 0;
  size_t ns = i;
  while (i < tag.n && is_name_char(tag.p[i])) i++;
  Str nm = str_new(tag.p + ns, i - ns);
  if (i + 4 > tag.n || memcmp(tag.p + i, " in ", 4) != 0) return 0;
  Str rest = str_new(tag.p + i + 4, tag.n - (i + 4));
  if (rest.n == 0) return 0;
  for (size_t q = 0; q < rest.n;) {
    uint32_t cp;
    size_t adv;
    cp = u8dec((const unsigned char *)rest.p + q, rest.n - q, &adv);
    if (js_space(cp)) return 0;
    q += adv;
  }
  *elt = nm;
  *pathText = rest;
  return 1;
}
static int tpl_match_if(Str tag, int *isNot, Str *pathText) {
  if (tag.n < 3 || memcmp(tag.p, "if ", 3) != 0) return 0;
  size_t i = 3;
  int not = 0;
  if (i + 4 <= tag.n && memcmp(tag.p + i, "not ", 4) == 0) { not = 1; i += 4; }
  Str rest = str_new(tag.p + i, tag.n - i);
  if (rest.n == 0) return 0;
  for (size_t q = 0; q < rest.n;) {
    uint32_t cp;
    size_t adv;
    cp = u8dec((const unsigned char *)rest.p + q, rest.n - q, &adv);
    if (js_space(cp)) return 0;
    q += adv;
  }
  *isNot = not;
  *pathText = rest;
  return 1;
}
static int tpl_match_include(Str tag, Str *incName) {
  if (tag.n < 11) return 0;
  if (memcmp(tag.p, "include \"", 9) != 0) return 0;
  if (tag.p[tag.n - 1] != '"') return 0;
  Str mid = str_new(tag.p + 9, tag.n - 10);
  if (mid.n == 0) return 0;
  for (size_t q = 0; q < mid.n; q++) {
    char c = mid.p[q];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
  }
  *incName = mid;
  return 1;
}

static TNode *tnode_new(TKind kind) {
  TNode *n = (TNode *)arena_alloc(sizeof(TNode));
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  return n;
}
typedef struct { TNode **items; size_t n, cap; } NodeVec;
static void nvec_push(NodeVec *v, TNode *nd) {
  if (v->n == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    TNode **ni = (TNode **)arena_alloc(nc * sizeof(TNode *));
    if (v->items) memcpy(ni, v->items, v->n * sizeof(TNode *));
    v->items = ni; v->cap = nc;
  }
  v->items[v->n++] = nd;
}
// Parse a template. Returns 1 on success (nodes in *out_nodes), 0 on failure
// (err = `template "<name>": <reason>`).
static int parse_template(Str name, Str src, TNode ***out_nodes, size_t *out_n, char *err, size_t errn) {
  TokList tl = {0};
  if (!tokenize(name, src, &tl, err, errn)) return 0;
  NodeVec root = {0};
  typedef struct { TNode *node; int inElse; } Frame;
  Frame stack[64];
  int sp = 1;
  stack[0].node = NULL;
  stack[0].inElse = 0;
  NodeVec *body = &root;
  for (size_t ti = 0; ti < tl.n; ti++) {
    TToken *tok = &tl.toks[ti];
    if (tok->kind == TK_TEXT) {
      TNode *nd = tnode_new(TN_TEXT);
      nd->text = tok->text;
      nvec_push(body, nd);
    } else if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      TNode *nd = tnode_new(tok->kind == TK_VAR ? TN_VAR : TN_RAW);
      nd->path = tok->path;
      nd->npath = tok->npath;
      nvec_push(body, nd);
    } else {
      Str tag = tok->tag;
      Frame *top = &stack[sp - 1];
      Str elt, ptext;
      int isNot = 0;
      Str inc;
      if (tpl_match_for(tag, &elt, &ptext)) {
        Str *path = NULL;
        int npath = 0;
        if (!tpl_parse_path(name, ptext, &path, &npath, err, errn)) return 0;
        TNode *nd = tnode_new(TN_FOR);
        nd->eltName = elt;
        nd->path = path;
        nd->npath = npath;
        nvec_push(body, nd);
        if (sp >= 64) { snprintf(err, errn, "template \"%.*s\": a for is not closed", (int)name.n, name.p); return 0; }
        stack[sp].node = nd;
        stack[sp].inElse = 0;
        sp++;
        body = (NodeVec *)&nd->kids;
      } else if (tpl_match_if(tag, &isNot, &ptext)) {
        Str *path = NULL;
        int npath = 0;
        if (!tpl_parse_path(name, ptext, &path, &npath, err, errn)) return 0;
        TNode *nd = tnode_new(TN_IF);
        nd->isNot = isNot;
        nd->path = path;
        nd->npath = npath;
        nvec_push(body, nd);
        if (sp >= 64) { snprintf(err, errn, "template \"%.*s\": a if is not closed", (int)name.n, name.p); return 0; }
        stack[sp].node = nd;
        stack[sp].inElse = 0;
        sp++;
        body = (NodeVec *)&nd->kids;
      } else if (str_eq_lit(tag, "else")) {
        if (top->node == NULL || top->node->kind != TN_IF || top->inElse) {
          snprintf(err, errn, "template \"%.*s\": an else outside an if", (int)name.n, name.p);
          return 0;
        }
        top->inElse = 1;
        body = (NodeVec *)&top->node->elseKids;
      } else if (str_eq_lit(tag, "endfor") || str_eq_lit(tag, "endif")) {
        int wantFor = str_eq_lit(tag, "endfor");
        if (top->node == NULL || top->node->kind != (wantFor ? TN_FOR : TN_IF)) {
          snprintf(err, errn, "template \"%.*s\": an %.*s with no open %s", (int)name.n, name.p, (int)tag.n, tag.p, wantFor ? "for" : "if");
          return 0;
        }
        sp--;
        Frame *parent = &stack[sp - 1];
        if (parent->node == NULL) body = &root;
        else if (parent->node->kind == TN_FOR) body = (NodeVec *)&parent->node->kids;
        else body = parent->inElse ? (NodeVec *)&parent->node->elseKids : (NodeVec *)&parent->node->kids;
      } else if (tpl_match_include(tag, &inc)) {
        TNode *nd = tnode_new(TN_INCLUDE);
        nd->incName = inc;
        nvec_push(body, nd);
      } else {
        snprintf(err, errn, "template \"%.*s\": {%% %.*s %%} is not a tag", (int)name.n, name.p, (int)tag.n, tag.p);
        return 0;
      }
    }
  }
  if (sp > 1) {
    TNode *top = stack[sp - 1].node;
    snprintf(err, errn, "template \"%.*s\": a %s is not closed", (int)name.n, name.p, top->kind == TN_FOR ? "for" : "if");
    return 0;
  }
  *out_nodes = root.items;
  *out_n = root.n;
  return 1;
}

// ---------------------------------------------------------------- template rendering
typedef struct Scope Scope;
struct Scope { Val *vars; Scope *parent; };
static Val *tpl_lookup(Scope *sc, Str *path, int npath) {
  Val *v = NULL;
  for (Scope *s = sc; s; s = s->parent) {
    v = val_obj_get(s->vars, path[0]);
    if (v) break;
  }
  if (!v) return NULL;
  for (int k = 1; k < npath; k++) {
    if (!v) return NULL;
    if (v->t == V_OBJ) {
      v = val_obj_get(v, path[k]);
    } else if (v->t == V_ARR) {
      if (str_eq_lit(path[k], "length")) {
        v = val_int((long long)v->u.arr.len);
      } else {
        // canonical array index: digits, no leading zero (unless "0"), < 2^32-1
        Str seg = path[k];
        int ok = seg.n > 0;
        for (size_t q = 0; q < seg.n; q++) if (!isdigit((unsigned char)seg.p[q])) { ok = 0; break; }
        if (ok && seg.n > 1 && seg.p[0] == '0') ok = 0;
        unsigned long long idx = 0;
        if (ok) {
          for (size_t q = 0; q < seg.n; q++) {
            idx = idx * 10 + (unsigned long long)(seg.p[q] - '0');
            if (idx > 0xFFFFFFFFull) { ok = 0; break; }
          }
        }
        if (!ok || idx >= 4294967295ull || idx >= v->u.arr.len) return NULL;
        v = v->u.arr.items[idx];
      }
    } else return NULL;
    if (!v) return NULL;
  }
  return v;
}
typedef struct { Template *templates; size_t ntemplates; } TplCtx;
static Template *tpl_find(TplCtx *ctx, Str name) {
  for (size_t i = 0; i < ctx->ntemplates; i++) {
    if (str_eq(ctx->templates[i].name, name)) return &ctx->templates[i];
  }
  return NULL;
}
static void render_nodes(TNode **nodes, size_t n, Scope *sc, TplCtx *ctx, int depth, StrBuf *out) {
  for (size_t ni = 0; ni < n; ni++) {
    TNode *nd = nodes[ni];
    switch (nd->kind) {
      case TN_TEXT:
        sb_put(out, nd->text);
        break;
      case TN_VAR: {
        Val *v = tpl_lookup(sc, nd->path, nd->npath);
        StrBuf tmp = {0};
        val_text(&tmp, v);
        escape_html(out, sb_str(&tmp));
        sb_free(&tmp);
        break;
      }
      case TN_RAW: {
        Val *v = tpl_lookup(sc, nd->path, nd->npath);
        val_text(out, v);
        break;
      }
      case TN_IF: {
        Val *v = tpl_lookup(sc, nd->path, nd->npath);
        int truth = val_truthy(v);
        if (truth != nd->isNot) render_nodes(nd->kids, nd->nkids, sc, ctx, depth, out);
        else if (nd->elseKids) render_nodes(nd->elseKids, nd->nelse, sc, ctx, depth, out);
        break;
      }
      case TN_FOR: {
        Val *xs = tpl_lookup(sc, nd->path, nd->npath);
        if (!xs || xs->t != V_ARR) break;
        for (size_t k = 0; k < xs->u.arr.len; k++) {
          Val *vars = val_obj();
          val_obj_set(vars, nd->eltName, xs->u.arr.items[k]);
          Val *loop = val_obj();
          val_obj_set(loop, str_lit("index"), val_int((long long)k + 1));
          val_obj_set(loop, str_lit("first"), val_bool(k == 0));
          val_obj_set(loop, str_lit("last"), val_bool(k == xs->u.arr.len - 1));
          val_obj_set(loop, str_lit("length"), val_int((long long)xs->u.arr.len));
          val_obj_set(vars, str_lit("loop"), loop);
          Scope sub;
          sub.vars = vars;
          sub.parent = sc;
          render_nodes(nd->kids, nd->nkids, &sub, ctx, depth, out);
        }
        break;
      }
      case TN_INCLUDE: {
        Template *t = tpl_find(ctx, nd->incName);
        if (t && depth < INCLUDE_DEPTH) render_nodes(t->nodes, t->nnodes, sc, ctx, depth + 1, out);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- build pipeline (index.js)
typedef struct Tag Tag;
typedef struct Source Source;
struct Source {
  Str path, text, body;
  Val *data;
  int isPost;
  Str layout, slug, outDir, url;
  Str html, excerpt;
  TocEnt *toc;
  size_t ntoc;
  Tag **tagRefs;
  size_t ntagRefs, captagRefs;
  Source *newer, *older;
  Val *card;
};
struct Tag {
  Str name, slug, url;
  Source **posts;
  size_t nposts, capposts;
};
typedef struct { Str *items; size_t n, cap; } StrVec;
static void strvec_push(StrVec *v, Str s) {
  if (v->n == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 16;
    Str *ni = (Str *)arena_alloc(nc * sizeof(Str));
    if (v->items) memcpy(ni, v->items, v->n * sizeof(Str));
    v->items = ni; v->cap = nc;
  }
  v->items[v->n++] = s;
}
typedef struct { Str key; int val; int used; } MapEnt;
typedef struct { MapEnt *ents; size_t cap; } StrMap;
static uint64_t hash_str(Str s) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < s.n; i++) { h ^= (unsigned char)s.p[i]; h *= 1099511628211ull; }
  return h;
}
static void map_init(StrMap *m, size_t cap) {
  m->cap = cap;
  m->ents = (MapEnt *)calloc(cap, sizeof(MapEnt));
}
static MapEnt *map_slot(StrMap *m, Str key) {
  size_t i = (size_t)hash_str(key) & (m->cap - 1);
  while (m->ents[i].used) {
    if (str_eq(m->ents[i].key, key)) return &m->ents[i];
    i = (i + 1) & (m->cap - 1);
  }
  return &m->ents[i];
}
static int map_get(StrMap *m, Str key, int *out) {
  MapEnt *e = map_slot(m, key);
  if (!e->used) return 0;
  *out = e->val;
  return 1;
}
static void map_put(StrMap *m, Str key, int val) {
  MapEnt *e = map_slot(m, key);
  e->key = key; e->val = val; e->used = 1;
}
typedef struct { Str key, val; } OutEnt;
typedef struct { OutEnt *ents; size_t n, cap; } OutMap;
static void out_set(OutMap *o, Str key, Str val) {
  for (size_t i = 0; i < o->n; i++) {
    if (str_eq(o->ents[i].key, key)) { o->ents[i].val = val; return; }
  }
  if (o->n == o->cap) {
    size_t nc = o->cap ? o->cap * 2 : 32;
    OutEnt *ni = (OutEnt *)arena_alloc(nc * sizeof(OutEnt));
    if (o->ents) memcpy(ni, o->ents, o->n * sizeof(OutEnt));
    o->ents = ni; o->cap = nc;
  }
  o->ents[o->n].key = key;
  o->ents[o->n].val = val;
  o->n++;
}
typedef struct {
  Val *input;
  Str base;
  TplCtx tpl;
  StrVec report;
  Source *sources;
  size_t nsources, capsources;
  struct { Str path, content; } *statics;
  size_t nstatics, capstatics;
  StrMap bySource;
  StrMap staticPaths;
  StrMap broken;
  Source **posts;
  size_t nposts;
  Tag **tags;
  size_t ntags, captags;
  OutMap output;
  Val *siteVal;
  Val *allTagsVal;
  Val *recentVal;
} Build;

static Str dir_of(Str path) {
  for (size_t i = path.n; i > 0; i--) {
    if (path.p[i - 1] == '/') return str_new(path.p, i - 1);
  }
  return str_new("", 0);
}
static int is_scheme(Str t) {
  if (t.n == 0) return 0;
  char c0 = t.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
  for (size_t i = 1; i < t.n; i++) {
    char c = t.p[i];
    if (c == ':') return 1;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
  }
  return 0;
}
// resolvePath: `rel` from `dir`, or 0 when it climbs out of the root.
static int resolve_path(Str dir, Str rel, Str *out) {
  Str parts[128];
  int np = 0;
  if (dir.n > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (np < 128) parts[np++] = str_new(dir.p + start, i - start);
        start = i + 1;
      }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Str seg = str_new(rel.p + start, i - start);
      start = i + 1;
      if (seg.n == 0) continue;
      if (str_eq_lit(seg, ".")) continue;
      if (str_eq_lit(seg, "..")) {
        if (np == 0) return 0;
        np--;
      } else if (np < 128) parts[np++] = seg;
    }
  }
  StrBuf b = {0};
  for (int i = 0; i < np; i++) {
    if (i) sb_putc(&b, '/');
    sb_put(&b, parts[i]);
  }
  *out = sb_str(&b);
  return 1;
}
typedef struct { Build *b; Source *s; } LinkCtx;
static Str link_resolve(void *ctxp, Str target) {
  LinkCtx *ctx = (LinkCtx *)ctxp;
  Build *b = ctx->b;
  Source *s = ctx->s;
  if (is_scheme(target) || (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#'))) return target;
  size_t hash = MD_NONE;
  for (size_t i = 0; i < target.n; i++) if (target.p[i] == '#') { hash = i; break; }
  Str rel = hash == MD_NONE ? target : str_new(target.p, hash);
  Str frag = hash == MD_NONE ? str_new("", 0) : str_new(target.p + hash, target.n - hash);
  Str path;
  int havePath = rel.n > 0 && resolve_path(dir_of(s->path), rel, &path);
  if (havePath) {
    int idx;
    if (map_get(&b->bySource, path, &idx)) {
      StrBuf u = {0};
      sb_put(&u, b->sources[idx].url);
      sb_put(&u, frag);
      return sb_str(&u);
    }
    int one;
    if (map_get(&b->staticPaths, path, &one)) {
      StrBuf u = {0};
      sb_put(&u, b->base);
      sb_putc(&u, '/');
      sb_put(&u, path);
      sb_put(&u, frag);
      return sb_str(&u);
    }
  }
  StrBuf line = {0};
  sb_puts(&line, "broken-link ");
  sb_put(&line, s->path);
  sb_puts(&line, " -> ");
  sb_put(&line, target);
  map_put(&b->broken, sb_str(&line), 1);
  return target;
}
static long words_of(Str text) {
  long n = 0;
  int inWord = 0;
  size_t i = 0;
  while (i < text.n) {
    uint32_t cp;
    size_t adv;
    cp = u8dec((const unsigned char *)text.p + i, text.n - i, &adv);
    if (js_space(cp)) inWord = 0;
    else if (!inWord) { n++; inWord = 1; }
    i += adv;
  }
  return n;
}
static Val *link_of_val(Source *s) {
  if (!s) return val_null();
  Val *o = val_obj();
  Val *title = val_obj_get_lit(s->data, "title");
  val_obj_set(o, str_lit("title"), title ? title : val_str(str_lit("")));
  val_obj_set(o, str_lit("url"), val_str(s->url));
  Val *date = val_obj_get_lit(s->data, "date");
  val_obj_set(o, str_lit("date"), date ? date : val_str(str_lit("")));
  val_obj_set(o, str_lit("date_long"), date ? val_str(date_long(date->u.s)) : val_str(str_lit("")));
  return o;
}
static Val *card_of(Source *s) {
  Val *o = val_obj();
  Val *title = val_obj_get_lit(s->data, "title");
  val_obj_set(o, str_lit("title"), title ? title : val_str(str_lit("")));
  val_obj_set(o, str_lit("url"), val_str(s->url));
  Val *date = val_obj_get_lit(s->data, "date");
  val_obj_set(o, str_lit("date"), date ? date : val_str(str_lit("")));
  val_obj_set(o, str_lit("date_long"), date ? val_str(date_long(date->u.s)) : val_str(str_lit("")));
  Val *summary = val_obj_get_lit(s->data, "summary");
  val_obj_set(o, str_lit("summary"), summary ? summary : val_str(str_lit("")));
  val_obj_set(o, str_lit("excerpt"), val_str(s->excerpt));
  long w = words_of(s->body);
  long rm = (w + 199) / 200;
  if (rm < 1) rm = 1;
  val_obj_set(o, str_lit("reading_minutes"), val_int(rm));
  Val *tags = val_arr();
  for (size_t i = 0; i < s->ntagRefs; i++) {
    Val *t = val_obj();
    val_obj_set(t, str_lit("name"), val_str(s->tagRefs[i]->name));
    val_obj_set(t, str_lit("slug"), val_str(s->tagRefs[i]->slug));
    val_obj_set(t, str_lit("url"), val_str(s->tagRefs[i]->url));
    val_arr_push(tags, t);
  }
  val_obj_set(o, str_lit("tags"), tags);
  return o;
}
static Val *page_of(Source *s) {
  Val *o = card_of(s);
  val_obj_set(o, str_lit("slug"), val_str(s->slug));
  val_obj_set(o, str_lit("content"), val_str(s->html));
  Val *toc = val_arr();
  for (size_t i = 0; i < s->ntoc; i++) {
    Val *h = val_obj();
    val_obj_set(h, str_lit("level"), val_int(s->toc[i].level));
    val_obj_set(h, str_lit("id"), val_str(s->toc[i].id));
    val_obj_set(h, str_lit("html"), val_str(s->toc[i].html));
    val_arr_push(toc, h);
  }
  val_obj_set(o, str_lit("toc"), toc);
  val_obj_set(o, str_lit("newer"), link_of_val(s->newer));
  val_obj_set(o, str_lit("older"), link_of_val(s->older));
  val_obj_set(o, str_lit("is_post"), val_bool(s->isPost));
  return o;
}
static Val *tag_of_val(Tag *t) {
  Val *o = val_obj();
  val_obj_set(o, str_lit("name"), val_str(t->name));
  val_obj_set(o, str_lit("slug"), val_str(t->slug));
  val_obj_set(o, str_lit("url"), val_str(t->url));
  val_obj_set(o, str_lit("count"), val_int((long long)t->nposts));
  Val *posts = val_arr();
  for (size_t i = 0; i < t->nposts; i++) val_arr_push(posts, t->posts[i]->card);
  val_obj_set(o, str_lit("posts"), posts);
  return o;
}
static Val *base_vars(Build *b) {
  Val *v = val_obj();
  val_obj_set(v, str_lit("site"), b->siteVal);
  val_obj_set(v, str_lit("tags"), b->allTagsVal);
  val_obj_set(v, str_lit("recent"), b->recentVal);
  return v;
}
static Val *render_tpl(Build *b, Str name, Val *vars) {
  Template *t = tpl_find(&b->tpl, name);
  if (!t) return NULL;
  Scope root;
  root.vars = vars;
  root.parent = NULL;
  StrBuf out = {0};
  render_nodes(t->nodes, t->nnodes, &root, &b->tpl, 0, &out);
  return val_str(sb_str(&out));
}
static Str index_url(Build *b, long long k) {
  StrBuf u = {0};
  sb_put(&u, b->base);
  if (k == 1) {
    sb_putc(&u, '/');
  } else {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/page/%lld/", k);
    sb_puts(&u, tmp);
  }
  return sb_str(&u);
}
static int cmp_file_path(const void *a, const void *bb) {
  Val *fa = *(Val *const *)a;
  Val *fb = *(Val *const *)bb;
  Val *pa = val_obj_get_lit(fa, "path");
  Val *pb = val_obj_get_lit(fb, "path");
  Str sa = pa && pa->t == V_STR ? pa->u.s : str_lit("");
  Str sb = pb && pb->t == V_STR ? pb->u.s : str_lit("");
  return cmp_u16(sa, sb);
}
static int cmp_post(const void *a, const void *bb) {
  Source *x = *(Source *const *)a;
  Source *y = *(Source *const *)bb;
  Val *dx = val_obj_get_lit(x->data, "date");
  Val *dy = val_obj_get_lit(y->data, "date");
  Str sx = dx && dx->t == V_STR ? dx->u.s : str_lit("");
  Str sy = dy && dy->t == V_STR ? dy->u.s : str_lit("");
  int c = cmp_u16(sy, sx);
  if (c) return c;
  return cmp_u16(x->slug, y->slug);
}
static int cmp_tag(const void *a, const void *bb) {
  Tag *x = *(Tag *const *)a;
  Tag *y = *(Tag *const *)bb;
  if (x->nposts != y->nposts) return x->nposts < y->nposts ? 1 : -1;
  return cmp_u16(x->slug, y->slug);
}
static int cmp_str_u16(const void *a, const void *bb) {
  return cmp_u16(*(const Str *)a, *(const Str *)bb);
}
static int cmp_str_key(const void *a, const void *bb) {
  return cmp_u16(*(const Str *)a, *(const Str *)bb);
}

// 1. readSources
static void read_sources(Build *b) {
  Val *files = val_obj_get_lit(b->input, "files");
  size_t n = files && files->t == V_ARR ? files->u.arr.len : 0;
  if (n > 1) qsort(files->u.arr.items, n, sizeof(Val *), cmp_file_path);
  for (size_t i = 0; i < n; i++) {
    Val *f = files->u.arr.items[i];
    Val *path = val_obj_get_lit(f, "path");
    Val *content = val_obj_get_lit(f, "content");
    Str p = path && path->t == V_STR ? path->u.s : str_lit("");
    Str c = content && content->t == V_STR ? content->u.s : str_lit("");
    if (str_has_suffix(p, ".md")) {
      StrBuf nb = {0};
      for (size_t k = 0; k < c.n; k++) {
        if (c.p[k] == '\r') {
          sb_putc(&nb, '\n');
          if (k + 1 < c.n && c.p[k + 1] == '\n') k++;
        } else sb_putc(&nb, c.p[k]);
      }
      if (b->nsources == b->capsources) {
        size_t nc = b->capsources ? b->capsources * 2 : 64;
        Source *ni = (Source *)arena_alloc(nc * sizeof(Source));
        if (b->sources) memcpy(ni, b->sources, b->nsources * sizeof(Source));
        b->sources = ni; b->capsources = nc;
      }
      Source *s = &b->sources[b->nsources++];
      memset(s, 0, sizeof(*s));
      s->path = p;
      s->text = sb_str(&nb);
    } else {
      if (b->nstatics == b->capstatics) {
        size_t nc = b->capstatics ? b->capstatics * 2 : 32;
        void *ni = arena_alloc(nc * sizeof(*b->statics));
        if (b->statics) memcpy(ni, b->statics, b->nstatics * sizeof(*b->statics));
        b->statics = ni; b->capstatics = nc;
      }
      b->statics[b->nstatics].path = p;
      b->statics[b->nstatics].content = c;
      b->nstatics++;
    }
  }
}
// 2. frontMatter
static void front_matter(Build *b) {
  size_t kept = 0;
  for (size_t i = 0; i < b->nsources; i++) {
    Source *s = &b->sources[i];
    FMResult fm = parse_front_matter(s->text);
    if (!fm.ok) {
      StrBuf line = {0};
      sb_puts(&line, "refused ");
      sb_put(&line, s->path);
      sb_puts(&line, ": ");
      sb_puts(&line, fm.err);
      strvec_push(&b->report, sb_str(&line));
      continue;
    }
    s->data = fm.data;
    s->body = fm.body;
    s->isPost = str_has_prefix(s->path, "posts/");
    const char *own = NULL;
    if (str_has_prefix(s->path, "tags/")) own = "tags";
    else if (str_has_prefix(s->path, "page/")) own = "page";
    if (own) {
      StrBuf line = {0};
      sb_puts(&line, "refused ");
      sb_put(&line, s->path);
      sb_puts(&line, ": ");
      sb_puts(&line, own);
      sb_puts(&line, "/ is the generator's own");
      strvec_push(&b->report, sb_str(&line));
      continue;
    }
    if (s->isPost && !val_obj_get_lit(s->data, "date")) {
      StrBuf line = {0};
      sb_puts(&line, "refused ");
      sb_put(&line, s->path);
      sb_puts(&line, ": a post needs a date");
      strvec_push(&b->report, sb_str(&line));
      continue;
    }
    Val *lay = val_obj_get_lit(s->data, "layout");
    Str layout = lay && lay->t == V_STR ? lay->u.s : (s->isPost ? str_lit("post") : str_lit("page"));
    if (!tpl_find(&b->tpl, layout)) {
      StrBuf line = {0};
      sb_puts(&line, "refused ");
      sb_put(&line, s->path);
      sb_puts(&line, ": layout \"");
      sb_put(&line, layout);
      sb_puts(&line, "\" is no template that parses");
      strvec_push(&b->report, sb_str(&line));
      continue;
    }
    s->layout = layout;
    Val *draft = val_obj_get_lit(s->data, "draft");
    if (draft && draft->t == V_BOOL && draft->u.boolean) {
      StrBuf line = {0};
      sb_puts(&line, "draft ");
      sb_put(&line, s->path);
      strvec_push(&b->report, sb_str(&line));
      continue;
    }
    b->sources[kept++] = *s;
  }
  b->nsources = kept;
}
// 3. permalinks
static void permalinks(Build *b) {
  typedef struct { Str dir; Str *taken; size_t n, cap; } DirTaken;
  DirTaken dirs[64];
  int ndirs = 0;
  for (size_t i = 0; i < b->nsources; i++) {
    Source *s = &b->sources[i];
    Str dir = s->isPost ? str_lit("posts") : dir_of(s->path);
    DirTaken *dt = NULL;
    for (int d = 0; d < ndirs; d++) if (str_eq(dirs[d].dir, dir)) { dt = &dirs[d]; break; }
    if (!dt) {
      dt = &dirs[ndirs++];
      dt->dir = dir; dt->taken = NULL; dt->n = 0; dt->cap = 0;
      if (dir.n == 0) {
        dt->taken = (Str *)arena_alloc(3 * sizeof(Str));
        dt->taken[0] = str_lit("posts"); dt->taken[1] = str_lit("tags"); dt->taken[2] = str_lit("page");
        dt->n = 3; dt->cap = 3;
      }
    }
    Val *slugv = val_obj_get_lit(s->data, "slug");
    Val *titlev = val_obj_get_lit(s->data, "title");
    Str base = slugv && slugv->t == V_STR ? slugv->u.s : slugify(titlev ? titlev->u.s : str_lit(""));
    Str slug = base;
    for (long n = 2;; n++) {
      int taken = 0;
      for (size_t k = 0; k < dt->n; k++) if (str_eq(dt->taken[k], slug)) { taken = 1; break; }
      if (!taken) break;
      StrBuf cb = {0};
      sb_put(&cb, base);
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "-%ld", n);
      sb_puts(&cb, tmp);
      slug = sb_str(&cb);
    }
    if (dt->n == dt->cap) {
      size_t nc = dt->cap ? dt->cap * 2 : 8;
      Str *ni = (Str *)arena_alloc(nc * sizeof(Str));
      if (dt->taken) memcpy(ni, dt->taken, dt->n * sizeof(Str));
      dt->taken = ni; dt->cap = nc;
    }
    dt->taken[dt->n++] = slug;
    s->slug = slug;
    StrBuf od = {0};
    if (dir.n > 0) { sb_put(&od, dir); sb_putc(&od, '/'); }
    sb_put(&od, slug);
    s->outDir = sb_str(&od);
    StrBuf u = {0};
    sb_put(&u, b->base);
    sb_putc(&u, '/');
    sb_put(&u, s->outDir);
    sb_putc(&u, '/');
    s->url = sb_str(&u);
    map_put(&b->bySource, s->path, (int)i);
  }
}
// 4. markdown
static void markdown(Build *b) {
  for (size_t i = 0; i < b->nstatics; i++) map_put(&b->staticPaths, b->statics[i].path, 1);
  for (size_t i = 0; i < b->nsources; i++) {
    Source *s = &b->sources[i];
    LinkCtx ctx;
    ctx.b = b;
    ctx.s = s;
    MdResult md = md_render(s->body, link_resolve, &ctx);
    s->html = md.html;
    s->excerpt = md.excerpt;
    s->toc = md.toc;
    s->ntoc = md.ntoc;
  }
}
// 5. collections
static void collections(Build *b) {
  b->posts = (Source **)arena_alloc((b->nsources + 1) * sizeof(Source *));
  b->nposts = 0;
  for (size_t i = 0; i < b->nsources; i++) if (b->sources[i].isPost) b->posts[b->nposts++] = &b->sources[i];
  if (b->nposts > 1) qsort(b->posts, b->nposts, sizeof(Source *), cmp_post);
  for (size_t k = 0; k < b->nposts; k++) {
    b->posts[k]->newer = k == 0 ? NULL : b->posts[k - 1];
    b->posts[k]->older = k == b->nposts - 1 ? NULL : b->posts[k + 1];
  }
}
// 6. tags
static Tag *tag_by_slug(Build *b, Str slug) {
  for (size_t i = 0; i < b->ntags; i++) if (str_eq(b->tags[i]->slug, slug)) return b->tags[i];
  return NULL;
}
static void tag_add_post(Tag *t, Source *p) {
  if (t->nposts == t->capposts) {
    size_t nc = t->capposts ? t->capposts * 2 : 8;
    Source **ni = (Source **)arena_alloc(nc * sizeof(Source *));
    if (t->posts) memcpy(ni, t->posts, t->nposts * sizeof(Source *));
    t->posts = ni; t->capposts = nc;
  }
  t->posts[t->nposts++] = p;
}
static void source_add_tagref(Source *s, Tag *t) {
  if (s->ntagRefs == s->captagRefs) {
    size_t nc = s->captagRefs ? s->captagRefs * 2 : 4;
    Tag **ni = (Tag **)arena_alloc(nc * sizeof(Tag *));
    if (s->tagRefs) memcpy(ni, s->tagRefs, s->ntagRefs * sizeof(Tag *));
    s->tagRefs = ni; s->captagRefs = nc;
  }
  s->tagRefs[s->ntagRefs++] = t;
}
// A source's tags, each slug once, in its own order (returns names).
static void own_tag_names(Source *s, StrVec *out) {
  out->n = 0;
  Val *tagsv = val_obj_get_lit(s->data, "tags");
  if (!tagsv || tagsv->t != V_ARR) return;
  Str *seen = (Str *)arena_alloc((tagsv->u.arr.len + 1) * sizeof(Str));
  size_t nseen = 0;
  for (size_t i = 0; i < tagsv->u.arr.len; i++) {
    Val *nv = tagsv->u.arr.items[i];
    if (!nv || nv->t != V_STR) continue;
    Str slug = slugify(nv->u.s);
    int dup = 0;
    for (size_t q = 0; q < nseen; q++) if (str_eq(seen[q], slug)) { dup = 1; break; }
    if (dup) continue;
    seen[nseen++] = slug;
    strvec_push(out, nv->u.s);
  }
}
static void tags(Build *b) {
  StrVec names = {0};
  for (size_t pi = 0; pi < b->nposts; pi++) {
    Source *p = b->posts[pi];
    own_tag_names(p, &names);
    for (size_t ti = 0; ti < names.n; ti++) {
      Str name = names.items[ti];
      Str slug = slugify(name);
      Tag *t = tag_by_slug(b, slug);
      if (!t) {
        t = (Tag *)arena_alloc(sizeof(Tag));
        memset(t, 0, sizeof(*t));
        t->name = name;
        t->slug = slug;
        StrBuf u = {0};
        sb_put(&u, b->base);
        sb_puts(&u, "/tags/");
        sb_put(&u, slug);
        sb_putc(&u, '/');
        t->url = sb_str(&u);
        if (b->ntags == b->captags) {
          size_t nc = b->captags ? b->captags * 2 : 16;
          Tag **ni = (Tag **)arena_alloc(nc * sizeof(Tag *));
          if (b->tags) memcpy(ni, b->tags, b->ntags * sizeof(Tag *));
          b->tags = ni; b->captags = nc;
        }
        b->tags[b->ntags++] = t;
      }
      tag_add_post(t, p);
      source_add_tagref(p, t);
    }
  }
  for (size_t i = 0; i < b->nsources; i++) {
    Source *s = &b->sources[i];
    if (s->isPost) continue;
    own_tag_names(s, &names);
    for (size_t ti = 0; ti < names.n; ti++) {
      Str name = names.items[ti];
      Str slug = slugify(name);
      Tag *t = tag_by_slug(b, slug);
      if (!t) {
        t = (Tag *)arena_alloc(sizeof(Tag));
        memset(t, 0, sizeof(*t));
        t->name = name;
        t->slug = slug;
        t->url = str_lit("");
      }
      source_add_tagref(s, t);
    }
  }
  if (b->ntags > 1) qsort(b->tags, b->ntags, sizeof(Tag *), cmp_tag);
}
// 7. layouts
static void layouts(Build *b) {
  Val *site = val_obj_get_lit(b->input, "site");
  long long per = 10, recent = 3;
  Val *perv = site ? val_obj_get_lit(site, "per_page") : NULL;
  Val *recv = site ? val_obj_get_lit(site, "recent") : NULL;
  if (perv && perv->t == V_NUM && perv->u.num.isInt) per = perv->u.num.i;
  if (recv && recv->t == V_NUM && recv->u.num.isInt) recent = recv->u.num.i;
  if (per < 1) per = 1;
  for (size_t i = 0; i < b->nposts; i++) b->posts[i]->card = card_of(b->posts[i]);
  Val *siteVal = val_obj();
  Val *stitle = site ? val_obj_get_lit(site, "title") : NULL;
  val_obj_set(siteVal, str_lit("title"), stitle ? stitle : val_str(str_lit("")));
  val_obj_set(siteVal, str_lit("base_url"), val_str(b->base));
  b->siteVal = siteVal;
  Val *allTags = val_arr();
  for (size_t i = 0; i < b->ntags; i++) {
    Val *t = val_obj();
    val_obj_set(t, str_lit("name"), val_str(b->tags[i]->name));
    val_obj_set(t, str_lit("slug"), val_str(b->tags[i]->slug));
    val_obj_set(t, str_lit("url"), val_str(b->tags[i]->url));
    val_obj_set(t, str_lit("count"), val_int((long long)b->tags[i]->nposts));
    val_arr_push(allTags, t);
  }
  b->allTagsVal = allTags;
  Val *recentVal = val_arr();
  for (long long i = 0; i < recent && i < (long long)b->nposts; i++) val_arr_push(recentVal, b->posts[i]->card);
  b->recentVal = recentVal;
  for (size_t i = 0; i < b->nsources; i++) {
    Source *s = &b->sources[i];
    Val *vars = base_vars(b);
    val_obj_set(vars, str_lit("page"), page_of(s));
    Val *html = render_tpl(b, s->layout, vars);
    StrBuf key = {0};
    sb_put(&key, s->outDir);
    sb_puts(&key, "/index.html");
    out_set(&b->output, sb_str(&key), html ? html->u.s : str_lit(""));
  }
  long long npages = (long long)((b->nposts + (size_t)per - 1) / (size_t)per);
  if (npages < 1) npages = 1;
  for (long long k = 1; k <= npages; k++) {
    Val *vars = base_vars(b);
    Val *posts = val_arr();
    for (long long j = (k - 1) * per; j < k * per && j < (long long)b->nposts; j++) val_arr_push(posts, b->posts[j]->card);
    val_obj_set(vars, str_lit("posts"), posts);
    Val *pager = val_obj();
    val_obj_set(pager, str_lit("number"), val_int(k));
    val_obj_set(pager, str_lit("total"), val_int(npages));
    val_obj_set(pager, str_lit("url"), val_str(index_url(b, k)));
    val_obj_set(pager, str_lit("newer_url"), val_str(k == 1 ? str_lit("") : index_url(b, k - 1)));
    val_obj_set(pager, str_lit("older_url"), val_str(k == npages ? str_lit("") : index_url(b, k + 1)));
    val_obj_set(vars, str_lit("pager"), pager);
    Val *html = render_tpl(b, str_lit("index"), vars);
    if (html) {
      StrBuf key = {0};
      if (k == 1) sb_puts(&key, "index.html");
      else { char tmp[64]; snprintf(tmp, sizeof(tmp), "page/%lld/index.html", k); sb_puts(&key, tmp); }
      out_set(&b->output, sb_str(&key), html->u.s);
    }
  }
  for (size_t i = 0; i < b->ntags; i++) {
    Tag *t = b->tags[i];
    Val *vars = base_vars(b);
    val_obj_set(vars, str_lit("tag"), tag_of_val(t));
    Val *html = render_tpl(b, str_lit("tag"), vars);
    if (html) {
      StrBuf key = {0};
      sb_puts(&key, "tags/");
      sb_put(&key, t->slug);
      sb_puts(&key, "/index.html");
      out_set(&b->output, sb_str(&key), html->u.s);
    }
  }
  {
    Val *vars = base_vars(b);
    Val *html = render_tpl(b, str_lit("tags"), vars);
    if (html) out_set(&b->output, str_lit("tags/index.html"), html->u.s);
  }
  for (size_t i = 0; i < b->nstatics; i++) {
    int exists = 0;
    for (size_t k = 0; k < b->output.n; k++) if (str_eq(b->output.ents[k].key, b->statics[i].path)) { exists = 1; break; }
    if (exists) {
      StrBuf line = {0};
      sb_puts(&line, "shadowed ");
      sb_put(&line, b->statics[i].path);
      sb_puts(&line, ": a built page has its path");
      strvec_push(&b->report, sb_str(&line));
    } else {
      out_set(&b->output, b->statics[i].path, b->statics[i].content);
    }
  }
}
// 8. report
static void report(Build *b) {
  StrVec lines = {0};
  for (size_t i = 0; i < b->report.n; i++) strvec_push(&lines, b->report.items[i]);
  for (size_t i = 0; i < b->broken.cap; i++) {
    if (b->broken.ents[i].used) strvec_push(&lines, b->broken.ents[i].key);
  }
  if (lines.n > 1) qsort(lines.items, lines.n, sizeof(Str), cmp_str_u16);
  size_t pages = 0;
  for (size_t i = 0; i < b->nsources; i++) if (!b->sources[i].isPost) pages++;
  StrBuf rb = {0};
  for (size_t i = 0; i < lines.n; i++) {
    sb_put(&rb, lines.items[i]);
    sb_putc(&rb, '\n');
  }
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "built %zu pages, %zu posts, %zu tags and %zu static files\n", pages, b->nposts, b->ntags, b->nstatics);
  sb_puts(&rb, tmp);
  out_set(&b->output, str_lit("build-report.txt"), sb_str(&rb));
}

static Val *build_case(Val *input) {
  Build b;
  memset(&b, 0, sizeof(b));
  b.input = input;
  Val *site = val_obj_get_lit(input, "site");
  Val *baseUrl = site ? val_obj_get_lit(site, "base_url") : NULL;
  Str base = baseUrl && baseUrl->t == V_STR ? baseUrl->u.s : str_lit("");
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;
  b.base = base;
  // templates, parsed in code-unit order of their names; failures go to the report
  Val *templates = val_obj_get_lit(input, "templates");
  if (templates && templates->t == V_OBJ && templates->u.obj.len > 0) {
    size_t nt = templates->u.obj.len;
    Str *names = (Str *)arena_alloc(nt * sizeof(Str));
    for (size_t i = 0; i < nt; i++) names[i] = templates->u.obj.kvs[i].key;
    if (nt > 1) qsort(names, nt, sizeof(Str), cmp_str_key);
    b.tpl.templates = (Template *)arena_alloc(nt * sizeof(Template));
    for (size_t i = 0; i < nt; i++) {
      Val *src = val_obj_get(templates, names[i]);
      Str srcStr = src && src->t == V_STR ? src->u.s : str_lit("");
      TNode **nodes = NULL;
      size_t nnodes = 0;
      char err[1024] = {0};
      if (parse_template(names[i], srcStr, &nodes, &nnodes, err, sizeof(err))) {
        b.tpl.templates[b.tpl.ntemplates].name = names[i];
        b.tpl.templates[b.tpl.ntemplates].nodes = nodes;
        b.tpl.templates[b.tpl.ntemplates].nnodes = nnodes;
        b.tpl.ntemplates++;
      } else {
        StrBuf line = {0};
        sb_puts(&line, err);
        strvec_push(&b.report, sb_str(&line));
      }
    }
  }
  map_init(&b.bySource, 4096);
  map_init(&b.staticPaths, 1024);
  map_init(&b.broken, 4096);
  read_sources(&b);
  front_matter(&b);
  permalinks(&b);
  markdown(&b);
  collections(&b);
  tags(&b);
  layouts(&b);
  report(&b);
  Val *out = val_obj();
  for (size_t i = 0; i < b.output.n; i++) val_obj_set(out, b.output.ents[i].key, val_str(b.output.ents[i].val));
  return out;
}

// ---------------------------------------------------------------- JSON output (JSON.stringify, compact)
static void write_json(StrBuf *out, Val *v) {
  switch (v->t) {
    case V_NULL: sb_puts(out, "null"); break;
    case V_BOOL: sb_puts(out, v->u.boolean ? "true" : "false"); break;
    case V_NUM: {
      if (v->u.num.isInt) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", v->u.num.i);
        sb_puts(out, tmp);
      } else {
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%.17g", v->u.num.d);
        sb_puts(out, tmp);
      }
      break;
    }
    case V_STR:
      sb_putc(out, '"');
      json_escape(out, v->u.s);
      sb_putc(out, '"');
      break;
    case V_ARR:
      sb_putc(out, '[');
      for (size_t i = 0; i < v->u.arr.len; i++) {
        if (i) sb_putc(out, ',');
        write_json(out, v->u.arr.items[i]);
      }
      sb_putc(out, ']');
      break;
    case V_OBJ:
      sb_putc(out, '{');
      for (size_t i = 0; i < v->u.obj.len; i++) {
        if (i) sb_putc(out, ',');
        sb_putc(out, '"');
        json_escape(out, v->u.obj.kvs[i].key);
        sb_putc(out, '"');
        sb_putc(out, ':');
        write_json(out, v->u.obj.kvs[i].val);
      }
      sb_putc(out, '}');
      break;
  }
}

static char *read_whole_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = 0;
  *out_len = got;
  return buf;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *inDir = getenv("MAKEFUN_INPUT_DIR");
  const char *outDir = getenv("MAKEFUN_OUTPUT_DIR");
  if (!inDir) inDir = "/input";
  if (!outDir) outDir = "/output";
  char inPath[4096];
  snprintf(inPath, sizeof(inPath), "%s/cases.json", inDir);
  size_t len = 0;
  char *buf = read_whole_file(inPath, &len);
  if (!buf) {
    fprintf(stderr, "sitegen: cannot read %s\n", inPath);
    return 1;
  }
  JParser j;
  j.p = buf;
  j.n = len;
  j.i = 0;
  j.err = 0;
  Val *cases = j_parse_value(&j);
  if (j.err || !cases || cases->t != V_ARR) {
    fprintf(stderr, "sitegen: bad cases.json\n");
    return 1;
  }
  Val *answers = val_arr();
  for (size_t i = 0; i < cases->u.arr.len; i++) {
    Val *c = cases->u.arr.items[i];
    Val *input = val_obj_get_lit(c, "input");
    if (!input) {
      fprintf(stderr, "sitegen: case %zu has no input\n", i);
      return 1;
    }
    val_arr_push(answers, build_case(input));
  }
  StrBuf sb = {0};
  write_json(&sb, answers);
  // atomic write: temp file + rename (best-effort mkdir: the harness mounts
  // the output dir, but a missing one must not fail the run outright)
  mkdir(outDir, 0777);
  char tmpPath[4096], outPath[4096];
  snprintf(outPath, sizeof(outPath), "%s/answers.json", outDir);
  snprintf(tmpPath, sizeof(tmpPath), "%s/answers.json.tmp.%ld", outDir, (long)getpid());
  FILE *f = fopen(tmpPath, "wb");
  if (!f) {
    fprintf(stderr, "sitegen: cannot write %s\n", tmpPath);
    return 1;
  }
  if (sb.len > 0 && fwrite(sb.buf, 1, sb.len, f) != sb.len) {
    fclose(f);
    unlink(tmpPath);
    fprintf(stderr, "sitegen: write failed\n");
    return 1;
  }
  if (fclose(f) != 0) {
    unlink(tmpPath);
    fprintf(stderr, "sitegen: close failed\n");
    return 1;
  }
  if (rename(tmpPath, outPath) != 0) {
    unlink(tmpPath);
    fprintf(stderr, "sitegen: rename failed\n");
    return 1;
  }
  // The driver polls for answers.json and exits as soon as it appears; it
  // must exit BEFORE this process does, or the driver reaps this process and
  // the build's CPU is folded into the entrypoint's. Linger half a second
  // (blocked, no CPU) so the driver is gone first. The answer is already
  // complete on disk (the rename is atomic and was this process's last act).
  {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 500L * 1000L * 1000L;
    nanosleep(&ts, NULL);
  }
  return 0;
}
