/*
 * hao — signature-aware arm64 (Apple AAPCS64) <-> x86_64 (SysV) calls.
 *
 * The generic register bridge is exact for scalar signatures. Structures
 * are classified differently by the two ABIs (e.g. NSRect is an HFA in
 * v0-v3 on arm64 but a MEMORY-class stack argument / sret on x86_64), so
 * calls whose ObjC type encoding contains such types go through here:
 *   guest -> native:  gather arm64 args by encoding, lay them out per SysV
 *   native -> guest:  a signature thunk decodes SysV args; we place them in
 *                     guest registers/stack and run the translated callee
 */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXARGS 64      /* arguments of one signature, self and _cmd included */
#define MAXSIZE 65536   /* larger by-value aggregates are not marshalled */

typedef struct htype {
    char kind;          /* 'v' void, 'i' int-class, 'f' float, 'd' double, 'D' long double, 's' struct */
    int size, align;
    int sgn;            /* sign-extend when widening (signed int < 8 bytes) */
    int hfa;            /* number of HFA members (0 = not an HFA) */
    char hfa_kind;      /* 'f' or 'd' */
    uint8_t ebc[2];     /* x86_64 SysV class per eightbyte: 0 none, 1 SSE, 2 INTEGER */
    bool bitfield;      /* an aggregate holding bitfields */
    bool block;         /* '@?': a block pointer */
    uint64_t code;      /* 8-byte slots holding code ('^?' function pointers, '@?' blocks): bit k = offset 8k;
                         * 0 for unions and layouts the encoding does not determine */
    bool amb_arg, amb_ret; /* other layouts the encoding admits are passed / returned differently */
    int size_max;       /* the largest size of those layouts */
    /* An aggregate holding a long double (ld): Apple arm64 lays it out with an 8-byte double (size,
     * alignment and HFA classification above), x86_64 SysV with a 16-byte, 16-aligned x87 value. The
     * native layout is nsize/nalign; lv lists the leaves that convert between the two. SysV passes
     * such an aggregate in memory and returns it in memory, except one that is a single long double
     * (nst0), which comes back in st0. */
    bool ld, nst0;
    int nsize, nalign, nlv;
    const struct ldleaf *lv;
} htype;
enum { EB_NONE = 0, EB_SSE = 1, EB_INT = 2 };
static void eb_merge(uint8_t *dst, uint8_t c) { if (c > *dst) *dst = c; }

typedef struct hsig {
    htype ret;
    int nargs;
    bool simple;
    bool ambiguous;     /* a bitfield layout was assumed, not determined */
    bool unmarshalable; /* too many arguments or too large an aggregate */
    bool fnret;         /* the result is a function pointer ('^?') */
    uint16_t codeslots; /* arm64 argument slots holding code (arm64_code_slots) */
    htype args[];       /* nargs (at most MAXARGS) */
} hsig;

/* ------------------------------------------------------- encoding parser
 *
 * Bitfields. An encoding gives a bitfield's width ("b8") but not its
 * declared type, and the type decides the layout. clang lays out bitfields
 * the same way for Darwin x86_64 and arm64: a bitfield of type T goes at
 * the next bit where it does not straddle a sizeof(T) boundary, a
 * zero-width bitfield moves to the next such boundary, and only named
 * bitfields raise the aggregate's alignment to alignof(T). The parser keeps
 * every layout the encoding admits (each bitfield declared char, short,
 * int or long long, named or not), the assumed one first: unsigned int
 * storage (long long beyond 32 bits), named, which is how NSDecimal and
 * nearly all SDK bitfields are declared. Method and block encodings also
 * give each argument's size, which narrows the candidates (parse_sig). A
 * type is ambiguous when its candidates are passed or returned
 * differently. Whatever the layout, the call paths never copy a parsed
 * size into memory they do not own: a struct returned through memory is
 * written in place by the callee (hao_call_native_sig, run_guest_with_args).
 */

static const char *skip_quals(const char *p)
{
    while (*p && strchr("rnNoORVAj", *p)) p++;
    return p;
}
static const char *skip_digits(const char *p)
{
    while (*p == '-' || (*p >= '0' && *p <= '9')) p++;
    return p;
}

#define LAYMAX 16       /* candidate layouts per type */
#define STMAX 32        /* partial layouts while laying out one aggregate */
#define PARSE_DEPTH 16

/* A candidate layout: size, alignment, and the x86_64 SysV class of each of
 * the first 16 bytes. Classifying bytes at their absolute offsets merges
 * every leaf into the eightbyte that holds it, as the SysV algorithm does
 * for nested aggregates. While an aggregate is laid out, `n` counts bits
 * (the data size so far); in a finished layout it is the size in bytes. */
typedef struct { int n, align; uint8_t bc[16]; } hlay;     /* no padding: compared with memcmp */

/* Per-type facts gathered while parsing, beyond htype:
 *  lay    candidate layouts, lay[0] the assumed one
 *  over   more candidates than fit: the type counts as ambiguous
 *  bad    malformed (truncated, unknown type, absurd size): not marshalled
 *  hom    homogeneous FP members for AAPCS64 HFA detection: -1 not
 *         homogeneous, 0 none (empty), n members of kind homk */
typedef struct {
    hlay lay[LAYMAX];
    int nlay;
    bool over, bad;
    int hom;
    char homk;
} hinfo;

static void bc_fill(uint8_t *bc, int off, int len, uint8_t cls)
{
    for (int i = off < 0 ? 0 : off; i < off + len && i < 16; i++) eb_merge(&bc[i], cls);
}
static void bc_merge_at(uint8_t *bc, int off, const uint8_t *src)
{
    for (int i = 0; off + i < 16; i++) eb_merge(&bc[off + i], src[i]);
}
static bool lay_add(hlay *v, int *n, int max, const hlay *x)
{
    for (int i = 0; i < *n; i++) if (!memcmp(&v[i], x, sizeof *x)) return true;
    if (*n == max) return false;
    v[(*n)++] = *x;
    return true;
}
/* fold a member's homogeneity into its aggregate's (sum for structs and
 * arrays, max for unions) */
static void hom_merge(hinfo *h, const hinfo *m, bool sum)
{
    if (h->hom < 0) return;
    if (m->hom < 0 || (m->hom && h->homk && m->homk != h->homk)) { h->hom = -1; return; }
    if (!m->hom) return;
    h->homk = m->homk;
    h->hom = sum ? (h->hom + m->hom > MAXSIZE ? MAXSIZE : h->hom + m->hom) : (m->hom > h->hom ? m->hom : h->hom);
}

/* The declarations a bitfield of width w may have, as (storage unit bytes,
 * named); the assumed one first. */
static int bf_decls(int w, int unit[9], bool named[9])
{
    int n = 0, conv = w > 32 ? 8 : 4;
    unit[n] = conv; named[n++] = w > 0;                       /* a zero-width bitfield has no name */
    for (int u = 1; u <= 8; u *= 2) {
        if (w > 8 * u) continue;
        for (int nm = w > 0; nm >= 0; nm--)
            if (!(u == conv && nm == (w > 0))) { unit[n] = u; named[n++] = nm; }
    }
    return n;
}
static void bf_place(hlay *x, bool in_union, int w, int unit, bool named)
{
    int bits = 8 * unit;
    if (in_union) {
        if (w > x->n) x->n = w;
        bc_fill(x->bc, 0, (w + 7) / 8, EB_INT);
    } else {
        if (!w || x->n % bits + w > bits) x->n = (x->n + bits - 1) / bits * bits;
        if (w) bc_fill(x->bc, x->n / 8, (x->n + w - 1) / 8 - x->n / 8 + 1, EB_INT);
        x->n += w;
    }
    if (named && unit > x->align) x->align = unit;
}

/* SysV eightbyte classes of a finished layout */
static void lay_ebc(const hlay *L, bool aggregate, uint8_t ebc[2])
{
    if (!aggregate) { ebc[0] = L->bc[0]; ebc[1] = EB_NONE; return; }
    uint8_t e[2] = {0, 0};
    for (int i = 0; i < 16; i++) eb_merge(&e[i / 8], L->bc[i]);
    /* an eightbyte of padding only still occupies a register */
    ebc[0] = e[0] ? e[0] : (L->n ? EB_INT : EB_NONE);
    ebc[1] = L->n > 8 ? (e[1] ? e[1] : EB_INT) : EB_NONE;
}
/* AAPCS64 HFA: 1-4 members of one FP kind and no padding */
static int lay_hfa(const hinfo *h, const hlay *L)
{
    int esz = h->homk == 'f' ? 4 : 8;
    return h->hom >= 1 && h->hom <= 4 && L->n == h->hom * esz ? h->hom : 0;
}
/* How a value of this layout is passed (arg) or returned by each ABI:
 * equal shapes are marshalled identically. */
static uint32_t lay_shape(const hinfo *h, const hlay *L, bool arg)
{
    uint8_t ebc[2];
    lay_ebc(L, true, ebc);
    int hfa = lay_hfa(h, L), words = (L->n + 7) / 8;
    uint32_t a64 = hfa ? 0x10u | (uint32_t)hfa : L->n > 16 ? 0x20u : 0x30u | (uint32_t)words;
    uint32_t sysv = L->n > 16 ? 0x8000u | (arg ? (uint32_t)(words & 0x7fff) : 0) : (uint32_t)(ebc[0] << 2 | ebc[1]);
    return a64 << 16 | sysv;
}
/* make layout L the one htype t describes */
static void type_from_lay(htype *t, const hinfo *h, const hlay *L)
{
    t->size = L->n; t->align = L->align;
    lay_ebc(L, t->kind == 's', t->ebc);
    t->hfa = t->kind == 's' ? lay_hfa(h, L) : 0;
    t->hfa_kind = t->hfa ? h->homk : 0;
}
/* is every candidate of size `size` (any size if < 0) passed like L? */
static bool lay_ambiguous(const hinfo *h, const hlay *L, int size, bool arg)
{
    if (h->over) return true;
    for (int j = 0; j < h->nlay; j++)
        if ((size < 0 || h->lay[j].n == size) && lay_shape(h, &h->lay[j], arg) != lay_shape(h, L, arg)) return true;
    return false;
}

static const char *parse_type(const char *p, htype *t, hinfo *h, int depth)
{
    p = skip_quals(p);
    memset(t, 0, sizeof *t);
    h->nlay = 0; h->over = h->bad = false; h->hom = 0; h->homk = 0;
    char c = *p;
    if (c) p++;                                               /* never step over the terminator */
    if (depth > PARSE_DEPTH) c = 0;
    switch (c) {
    case 'c': t->kind = 'i'; t->size = t->align = 1; t->sgn = 1; break;
    case 'C': case 'B': t->kind = 'i'; t->size = t->align = 1; break;
    case 's': t->kind = 'i'; t->size = t->align = 2; t->sgn = 1; break;
    case 'S': t->kind = 'i'; t->size = t->align = 2; break;
    case 'i': case 'l': t->kind = 'i'; t->size = t->align = 4; t->sgn = 1; break;
    case 'I': case 'L': t->kind = 'i'; t->size = t->align = 4; break;
    case 'q': case 'Q': t->kind = 'i'; t->size = t->align = 8; break;
    case 'f': t->kind = 'f'; t->size = t->align = 4; break;
    case 'd': t->kind = 'd'; t->size = t->align = 8; break;
    case 'D': t->kind = 'D'; t->size = t->align = 8; break;
    case 'v': t->kind = 'v'; break;
    case '@':
        if (*p == '?') {                                      /* block, extended: @?<v@?...> */
            p++;
            t->block = true;
            t->code = 1;
            if (*p == '<') for (int d = 0; *p; p++) { if (*p == '<') d++; else if (*p == '>' && --d == 0) { p++; break; } }
        }
        else if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
        /* fallthrough */
    case '*': case '#': case ':': case '?':
        t->kind = 'i'; t->size = t->align = 8; break;
    case '^': {
        htype inner; hinfo ih;
        bool fn = *p == '?';                                  /* '^?': a function pointer */
        p = parse_type(p, &inner, &ih, depth + 1);           /* a pointer is a word whatever it points to, */
        t->kind = 'i'; t->size = t->align = 8;
        t->code = fn;
        h->bad = inner.kind == 'v' && ih.bad;                 /* but the pointee must be there */
        break;
    }
    case 'b':                                                 /* a bitfield outside an aggregate */
        p = skip_digits(p); t->kind = 'i'; t->size = t->align = 4; t->bitfield = true; h->bad = true;
        break;
    case '[': {
        long n = strtol(p, (char **)&p, 10);
        htype el; hinfo eh;
        p = skip_digits(parse_type(p, &el, &eh, depth + 1));
        if (*p == ']') p++; else h->bad = true;
        if (n < 0 || n > MAXSIZE || el.kind == 'v') { n = 0; h->bad = true; }
        t->kind = 's'; t->bitfield = el.bitfield;
        if (el.code && eh.nlay == 1 && el.size > 0 && el.size % 8 == 0)
            for (long k = 0; k < n && k * (el.size / 8) < 64; k++) t->code |= el.code << (k * (el.size / 8));
        h->over |= eh.over; h->bad |= eh.bad;
        if (n) { h->hom = eh.hom < 0 ? -1 : (int)((int64_t)eh.hom * n > MAXSIZE ? MAXSIZE : (int64_t)eh.hom * n); h->homk = eh.homk; }
        for (int j = 0; j < eh.nlay; j++) {
            const hlay *E = &eh.lay[j];
            hlay L;
            memset(&L, 0, sizeof L);
            int64_t bytes = (int64_t)E->n * n;                /* both <= MAXSIZE: no int overflow here */
            if (bytes > MAXSIZE) { bytes = MAXSIZE; h->bad = true; }
            L.n = (int)bytes; L.align = E->align;
            for (int k = 0; k < n && E->n && k * E->n < 16; k++) bc_merge_at(L.bc, k * E->n, E->bc);
            lay_add(h->lay, &h->nlay, LAYMAX, &L);
        }
        break;
    }
    case '{': case '(': {
        bool uni = c == '(';
        char close = uni ? ')' : '}';
        while (*p && *p != '=' && *p != close) p++;
        if (*p == '=') p++;
        hlay st[STMAX], nx[STMAX];
        int ns = 1;
        memset(&st[0], 0, sizeof st[0]);
        st[0].align = 1;
        bool bits = false, members = false;
        uint64_t code = 0;
        bool code_bad = uni;                                  /* which union member is live is unknown */
        while (*p && *p != close) {
            if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; continue; }
            int nn = 0;
            members = true;
            if (*p == 'b') {
                long w = strtol(p + 1, (char **)&p, 10);
                if (w < 0 || w > 64) { w = w < 0 ? 0 : 64; h->bad = true; }    /* not a C bitfield */
                bits = true;
                if (w) h->hom = -1;                           /* AAPCS64 ignores zero-width bitfields */
                int unit[9]; bool named[9];
                int nd = bf_decls((int)w, unit, named);
                for (int i = 0; i < ns; i++) for (int d = 0; d < nd; d++) {
                    hlay x = st[i];
                    bf_place(&x, uni, (int)w, unit[d], named[d]);
                    if (!lay_add(nx, &nn, STMAX, &x)) h->over = true;
                }
            } else {
                htype f; hinfo fh;
                p = skip_digits(parse_type(p, &f, &fh, depth + 1));
                if (f.kind == 'v') { h->bad = true; break; }
                if (f.code && !code_bad) {                    /* code slots at the field's offset, one layout only */
                    int fa = fh.lay[0].align ? fh.lay[0].align : 1;
                    int off = ((st[0].n + 7) / 8 + fa - 1) / fa * fa;
                    if (ns != 1 || fh.nlay != 1 || off % 8 || off / 8 >= 64) code_bad = true;
                    else code |= f.code << (off / 8);
                }
                bits |= f.bitfield;
                h->over |= fh.over; h->bad |= fh.bad;
                hom_merge(h, &fh, !uni);
                for (int i = 0; i < ns; i++) for (int j = 0; j < fh.nlay; j++) {
                    hlay x = st[i];
                    const hlay *F = &fh.lay[j];
                    int off = uni ? 0 : ((x.n + 7) / 8 + F->align - 1) / F->align * F->align;
                    if (off < 16) bc_merge_at(x.bc, off, F->bc);
                    int end = 8 * (off + F->n);
                    if (end > 8 * MAXSIZE) { end = 8 * MAXSIZE; h->bad = true; }
                    if (!uni || end > x.n) x.n = end;
                    if (F->align > x.align) x.align = F->align;
                    if (!lay_add(nx, &nn, STMAX, &x)) h->over = true;
                }
            }
            memcpy(st, nx, (size_t)nn * sizeof *nx);
            ns = nn;
        }
        if (*p == close) p++; else h->bad = true;
        if (!members) h->bad = true;                          /* contents not encoded (vector types, C++) */
        t->code = code_bad || ns != 1 ? 0 : code;
        for (int i = 0; i < ns; i++) {
            hlay L = st[i];
            L.n = ((L.n + 7) / 8 + L.align - 1) / L.align * L.align;
            if (!lay_add(h->lay, &h->nlay, LAYMAX, &L)) h->over = true;
        }
        t->kind = 's'; t->bitfield = bits;
        break;
    }
    case 0:                                                   /* truncated, or nested too deeply */
        t->kind = 'v'; h->bad = true; break;
    default:                                                  /* unknown, e.g. a vector type's empty encoding */
        t->kind = 'i'; t->size = t->align = 8; h->bad = true; break;
    }
    if (t->kind != 's') {
        hlay *L = &h->lay[0];
        memset(L, 0, sizeof *L);
        L->n = t->size; L->align = t->align ? t->align : 1;
        h->nlay = 1;
        if (t->kind == 'i') { bc_fill(L->bc, 0, t->size, EB_INT); h->hom = -1; }
        else if (t->kind != 'v') { bc_fill(L->bc, 0, t->size, EB_SSE); h->hom = 1; h->homk = t->kind == 'f' ? 'f' : 'd'; }
    }
    type_from_lay(t, h, &h->lay[0]);
    t->size_max = 0;
    for (int j = 0; j < h->nlay; j++) if (h->lay[j].n > t->size_max) t->size_max = h->lay[j].n;
    if (t->kind == 's') {
        t->amb_arg = lay_ambiguous(h, &h->lay[0], -1, true);
        t->amb_ret = lay_ambiguous(h, &h->lay[0], -1, false);
    }
    return p;
}

/* ------------------------------------------------ long double aggregates
 *
 * Apple arm64's long double is double, x86_64's an x87 value in 16 bytes;
 * an aggregate holding one has two layouts. Both are computed here from the
 * encoding, member by member with each ABI's alignment, as leaves: a long
 * double, converted, or bytes laid out alike, copied. A union holding a long
 * double (which member is live is unknown) and a bitfield layout the
 * encoding does not determine are not marshalled. */

typedef struct ldleaf { uint32_t goff, noff, size; char kind; } ldleaf;   /* kind 'D' or 'b' */
#define LDLEAVES 256
typedef struct { ldleaf v[LDLEAVES]; int n; bool bad, has_d; } ldwalk;

static void ld_leaf(ldwalk *w, uint32_t go, uint32_t no, uint32_t size, char kind)
{
    if (w->n && kind == 'b' && w->v[w->n - 1].kind == 'b') {          /* merge with a contiguous byte run */
        ldleaf *l = &w->v[w->n - 1];
        if (l->goff + l->size == go && l->noff + l->size == no) { l->size += size; return; }
    }
    if (w->n == LDLEAVES) { w->bad = true; return; }
    w->v[w->n++] = (ldleaf){go, no, size, kind};
}
static bool text_has_d(const char *a, const char *b)
{
    for (const char *q = a; q < b; q++) {
        if (*q == '"') { q++; while (q < b && *q != '"') q++; continue; }
        if (*q == 'D') return true;
    }
    return false;
}
static int align_up(int x, int a) { return a > 1 ? (x + a - 1) / a * a : x; }

/* The type at p in both layouts; its leaves are appended at offsets 0 (the caller shifts them). */
static const char *ld_type(const char *p, ldwalk *w, int depth, int *gs, int *ga, int *ns, int *na)
{
    p = skip_quals(p);
    *gs = *ns = 0; *ga = *na = 1;
    if (depth > PARSE_DEPTH) { w->bad = true; return p; }
    if (*p == 'D') { ld_leaf(w, 0, 0, 8, 'D'); w->has_d = true; *gs = *ga = 8; *ns = *na = 16; return p + 1; }
    htype t; hinfo h;
    const char *e = parse_type(p, &t, &h, depth);
    bool agg = *p == '{' || *p == '[';
    if (*p == '(' && text_has_d(p, e)) { w->bad = true; return e; }  /* a union with a long double member */
    if (!agg || !text_has_d(p, e)) {                                  /* laid out alike: bytes */
        if (h.bad || h.over || h.nlay != 1 || t.kind == 'v') w->bad = true;
        *gs = *ns = t.size; *ga = *na = t.align ? t.align : 1;
        if (t.size) ld_leaf(w, 0, 0, (uint32_t)t.size, 'b');
        return e;
    }
    if (*p == '[') {
        long n = strtol(p + 1, (char **)&p, 10);
        if (n <= 0 || n > MAXSIZE) { w->bad = true; return e; }
        int first = w->n, es, ea, ens, ena;
        ld_type(p, w, depth + 1, &es, &ea, &ens, &ena);
        int last = w->n;
        for (long k = 1; k < n && !w->bad; k++)
            for (int j = first; j < last; j++) {
                ldleaf l = w->v[j];
                ld_leaf(w, l.goff + (uint32_t)(k * es), l.noff + (uint32_t)(k * ens), l.size, l.kind);
            }
        *gs = (int)(n * es); *ns = (int)(n * ens); *ga = ea; *na = ena;
        if (*gs > MAXSIZE || *ns > MAXSIZE) w->bad = true;
        return e;
    }
    const char *q = p + 1;                                            /* '{' */
    while (*q && *q != '=' && *q != '}') q++;
    if (*q == '=') q++;
    int go = 0, no = 0, gal = 1, nal = 1;
    while (*q && *q != '}' && !w->bad) {
        if (*q == '"') { q++; while (*q && *q != '"') q++; if (*q) q++; continue; }
        if (*q == 'b') { w->bad = true; break; }                       /* bitfields beside a long double */
        int first = w->n, ms, ma, mns, mna;
        q = skip_digits(ld_type(q, w, depth + 1, &ms, &ma, &mns, &mna));
        go = align_up(go, ma); no = align_up(no, mna);
        for (int j = first; j < w->n; j++) { w->v[j].goff += (uint32_t)go; w->v[j].noff += (uint32_t)no; }
        go += ms; no += mns;
        if (ma > gal) gal = ma;
        if (mna > nal) nal = mna;
        if (go > MAXSIZE || no > MAXSIZE) w->bad = true;
    }
    *gs = align_up(go, gal); *ns = align_up(no, nal); *ga = gal; *na = nal;
    return e;
}

/* Give the top-level aggregate t (encoding at enc) its native layout when it holds a long double.
 * Returns false when it does but cannot be converted. */
static bool ld_layout(htype *t, const char *enc)
{
    const char *p = skip_quals(enc);
    if (t->kind != 's' || (*p != '{' && *p != '[' && *p != '(')) return true;
    htype tt; hinfo hh;
    const char *e = parse_type(p, &tt, &hh, 0);
    if (!text_has_d(p, e)) return true;
    ldwalk *w = calloc(1, sizeof *w);
    if (!w) return false;
    int gs, ga, ns, na;
    ld_type(p, w, 0, &gs, &ga, &ns, &na);
    if (!w->has_d) { free(w); return true; }                          /* a 'D' only in pointees */
    bool ok = !w->bad && gs == t->size && ga == t->align;
    if (ok) {
        ldleaf *lv = malloc((size_t)w->n * sizeof *lv);               /* kept with the cached signature */
        if (!lv) ok = false;
        else {
            memcpy(lv, w->v, (size_t)w->n * sizeof *lv);
            t->ld = true;
            t->nsize = ns; t->nalign = na; t->lv = lv; t->nlv = w->n;
            t->nst0 = ns == 16 && w->n == 1 && lv[0].kind == 'D';
        }
    }
    free(w);
    return ok;
}
/* guest (arm64) bytes of t -> native (x86_64) bytes, and back */
static void ld_to_native(const htype *t, const uint8_t *g, uint8_t *n)
{
    memset(n, 0, (size_t)t->nsize);
    for (int k = 0; k < t->nlv; k++) {
        const ldleaf *l = &t->lv[k];
        if (l->kind == 'b') { memcpy(n + l->noff, g + l->goff, l->size); continue; }
        double d; long double ld;
        memcpy(&d, g + l->goff, 8);
        ld = d;
        memcpy(n + l->noff, &ld, 10);
    }
}
static void ld_to_guest(const htype *t, const uint8_t *n, uint8_t *g)
{
    for (int k = 0; k < t->nlv; k++) {
        const ldleaf *l = &t->lv[k];
        if (l->kind == 'b') { memcpy(g + l->goff, n + l->noff, l->size); continue; }
        long double ld = 0; double d;
        memcpy(&ld, n + l->noff, 10);
        d = (double)ld;
        memcpy(g + l->goff, &d, 8);
    }
}

/* x86_64 SysV and arm64 agree for this signature with the register bridge? */
static bool sig_simple(const hsig *s)
{
    int ni = 0, nf = 0;
    if (s->ret.ld) return false;
    for (int k = 0; k < s->nargs; k++) if (s->args[k].ld) return false;
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        if (a->kind == 'i') ni++;
        else if (a->kind == 'f' || a->kind == 'd') nf++;
        else if (a->kind == 's' && !a->hfa && a->size <= 16 && a->ebc[0] == EB_INT && (a->size <= 8 || a->ebc[1] == EB_INT)) {
            /* both ABIs: consecutive GPRs, except that SysV never splits an aggregate between
             * registers and the stack: a two-eightbyte struct after 5 integer registers goes to
             * the stack (later integers still use r9) while arm64 puts it in x5, x6 */
            if (a->size > 8 && ni == 5) return false;
            ni += (a->size + 7) / 8;
        }
        else return false;
    }
    if (ni > 8 || nf > 8) return false;
    const htype *r = &s->ret;
    if (r->kind == 'v' || r->kind == 'i' || r->kind == 'f' || r->kind == 'd') return true;
    if (r->kind == 's') {
        if (r->hfa == 2 && r->hfa_kind == 'd') return true;       /* v0,v1 == xmm0,xmm1 */
        if (r->hfa == 1) return true;
        if (!r->hfa && r->size <= 16 && r->ebc[0] == EB_INT && (r->size <= 8 || r->ebc[1] == EB_INT)) {
            return true;                                      /* x0,x1 == rax,rdx */
        }
    }
    return false;
}

/* the frame offset after a type in a method or block encoding */
static const char *parse_offset(const char *p, long *off)
{
    if (*p == '-' || (*p >= '0' && *p <= '9')) { *off = strtol(p, (char **)&p, 10); return skip_digits(p); }
    *off = -1;
    return p;
}

/* Method and block encodings give each argument's frame offset, and clang
 * advances it by the argument's size (sizeof for an aggregate): only the
 * candidate layouts of that size remain. With none of that size (a packed
 * struct, which no encoding marks), an aggregate over 16 bytes is still
 * exact, since both ABIs then pass its bytes in memory; a smaller one is
 * ambiguous. */
static void fit_encoded_size(htype *a, const hinfo *h, long size)
{
    if (a->ld) { a->amb_arg = size != a->size && size != a->nsize; return; }   /* arm64 or x86_64 encoding */
    if (a->kind != 's' || (size == a->size && !a->bitfield)) return;
    for (int j = 0; j < h->nlay; j++)
        if (h->lay[j].n == size) {
            type_from_lay(a, h, &h->lay[j]);
            a->amb_arg = lay_ambiguous(h, &h->lay[j], (int)size, true);
            return;
        }
    if (size > 16 && size <= MAXSIZE && a->size > 16 && !a->hfa && !h->over) {
        a->size = (int)size;
        if (a->size > a->size_max) a->size_max = a->size;
        a->amb_arg = false;
        return;
    }
    a->amb_arg = true;
}

static uint16_t arm64_code_slots(const hsig *s);

static hsig *parse_sig(const char *enc)
{
    hsig *s = calloc(1, sizeof *s + MAXARGS * sizeof(htype));
    if (!s) { fprintf(stderr, "hao: signature cache: out of memory\n"); abort(); }
    hinfo h[2];
    long total, off, prev_off = -1;
    bool offsets = true;
    const char *p = parse_offset(parse_type(enc, &s->ret, &h[0], 0), &total);
    if (total < 0) offsets = false;
    if (h[0].bad) s->unmarshalable = true;
    if (!ld_layout(&s->ret, enc)) s->unmarshalable = true;
    while (*p) {
        if (s->nargs == MAXARGS) { s->unmarshalable = true; break; }
        htype a;
        hinfo *ha = &h[s->nargs & 1];
        bool array = *skip_quals(p) == '[';
        const char *ta = p;
        p = parse_offset(parse_type(p, &a, ha, 0), &off);
        if (!array && !ld_layout(&a, ta)) s->unmarshalable = true;
        if (a.kind == 'v') { s->unmarshalable |= ha->bad; break; }
        if (ha->bad) s->unmarshalable = true;
        if (array) {                                          /* a C array parameter is a pointer */
            memset(&a, 0, sizeof a);
            a.kind = 'i'; a.size = a.size_max = a.align = 8; a.ebc[0] = EB_INT;
        }
        if (off < 0 || off < prev_off) offsets = false;
        if (offsets && s->nargs) fit_encoded_size(&s->args[s->nargs - 1], &h[(s->nargs - 1) & 1], off - prev_off);
        prev_off = off;
        s->args[s->nargs++] = a;
    }
    if (offsets && s->nargs && total >= prev_off) fit_encoded_size(&s->args[s->nargs - 1], &h[(s->nargs - 1) & 1], total - prev_off);
    s->ambiguous = s->ret.amb_ret;
    if (s->ret.size_max > MAXSIZE) s->unmarshalable = true;
    if (s->unmarshalable && hao_verbose) fprintf(stderr, "hao: %s: malformed or too large; not marshalled\n", enc);
    for (int k = 0; k < s->nargs; k++) {
        s->ambiguous |= s->args[k].amb_arg;
        if (s->args[k].size_max > MAXSIZE) s->unmarshalable = true;
    }
    s->simple = sig_simple(s);
    s->codeslots = s->unmarshalable ? 0 : arm64_code_slots(s);
    s->fnret = s->ret.kind == 'i' && s->ret.code && !s->ret.block;
    hsig *fit = realloc(s, sizeof *s + (size_t)s->nargs * sizeof(htype));   /* kept for the process: no slack */
    if (fit) s = fit;
    if (s->ambiguous && !s->unmarshalable && hao_verbose)
        fprintf(stderr, "hao: %s: the layout is not fully determined by the encoding (bitfield types, packing); "
                "assuming unsigned int bitfields, natural alignment\n", enc);
    return s;
}

/* cache by encoding string contents: hash buckets of parsed signatures. It never fills up (a
 * full fixed table used to make every later signature look "simple"). */
#define SIGCACHE 4096
typedef struct signode { struct signode *next; char *enc; hsig *sig; } signode;
static signode *sigcache[SIGCACHE];
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER;

/* fork child: see the lock comment in jit.c */
static pthread_mutex_t guest_sig_lock = PTHREAD_MUTEX_INITIALIZER;   /* guest_sigs[] (below) */
void hao_abi_fork_child(void)
{
    sig_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    guest_sig_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

static uint32_t strhash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

/* Every objc_msgSend to a native method asks, so a cached encoding is
 * found without the lock (enc is published last, with release); only a
 * new one takes sig_lock, inside a hao critical section. */
hsig *hao_sig(const char *enc)
{
    if (!enc) return NULL;
    uint32_t h = strhash(enc) & (SIGCACHE - 1);
    /* Nodes, copied encodings and parsed signatures live for the process.
     * Acquire publication permits lock-free hits during block preparation. */
    signode *n = __atomic_load_n(&sigcache[h], __ATOMIC_ACQUIRE);
    for (signode *hit = n; hit; hit = hit->next)
        if (!strcmp(hit->enc, enc)) return hit->sig;
    hao_crit_enter();
    pthread_mutex_lock(&sig_lock);
    n = __atomic_load_n(&sigcache[h], __ATOMIC_RELAXED);
    while (n && strcmp(n->enc, enc)) n = n->next;
    if (!n) {
        n = malloc(sizeof *n);
        char *copy = strdup(enc);
        if (!n || !copy) { fprintf(stderr, "hao: signature cache: out of memory\n"); abort(); }
        n->enc = copy;
        n->sig = parse_sig(enc);
        n->next = __atomic_load_n(&sigcache[h], __ATOMIC_RELAXED);
        __atomic_store_n(&sigcache[h], n, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&sig_lock);
    hao_crit_leave();
    return n->sig;
}

/* Interned parsed signatures and their copied encoding strings live for the
 * process lifetime. A descriptor address itself is not a signature identity. */
const void *hao_sig_identity(const char *enc)
{
    return hao_opt.no_closures ? NULL : hao_sig(enc);
}

bool hao_sig_simple(const char *enc)
{
    hsig *s = hao_sig(enc);
    return !s || s->simple;
}

/* A variadic method's encoding lists only its fixed arguments: objc.c compares these counts with the
 * native IMP's va_start prologue before it bridges the call as variadic. */
bool hao_sig_scalar_args(const char *enc, int *ngp, int *nfp)
{
    hsig *s = hao_sig(enc);
    if (!s || s->unmarshalable || s->ambiguous) return false;
    if (!strchr("vifd", s->ret.kind) || s->ret.size > 8) return false;
    int g = 0, f = 0;
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        if (a->kind == 'i' && a->size <= 8) g++;
        else if (a->kind == 'f' || a->kind == 'd') f++;
        else return false;
    }
    *ngp = g;
    *nfp = f;
    return true;
}

/* An ObjC method encoding's facts for objc_msgSend to a native IMP (objc.c trap_msgsend): the result
 * is whether the register bridge is exact (hao_sig_simple); *codeslots the arm64 argument slots that
 * hold code, *fnret whether the result is a function pointer. */
bool hao_sig_msgsend(const char *enc, unsigned *codeslots, bool *fnret)
{
    hsig *s = hao_sig(enc);
    *codeslots = s ? s->codeslots : 0;
    *fnret = s && s->fnret;
    return !s || s->simple;
}

/* ------------------------------------------------- arm64 argument layout */

typedef struct { int ngrn, nsrn; uint64_t nsaa; } aapcs_state;

/* bytes arm64_get_arg copies for a value of type t (0: passed by reference, not copied) */
#define ARGBUF 32
static int arg_copy_size(const htype *t)
{
    if (t->kind == 'D') return (int)sizeof(long double);
    if (t->kind == 's' && !t->hfa && t->size > 16) return 0;
    return t->size;
}

/* AAPCS64: an aggregate result is returned through x8 unless it is an HFA
 * or fits in x0/x1 */
static bool arm64_indirect_ret(const htype *r) { return r->kind == 's' && !r->hfa && r->size > 16; }

/* Locate argument `t` per Apple AAPCS64 and return its bytes: copied into
 * `dst` (at most ARGBUF bytes), or for an aggregate passed by reference,
 * the caller's copy itself.
 * guest: cpu (registers) + guest stack pointer at call time. */
static const uint8_t *arm64_get_arg(hao_cpu *c, aapcs_state *st, const htype *t, uint8_t dst[ARGBUF])
{
    const uint8_t *sp = (const uint8_t *)(uintptr_t)c->x[31];
    if (t->kind == 'f' || t->kind == 'd' || t->kind == 'D') {
        int sz = t->kind == 'f' ? 4 : 8;
        if (st->nsrn < 8) { memcpy(dst, &c->v[st->nsrn++], (size_t)sz); }
        else { st->nsaa = (st->nsaa + (uint64_t)sz - 1) & ~((uint64_t)sz - 1); memcpy(dst, sp + st->nsaa, (size_t)sz); st->nsaa += (uint64_t)sz; }
        if (t->kind == 'D') { double d; memcpy(&d, dst, 8); long double ld = d; memcpy(dst, &ld, sizeof ld); }
        return dst;
    }
    if (t->kind == 's' && t->hfa) {
        int esz = t->hfa_kind == 'f' ? 4 : 8;
        if (st->nsrn + t->hfa <= 8) {
            for (int k = 0; k < t->hfa; k++) memcpy(dst + k * esz, &c->v[st->nsrn++], (size_t)esz);
        } else {
            st->nsrn = 8;
            st->nsaa = (st->nsaa + (uint64_t)t->align - 1) & ~((uint64_t)t->align - 1);
            memcpy(dst, sp + st->nsaa, (size_t)t->size);
            st->nsaa += ((uint64_t)t->size + 7) & ~7ull;
        }
        return dst;
    }
    if (t->kind == 's' && t->size > 16) {               /* passed by reference */
        uint64_t ptr;
        if (st->ngrn < 8) ptr = c->x[st->ngrn++];
        else { st->nsaa = (st->nsaa + 7) & ~7ull; memcpy(&ptr, sp + st->nsaa, 8); st->nsaa += 8; }
        return (const uint8_t *)(uintptr_t)ptr;
    }
    int nregs = (t->size + 7) / 8;
    if (t->kind == 's' && t->align == 16 && (st->ngrn & 1)) st->ngrn++;
    if (st->ngrn + nregs <= 8) {
        uint64_t tmp[2] = {c->x[st->ngrn], st->ngrn + 1 < 8 ? c->x[st->ngrn + 1] : 0};
        memcpy(dst, tmp, (size_t)t->size);
        st->ngrn += nregs;
    } else {
        st->ngrn = 8;
        uint64_t al = t->kind == 's' ? 8 : (uint64_t)t->size;     /* Apple: scalars at natural alignment */
        st->nsaa = (st->nsaa + al - 1) & ~(al - 1);
        memcpy(dst, sp + st->nsaa, (size_t)t->size);
        st->nsaa += t->kind == 's' ? (((uint64_t)t->size + 7) & ~7ull) : (uint64_t)t->size;
    }
    return dst;
}

/* The arm64 argument slots (hao/bridge.h argmask bits: 0-7 x0-x7, 8-15 the 8-byte stack slots at the
 * call's sp) that hold an argument's code slots (htype.code: '^?' function pointers, '@?' blocks, and
 * those fields of a by-value aggregate passed in registers or on the stack), placed as arm64_get_arg
 * places them. An aggregate passed by reference has none here: hao_call_native_sig thunks its copy. */
static uint16_t arm64_code_slots(const hsig *s)
{
    aapcs_state st = {0, 0, 0};
    unsigned r = 0;
    for (int k = 0; k < s->nargs; k++) {
        const htype *t = &s->args[k];
        if (t->kind == 'f' || t->kind == 'd' || t->kind == 'D') {
            uint64_t sz = t->kind == 'f' ? 4 : 8;
            if (st.nsrn < 8) st.nsrn++;
            else st.nsaa = ((st.nsaa + sz - 1) & ~(sz - 1)) + sz;
            continue;
        }
        if (t->kind == 's' && t->hfa) {
            if (st.nsrn + t->hfa <= 8) st.nsrn += t->hfa;
            else {
                st.nsrn = 8;
                st.nsaa = ((st.nsaa + (uint64_t)t->align - 1) & ~((uint64_t)t->align - 1)) + (((uint64_t)t->size + 7) & ~7ull);
            }
            continue;
        }
        if (t->kind == 's' && t->size > 16) {           /* by reference */
            if (st.ngrn < 8) st.ngrn++;
            else st.nsaa = ((st.nsaa + 7) & ~7ull) + 8;
            continue;
        }
        int nregs = (t->size + 7) / 8;
        if (t->kind == 's' && t->align == 16 && (st.ngrn & 1)) st.ngrn++;
        if (st.ngrn + nregs <= 8) {
            for (int e = 0; e < nregs; e++) if (t->code >> e & 1) r |= 1u << (st.ngrn + e);
            st.ngrn += nregs;
        } else {
            st.ngrn = 8;
            uint64_t al = t->kind == 's' ? 8 : (uint64_t)t->size;
            st.nsaa = (st.nsaa + al - 1) & ~(al - 1);
            for (int e = 0; e < nregs; e++)
                if ((t->code >> e & 1) && !(st.nsaa & 7) && st.nsaa / 8 + (uint64_t)e < 8) r |= 1u << (8 + st.nsaa / 8 + e);
            st.nsaa += t->kind == 's' ? (((uint64_t)t->size + 7) & ~7ull) : (uint64_t)t->size;
        }
    }
    return (uint16_t)r;
}

/* Place a native return value into guest registers. An indirect (x8)
 * result is not handled here: the native callee wrote it in place. */
static void arm64_set_ret(hao_cpu *c, const htype *r, const uint8_t *val)
{
    switch (r->kind) {
    case 'v': return;
    case 'i': { uint64_t v = 0; memcpy(&v, val, (size_t)r->size); if (r->size < 8 && r->sgn) v = (uint64_t)((int64_t)(v << (64 - 8 * r->size)) >> (64 - 8 * r->size)); c->x[0] = v; return; }
    case 'f': memset(&c->v[0], 0, 16); memcpy(&c->v[0], val, 4); return;
    case 'd': memset(&c->v[0], 0, 16); memcpy(&c->v[0], val, 8); return;
    case 'D': { long double ld; memcpy(&ld, val, sizeof ld); double d = (double)ld; memset(&c->v[0], 0, 16); memcpy(&c->v[0], &d, 8); return; }
    default: break;
    }
    if (r->hfa) {
        int esz = r->hfa_kind == 'f' ? 4 : 8;
        for (int k = 0; k < r->hfa; k++) { memset(&c->v[k], 0, 16); memcpy(&c->v[k], val + k * esz, (size_t)esz); }
    } else if (r->size <= 16) {
        uint64_t tmp[2] = {0, 0};
        memcpy(tmp, val, (size_t)r->size);
        c->x[0] = tmp[0]; c->x[1] = tmp[1];
    }
}

static int sysv_class_mem(const htype *t);
void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack, uint64_t out[6]);

/* calls whose signature abi.c does not marshal (they go through the register bridge): counted for
 * hao_bridge_stats, never silent */
uint64_t hao_abi_refused;
static void sig_refused(const char *enc, const char *why)
{
    __atomic_fetch_add(&hao_abi_refused, 1, __ATOMIC_RELAXED);
    if (hao_verbose) fprintf(stderr, "hao: signature %s not marshalled: %s\n", enc, why);
}

/* Guest functions and blocks among the code slots of an argument (htype.code) bound for native code
 * get native entry points, as the register scan gives them to x0-x7 (hao_thunk_native_args_mask): a
 * by-value aggregate's function pointers (mds's SIFileOps to SIOpenIndex) would otherwise reach the
 * native callee as guest addresses, and every call through them would take the fault path. `p` is
 * hao's own copy of the value, never guest memory. */
/* blocks.c defines it; this weak default lets tests/abi/test_layout.m, which includes abi.c on its own
 * for the parser, link without the runtime */
__attribute__((weak)) uint64_t hao_bridge_code_arg(uint64_t a, const char *enc) { (void)enc; return a; }
static void code_slots(uint8_t *p, int size, uint64_t code)
{
    for (; code; code &= code - 1) {
        int k = __builtin_ctzll(code);
        if (8 * k + 8 > size) break;
        uint64_t v, nv;
        memcpy(&v, p + 8 * k, 8);
        if (v && (nv = hao_bridge_code_arg(v, NULL)) != v) memcpy(p + 8 * k, &nv, 8);
    }
}

#define NSTACK 256      /* SysV stack argument slots of one signature-driven call */
#define LDBUF 4096      /* native-layout copies of long double aggregates in one call */
#define RETBUF 32       /* a result returned in registers: at most an HFA of four doubles */

/* Guest -> native call driven by an ObjC type encoding.
 * `nself`: number of leading implicit args already in the encoding
 * (ObjC encodings include self and _cmd). */
bool hao_call_native_sig(hao_cpu *c, uint64_t fn, const char *enc)
{
    hsig *s = hao_sig(enc);
    if (!s) return false;
    if (s->unmarshalable) { sig_refused(enc, "too many arguments or too large an aggregate"); return false; }
    uint8_t store[MAXARGS][ARGBUF] __attribute__((aligned(16)));
    uint8_t ldbuf[LDBUF] __attribute__((aligned(16)));   /* native layouts of long double aggregates */
    int ldo = 0;
    const uint8_t *av[MAXARGS];
    aapcs_state st = {0, 0, 0};
    const htype *r = &s->ret;
    uint8_t *ldret = NULL;                              /* a long double aggregate's native result */
    if (r->ld && !r->nst0) {
        if (r->nsize > LDBUF) { sig_refused(enc, "result too large"); return false; }
        ldret = ldbuf; ldo = (r->nsize + 15) & ~15;
    }
    for (int k = 0; k < s->nargs; k++) {
        if (arg_copy_size(&s->args[k]) > ARGBUF) { sig_refused(enc, "argument too large"); return false; }
        av[k] = arm64_get_arg(c, &st, &s->args[k], store[k]);
        if (s->args[k].code && av[k] == store[k] && !s->args[k].ld)
            code_slots(store[k], arg_copy_size(&s->args[k]), s->args[k].code);   /* a by-reference one: below */
        if (s->args[k].ld) {
            const htype *a = &s->args[k];
            if (ldo + a->nsize > LDBUF) { sig_refused(enc, "arguments too large"); return false; }
            ld_to_native(a, av[k], ldbuf + ldo);
            av[k] = ldbuf + ldo;
            ldo += (a->nsize + 15) & ~15;
        }
    }
    /* A struct result returned through memory by both ABIs is written by
     * the callee straight into the guest's x8 buffer: hao copies nothing, so
     * no layout mistake can overrun that buffer. (Not a long double
     * aggregate: its layouts differ, it comes back in ldret.) */
    bool mem_ret = r->kind == 's' && (r->ld ? !r->nst0 : sysv_class_mem(r));
    bool in_place = mem_ret && !r->ld && arm64_indirect_ret(r);
    uint8_t ret[RETBUF] __attribute__((aligned(16)));
    if (!in_place && !r->ld && (r->kind == 'D' ? (int)sizeof(long double) : r->size) > RETBUF) { sig_refused(enc, "result too large"); return false; }
    memset(ret, 0, sizeof ret);
    /* x86_64 SysV layout */
    uint64_t gpr[6] = {0}, stack[NSTACK], out[6] = {0};
    uint8_t xmm[8][16];
    memset(xmm, 0, sizeof xmm);
    int gi = 0, xi = 0, so = 0;
    if (mem_ret) gpr[gi++] = in_place ? c->x[8] : ldret ? (uint64_t)(uintptr_t)ldret : (uint64_t)(uintptr_t)ret;
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        const uint8_t *v = av[k];
        if (a->ld) {                                    /* X87 class: memory, 16-aligned */
            so = (so + 1) & ~1;
            int n = (a->nsize + 7) / 8;
            if (so + n > NSTACK) goto too_many;
            memcpy(&stack[so], v, (size_t)a->nsize);
            so += n;
        } else if (a->kind == 'i') {
            uint64_t x = 0; memcpy(&x, v, (size_t)a->size);
            if (a->sgn && a->size < 8) x = (uint64_t)((int64_t)(x << (64 - 8 * a->size)) >> (64 - 8 * a->size));
            if (gi < 6) gpr[gi++] = x;
            else if (so < NSTACK) stack[so++] = x;
            else goto too_many;
        } else if (a->kind == 'f' || a->kind == 'd') {
            if (xi < 8) memcpy(xmm[xi++], v, (size_t)a->size);
            else if (so < NSTACK) { stack[so] = 0; memcpy(&stack[so++], v, (size_t)a->size); }
            else goto too_many;
        } else if (sysv_class_mem(a)) {
            if (a->kind == 'D') so = (so + 1) & ~1;
            int n = a->kind == 'D' ? 2 : (a->size + 7) / 8;
            if (so + n > NSTACK) goto too_many;
            memset(&stack[so], 0, (size_t)n * 8);
            memcpy(&stack[so], v, (size_t)(a->kind == 'D' ? 16 : a->size));
            if (a->code && v != store[k]) code_slots((uint8_t *)&stack[so], a->size, a->code);
            so += n;
        } else {
            int need_i = (a->ebc[0] == EB_INT) + (a->size > 8 && a->ebc[1] == EB_INT);
            int need_x = (a->ebc[0] == EB_SSE) + (a->size > 8 && a->ebc[1] == EB_SSE);
            if (gi + need_i <= 6 && xi + need_x <= 8) {
                for (int e = 0; e < (a->size + 7) / 8; e++) {
                    uint64_t x = 0;
                    memcpy(&x, v + 8 * e, (size_t)((a->size - 8 * e) < 8 ? (a->size - 8 * e) : 8));
                    if (a->ebc[e] == EB_SSE) memcpy(xmm[xi++], &x, 8); else gpr[gi++] = x;
                }
            } else {
                int n = (a->size + 7) / 8;
                if (so + n > NSTACK) goto too_many;
                memset(&stack[so], 0, (size_t)n * 8);
                memcpy(&stack[so], v, (size_t)a->size);
                so += n;
            }
        }
    }
    out[4] = r->kind == 'D' || r->nst0;         /* long double comes back in x87 st0 */
    hao_sysv_call(fn, gpr, (const uint8_t (*)[16])xmm, stack, (uint64_t)so, out);
    if (in_place) return true;
    if (r->ld) {                                /* native layout -> the guest's: x8 buffer or registers */
        uint8_t *g = arm64_indirect_ret(r) ? (uint8_t *)(uintptr_t)c->x[8] : ret;
        ld_to_guest(r, r->nst0 ? (const uint8_t *)&out[4] : ldret, g);
        if (g == ret) arm64_set_ret(c, r, ret);
        return true;
    }
    /* native return -> bytes */
    if (r->kind == 'i') memcpy(ret, &out[0], 8);
    else if (r->kind == 'f' || r->kind == 'd') memcpy(ret, &out[2], 8);
    else if (r->kind == 'D') memcpy(ret, &out[4], sizeof(long double));
    else if (r->kind == 's' && !mem_ret) {
        int ri = 0, rx = 0;
        for (int e = 0; e < (r->size + 7) / 8 && e < 2; e++) {
            if (r->ebc[e] == EB_SSE) memcpy(ret + 8 * e, &out[2 + rx++], 8);
            else memcpy(ret + 8 * e, &out[ri++], 8);
        }
    }
    arm64_set_ret(c, &s->ret, ret);
    return true;
too_many:
    sig_refused(enc, "too many stack arguments");
    return false;
}

/* ------------------------------------------------- native -> guest closure */

/* x86_64 SysV: is t passed or returned in memory? */
static int sysv_class_mem(const htype *t)
{
    if (t->kind == 's' && t->size <= 16 && t->size > 0 && t->ebc[0] == EB_NONE) return 0;
    return t->kind == 'D' || (t->kind == 's' && (t->size > 16 || t->ebc[0] == EB_NONE));
}

/* Run guest function `guest` with native-form argument values; write a
 * result returned in registers to `ret` (RETBUF bytes). A result the guest
 * returns through x8 is written by the guest straight into the native
 * caller's buffer `sret`. */
static void run_guest_with_args(uint64_t guest, hsig *s, const uint8_t *const *args, uint8_t *ret, uint64_t sret)
{
    hao_thread *t = hao_current();
    hao_cpu saved = t->cpu;
    hao_cpu *c = &t->cpu;
    c->host_scratch[HAO_SCRATCH_IN_GUEST] = c->host_scratch[HAO_SCRATCH_HELPER] = 0;
    /* Guest stack, below a 512-byte gap under the suspended guest frame:
     * stack arguments at sp, then the caller-made copies of aggregates
     * passed by reference, each as large as any layout of its type. */
    uint64_t nstack = 0, ncopy = 0;
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        nstack += 16 + (((uint64_t)(a->kind == 'D' ? 8 : a->size) + 7) & ~7ull);
        if (a->kind == 's' && a->size > 16 && !a->hfa) ncopy += ((uint64_t)a->size_max + 15) & ~15ull;
    }
    uint64_t sp = (saved.x[31] - 512 - nstack - ncopy) & ~15ull;
    uint8_t *stack = (uint8_t *)(uintptr_t)sp;
    uint8_t *copy = stack + ((nstack + 15) & ~15ull);
    aapcs_state st = {0, 0, 0};
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        const uint8_t *v = args[k];
        double dtmp;
        if (a->kind == 'D') { long double ld; memcpy(&ld, v, sizeof ld); dtmp = (double)ld; v = (const uint8_t *)&dtmp; }
        if (a->kind == 'f' || a->kind == 'd' || a->kind == 'D') {
            int sz = a->kind == 'f' ? 4 : 8;
            if (st.nsrn < 8) { memset(&c->v[st.nsrn], 0, 16); memcpy(&c->v[st.nsrn++], v, (size_t)sz); }
            else { st.nsaa = (st.nsaa + (uint64_t)sz - 1) & ~((uint64_t)sz - 1); memcpy(stack + st.nsaa, v, (size_t)sz); st.nsaa += (uint64_t)sz; }
        } else if (a->kind == 's' && a->hfa && st.nsrn + a->hfa <= 8) {
            int esz = a->hfa_kind == 'f' ? 4 : 8;
            for (int j = 0; j < a->hfa; j++) { memset(&c->v[st.nsrn], 0, 16); memcpy(&c->v[st.nsrn++], v + j * esz, (size_t)esz); }
        } else if (a->kind == 's' && a->size > 16 && !a->hfa) {
            memset(copy, 0, (size_t)a->size_max);
            memcpy(copy, v, (size_t)a->size);
            uint64_t ptr = (uint64_t)(uintptr_t)copy;
            copy += ((size_t)a->size_max + 15) & ~(size_t)15;
            if (st.ngrn < 8) c->x[st.ngrn++] = ptr;
            else { st.nsaa = (st.nsaa + 7) & ~7ull; memcpy(stack + st.nsaa, &ptr, 8); st.nsaa += 8; }
        } else if (a->kind == 's' && a->hfa) {
            /* an HFA that does not fit in v0-v7 goes to the stack and exhausts only the vector
             * registers (AAPCS64 C.3, as arm64_get_arg does); later integers still use x registers */
            st.nsrn = 8;
            st.nsaa = (st.nsaa + (uint64_t)a->align - 1) & ~((uint64_t)a->align - 1);
            memcpy(stack + st.nsaa, v, (size_t)a->size);
            st.nsaa += ((uint64_t)a->size + 7) & ~7ull;
        } else {
            int nregs = (a->size + 7) / 8;
            if (st.ngrn + nregs <= 8) {
                uint64_t tmp[2] = {0, 0};
                memcpy(tmp, v, (size_t)a->size);
                if (a->kind == 'i' && a->size < 8 && a->sgn)
                    tmp[0] = (uint64_t)((int64_t)(tmp[0] << (64 - 8 * a->size)) >> (64 - 8 * a->size));
                for (int j = 0; j < nregs; j++) c->x[st.ngrn++] = tmp[j];
            } else {
                st.ngrn = 8;
                uint64_t al = a->kind == 's' ? 8 : (uint64_t)a->size;
                st.nsaa = (st.nsaa + al - 1) & ~(al - 1);
                memcpy(stack + st.nsaa, v, (size_t)a->size);
                st.nsaa += a->kind == 's' ? (((uint64_t)a->size + 7) & ~7ull) : (uint64_t)a->size;
            }
        }
    }
    const htype *r = &s->ret;
    if (arm64_indirect_ret(r)) c->x[8] = sret;
    c->x[31] = sp;
    c->x[30] = HAO_RETURN_MAGIC;
    c->pc = guest;
    t->callbacks++;
    unsigned objc_enum_result = hao_objc_enum_args(c, guest, true);
    hao_run(t, &saved);
    hao_objc_enum_result(c, objc_enum_result);
    /* collect a register result in native form */
    if (r->kind == 'i') memcpy(ret, &c->x[0], 8);
    else if (r->kind == 'f') memcpy(ret, &c->v[0], 4);
    else if (r->kind == 'd') memcpy(ret, &c->v[0], 8);
    else if (r->kind == 'D') { double d; memcpy(&d, &c->v[0], 8); long double ld = d; memcpy(ret, &ld, sizeof ld); }
    else if (r->kind == 's' && !arm64_indirect_ret(r)) {
        if (r->hfa) { int esz = r->hfa_kind == 'f' ? 4 : 8; for (int j = 0; j < r->hfa; j++) memcpy(ret + j * esz, &c->v[j], (size_t)esz); }
        else { uint64_t tmp[2] = {c->x[0], c->x[1]}; memcpy(ret, tmp, (size_t)r->size); }
    }
    t->cpu = saved;
}

/* x86_64 SysV: decode an incoming native call described by `s` from the
 * callback frame. A value passed in registers is copied to `store`; one
 * passed in memory is used where it lies, in the caller's frame. */
static void sysv_decode(hsig *s, hao_cb_frame *f, uint8_t store[][16], const uint8_t **vals, uint64_t *sret)
{
    int gi = 0, xi = 0, so = 0;
    *sret = 0;
    if (s->ret.kind == 's' && (s->ret.ld ? !s->ret.nst0 : sysv_class_mem(&s->ret))) *sret = f->gpr[gi++];
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        uint8_t *d = store[k];
        vals[k] = d;
        memset(d, 0, 16);
        if (a->ld) {                                          /* native layout, in memory, 16-aligned */
            so = (so + 1) & ~1;
            vals[k] = (const uint8_t *)&f->stack_args[so];
            so += (a->nsize + 7) / 8;
        } else if (a->kind == 'i') {
            if (gi < 6) memcpy(d, &f->gpr[gi++], 8);
            else memcpy(d, &f->stack_args[so++], 8);
        } else if (a->kind == 'f' || a->kind == 'd') {
            if (xi < 8) memcpy(d, f->xmm[xi++], 8);
            else memcpy(d, &f->stack_args[so++], 8);
        } else if (sysv_class_mem(a)) {
            if (a->kind == 'D') so = (so + 1) & ~1;
            vals[k] = (const uint8_t *)&f->stack_args[so];
            so += (a->kind == 'D' ? 16 : a->size + 7) / 8;
        } else {                                              /* at most 16 bytes */
            int need_i = (a->ebc[0] == EB_INT) + (a->size > 8 && a->ebc[1] == EB_INT);
            int need_x = (a->ebc[0] == EB_SSE) + (a->size > 8 && a->ebc[1] == EB_SSE);
            if (gi + need_i <= 6 && xi + need_x <= 8) {
                for (int e = 0; e < (a->size + 7) / 8 && e < 2; e++) {
                    if (a->ebc[e] == EB_SSE) memcpy(d + 8 * e, f->xmm[xi++], 8);
                    else memcpy(d + 8 * e, &f->gpr[gi++], 8);
                }
            } else {
                vals[k] = (const uint8_t *)&f->stack_args[so];
                so += (a->size + 7) / 8;
            }
        }
    }
}

static void sysv_encode_ret(hsig *s, hao_cb_frame *f, const uint8_t *ret, uint64_t sret)
{
    const htype *r = &s->ret;
    if (r->kind == 'v') return;
    if (r->ld) {                                  /* ret: the guest layout, from registers or its x8 buffer */
        if (r->nst0) { memset(f->xmm[0], 0, 16); ld_to_native(r, ret, f->xmm[0]); f->sig = 1; return; }
        ld_to_native(r, ret, (uint8_t *)(uintptr_t)sret);
        f->gpr[0] = sret;
        return;
    }
    if (r->kind == 'i') { memcpy(&f->gpr[0], ret, 8); return; }
    if (r->kind == 'f' || r->kind == 'd') { memset(f->xmm[0], 0, 16); memcpy(f->xmm[0], ret, (size_t)r->size); return; }
    if (sysv_class_mem(r)) {
        if (r->kind == 'D') { memcpy(f->xmm[0], ret, 16); f->sig = 1; return; }   /* the entry loads st0 */
        /* an HFA (exactly hfa elements, no padding) returned in v0-v3;
         * any other result the guest already wrote to sret through x8 */
        if (!arm64_indirect_ret(r)) memcpy((void *)(uintptr_t)sret, ret, (size_t)(r->hfa * (r->hfa_kind == 'f' ? 4 : 8)));
        f->gpr[0] = sret;
        return;
    }
    int gi = 0, xi = 0;
    for (int e = 0; e < (r->size + 7) / 8 && e < 2; e++) {
        if (r->ebc[e] == EB_SSE) { memset(f->xmm[xi], 0, 16); memcpy(f->xmm[xi++], ret + 8 * e, 8); }
        else memcpy(&f->gpr[gi++], ret + 8 * e, 8);
    }
}

void hao_note_native_block(uint64_t block);   /* blocks.c */

/* called from hao_callback when the entry thunk carried a signature */
void hao_callback_with_sig(hao_cb_frame *f, void *sigp)
{
    hsig *s = sigp;
    uint8_t store[MAXARGS][16] __attribute__((aligned(16)));
    const uint8_t *vals[MAXARGS];
    uint64_t sret;
    sysv_decode(s, f, store, vals, &sret);
    /* native blocks handed to the guest: learn their result types, so that
     * guest calls to them get narrow results extended (hao_run) */
    for (int k = 0; k < s->nargs; k++)
        if (s->args[k].block) { uint64_t b; memcpy(&b, vals[k], 8); if (b) hao_note_native_block(b); }
    /* long double aggregates: the guest gets its own layout (args), and returns into one (an x8 result) */
    uint8_t ldbuf[LDBUF] __attribute__((aligned(16)));
    int ldo = 0;
    for (int k = 0; k < s->nargs; k++) {
        const htype *a = &s->args[k];
        if (!a->ld) continue;
        if (ldo + a->size_max > LDBUF) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "long double aggregates too large for a callback\n"); abort(); }
        memset(ldbuf + ldo, 0, (size_t)a->size_max);
        ld_to_guest(a, vals[k], ldbuf + ldo);
        vals[k] = ldbuf + ldo;
        ldo += (a->size_max + 15) & ~15;
    }
    uint8_t ret[RETBUF] __attribute__((aligned(16)));
    memset(ret, 0, sizeof ret);
    const htype *r = &s->ret;
    uint8_t *gret = ret;
    if (r->ld && arm64_indirect_ret(r)) {
        if (ldo + r->size_max > LDBUF) { hao_diag_prefix(HAO_DIAG); fprintf(HAO_DIAG, "long double result too large for a callback\n"); abort(); }
        gret = ldbuf + ldo;
        memset(gret, 0, (size_t)r->size_max);
    }
    run_guest_with_args(f->target, s, vals, ret, r->ld ? (uint64_t)(uintptr_t)gret : sret);
    sysv_encode_ret(s, f, gret, sret);
}

/* signature thunk -> guest function: read back from the thunk itself (jit.c) */
uint64_t hao_sig_thunk_target(uint64_t a);
uint64_t hao_closure_target(uint64_t code) { return hao_sig_thunk_target(code); }

/* Signatures of guest functions that got no signature thunk: the process
 * has no executable memory (hao_jit_init failed), or the signature-thunk
 * area is full. Native code then reaches the guest address itself (an NX
 * fault) or its plain thunk, both of which arrive in hao_callback without a
 * signature; hao_callback looks the target up here. Keyed by guest address:
 * the first signature noted for a function wins. Readers take no lock. */
static hao_gmap guest_sigs = HAO_GMAP_INIT(2, 1, 8, 15);   /* guest -> hsig; grows with use (hao/gmap.h) */

void *hao_guest_sig(uint64_t guest)
{
    const uint64_t *e = hao_gmap_get1(&guest_sigs, guest);
    return e ? (void *)(uintptr_t)e[1] : NULL;
}

static bool note_guest_sig(uint64_t guest, hsig *s)
{
    bool fresh;
    hao_crit_enter();
    pthread_mutex_lock(&guest_sig_lock);
    uint64_t *e = guest ? hao_gmap_claim1(&guest_sigs, guest, &fresh) : NULL;
    if (e && fresh) { e[1] = (uint64_t)(uintptr_t)s; hao_gmap_publish1(&guest_sigs, e, guest); }
    pthread_mutex_unlock(&guest_sig_lock);
    hao_crit_leave();
    return e != NULL;
}

/* Native entry point for a guest function with signature `enc`: a raw
 * thunk when the register bridge is exact, else a signature thunk whose
 * arguments are decoded per x86_64 SysV and re-laid-out per AAPCS64.
 * hao_guest_of_entry maps either back to the guest function. Without
 * executable memory both are the guest address itself; a signature that
 * needs marshalling is then noted for hao_callback. */
static uint64_t guest_entry_for_sig(uint64_t guest, const char *enc, bool known_void)
{
    hsig *s = hao_opt.no_closures ? NULL : hao_sig(enc);
    if (s && s->unmarshalable) __atomic_fetch_add(&hao_abi_refused, 1, __ATOMIC_RELAXED);
    /* A supported void block retains its declared parameters, including the
     * hidden block argument, and has no result.
     * Do not discard that fact into the generic callback's residual-register
     * and speculative indirect-result scans. Other simple signatures retain
     * their existing register bridge unless the API supplies an explicit
     * supported C void callback contract. */
    bool precise_void = s && s->simple && !s->ambiguous && !s->unmarshalable &&
                        s->ret.kind == 'v' &&
                        (known_void || (s->nargs > 0 && s->args[0].block));
    if (known_void && !precise_void) return hao_thunk_for(guest);
    if (!s || (s->simple && !precise_void) || s->unmarshalable) return hao_thunk_for(guest);
    /* one signature thunk per (guest function, signature), shared by
     * callers that ask again for the same method or block invoke: jit.c
     * keeps them by (guest, sig) */
    uint64_t code = hao_sig_thunk(guest, s);
    if (!code) {
        /* The precise void contract belongs to this typed entry, not to
         * every use of the same guest address. Without an entry retain the
         * old generic behavior instead of installing a target-wide hint. */
        if (precise_void) return hao_thunk_for(guest);
        bool noted = note_guest_sig(guest, s);
        if (hao_verbose > 1)
            fprintf(stderr, "hao: no sig thunk for guest %#llx sig %s (%s)\n", (unsigned long long)guest, enc,
                    noted ? "signature noted for the callback path" : "signature table full");
        return hao_thunk_for(guest);
    }
    if (hao_verbose > 1) fprintf(stderr, "hao: sig thunk %#llx for guest %#llx sig %s\n", (unsigned long long)code, (unsigned long long)guest, enc);
    return code;
}

/* Source-declared C callbacks may retain a register-exact void contract too.
 * Unknown/nonvoid/ambiguous signatures keep the generic callback path. */
uint64_t hao_guest_entry_for_void_sig(uint64_t guest, const char *enc)
{
    return guest_entry_for_sig(guest, enc, true);
}

uint64_t hao_guest_entry_for_sig(uint64_t guest, const char *enc)
{
    return guest_entry_for_sig(guest, enc, false);
}
