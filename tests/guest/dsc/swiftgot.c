/*
 * Swift metadata of an arm64e-only shared-cache library whose relative
 * indirect references go through 8-byte slots of the cache's uniqued GOTs,
 * outside every segment of the image (no code of the image loads them).
 * Native libswiftCore follows those references; under hao the library is a
 * guest image (src/dsc.c), so the slots must hold native targets.
 *
 * Everything is derived at run time from the image's own load commands and
 * sections, found from one exported symbol (ANCHOR, a type descriptor):
 *   count   how many field-type "\x02" symbolic references, conformance
 *           ProtocolRefs / TypeRefs and resilient-witness requirements are
 *           indirect through a slot outside the image (offsets only; no slot
 *           is read)
 *   fields  every non-generic type in __swift5_types: its metadata accessor,
 *           then every stored field's mangled type name resolved by
 *           swift_getTypeByMangledNameInContext with the type as context
 *           (the demangler reads "\x02" slots: sharingd's crash)
 *   conf    every record in __swift5_proto: the protocol and type it names
 *           (read through their slots, each slot value checked with dladdr
 *           before it is used) and swift_conformsToProtocol(type, protocol),
 *           which scans every conformance's ProtocolRef and instantiates the
 *           witness table (resilient-witness requirements)
 *   keypath  every key path pattern in __TEXT,__const (no metadata names one;
 *           the code that calls swift_getKeyPath does) that is not generic and
 *           whose first component is external with no generic arguments and a
 *           property descriptor indirect through a slot outside the image:
 *           the count (must be non-zero), then per pattern swift_getKeyPath
 *           and the key path's class name, then whether the slot holds an
 *           address dladdr knows (the slot is read only after the call: hao
 *           binds key path slots in its swift_getKeyPath trap)
 * Pattern layout (swift KeyPath.swift _swift_getKeyPath, IRGen GenKeyPath.cpp;
 * shims/KeyPath.h): int32 once-variable, generic environment, root and value
 * mangled names, KVC string (relative), then the buffer header (size in its low
 * 24 bits) and the components; a component header has its kind in bits 24-30
 * (external = 0) and, for an external one, the number of generic arguments in
 * its payload; the property descriptor reference follows it.
 * Usage: swiftgot count|fields|conf|keypath
 */
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ANCHOR
#error "ANCHOR: the exported symbol of a type descriptor in the library"
#endif
extern const char anchor[] __asm__(ANCHOR);

typedef struct { const void *md; uintptr_t state; } resp_t;
typedef resp_t (*accessor_t)(uintptr_t);
typedef struct { const char *p; uintptr_t n; } tname_t;
extern tname_t swift_getTypeName(const void *md, _Bool qualified);
extern const void *swift_getTypeByMangledNameInContext(const char *, size_t, const void *, const void *const *);
extern const void *swift_conformsToProtocol(const void *md, const void *proto);
extern const void *swift_getObjCClassMetadata(const void *cls);
extern void *objc_getClass(const char *);
extern void *swift_getKeyPath(const void *pattern, const void *arguments);

static uintptr_t slide;
static struct { uintptr_t lo, hi; } segs[16];
static int nsegs;
static const uint8_t *sect[5];
static size_t sectsz[5];
static const char *const sectname[5] = {"__swift5_types", "__swift5_types2", "__swift5_proto", "__swift5_typeref", 0};
static const uint8_t *tconst;
static size_t tconstsz;

static int in_image(uintptr_t a)
{
    for (int i = 0; i < nsegs; i++)
        if (a >= segs[i].lo && a < segs[i].hi) return 1;
    return 0;
}

static uintptr_t rel_direct(const void *p)
{
    int32_t o;
    memcpy(&o, p, 4);
    return o ? (uintptr_t)p + o : 0;
}

/* a relative indirectable pointer: bit 0 set = the offset names a slot that holds the target */
static uintptr_t rel_slot(const void *p, int *indirect)
{
    int32_t o;
    memcpy(&o, p, 4);
    *indirect = o & 1;
    return o ? (uintptr_t)p + (o & ~1) : 0;
}

/* the slot's value, used only when dladdr knows the address it holds (an unbound slot holds a raw cache word) */
static const void *through(uintptr_t slot)
{
    const void *v = *(const void *const *)slot;
    Dl_info i;
    return v && dladdr(v, &i) ? v : NULL;
}

static uintptr_t rel_indirectable(const void *p)
{
    int ind;
    uintptr_t a = rel_slot(p, &ind);
    return a && ind ? (uintptr_t)through(a) : a;
}

static size_t mangled_len(const uint8_t *n)
{
    size_t i = 0;
    while (n[i]) {
        if (n[i] <= 0x17) i += 5;
        else if (n[i] <= 0x1f) i += 9;
        else i++;
    }
    return i;
}

static int outside_refs(const uint8_t *n)
{
    int k = 0;
    for (size_t i = 0; n[i];) {
        if (n[i] == 0x02 && !in_image(rel_direct(n + i + 1))) k++;
        i += n[i] <= 0x17 ? 5 : n[i] <= 0x1f ? 9 : 1;
    }
    return k;
}

static void ctx_name(uintptr_t d, char *buf, size_t n)
{
    const char *part[8];
    int k = 0;
    while (d && k < 8) {
        unsigned kind = *(const uint32_t *)d & 0x1f;
        /* module, protocol and type contexts are named at +8; an extension's +8 is a mangled name */
        part[k++] = kind == 1 ? "(extension)" : kind == 0 || kind == 3 || kind >= 16 ? (const char *)rel_direct((const void *)(d + 8)) : "(context)";
        if (kind == 0) break;
        d = rel_indirectable((const void *)(d + 4));
    }
    buf[0] = 0;
    while (k-- > 0) {
        strlcat(buf, part[k] ? part[k] : "?", n);
        if (k) strlcat(buf, ".", n);
    }
}

static void print_md(const char *tag, const void *md)
{
    if (!md) { printf("%s NULL\n", tag); return; }
    tname_t t = swift_getTypeName(md, 1);
    /* private and anonymous contexts print as "(unknown context at $<address>)": drop the address (the slide) */
    printf("%s ", tag);
    for (uintptr_t i = 0; i < t.n; i++) {
        static const char at[] = " at $";
        if (i + 5 <= t.n && !memcmp(t.p + i, at, 5)) {
            for (i += 5; i < t.n && strchr("0123456789abcdef", t.p[i]); i++) {}
            i--;
            continue;
        }
        putchar(t.p[i]);
    }
    putchar('\n');
}

static int nongeneric_type(uintptr_t d)
{
    uint32_t f = *(const uint32_t *)d;
    return (f & 0x1f) >= 16 && (f & 0x1f) <= 18 && !(f & 0x80);
}

static const void *type_md(uintptr_t d)
{
    accessor_t a = (accessor_t)rel_direct((const void *)(d + 12));
    return a ? a(0).md : NULL;
}

static int count_c02, count_proto, count_typeref, count_witness;

static void fields_of(uintptr_t d, int run)
{
    const uint8_t *fd = (const uint8_t *)rel_direct((const void *)(d + 16));
    if (!fd) return;
    uint16_t recsz;
    uint32_t nf;
    memcpy(&recsz, fd + 10, 2);
    memcpy(&nf, fd + 12, 4);
    for (uint32_t k = 0; k < nf; k++) {
        const uint8_t *r = fd + 16 + (size_t)k * recsz;
        const uint8_t *mn = (const uint8_t *)rel_direct(r + 4);
        const char *fname = (const char *)rel_direct(r + 8);
        if (!mn) continue;
        if (!run) { count_c02 += outside_refs(mn); continue; }
        printf("  field %s:", fname ? fname : "?");
        fflush(stdout);
        print_md("", swift_getTypeByMangledNameInContext((const char *)mn, mangled_len(mn), (const void *)d, NULL));
        fflush(stdout);
    }
}

static void types(int run)
{
    for (int s = 0; s < 2; s++)
        for (size_t o = 0; o + 4 <= sectsz[s]; o += 4) {
            const uint8_t *e = sect[s] + o;
            int32_t v;
            memcpy(&v, e, 4);
            if ((v & 3) > 1) continue; /* ObjC class records */
            uintptr_t d = (uintptr_t)e + (v & ~3);
            if (v & 1) d = (uintptr_t)through(d);
            if (!d || !nongeneric_type(d)) continue;
            if (run) {
                char nm[512];
                ctx_name(d, nm, sizeof nm);
                printf("type %s\n", nm);
                fflush(stdout);
                print_md("  metadata", type_md(d));
                fflush(stdout);
            }
            fields_of(d, run);
        }
}

static void conformances(int run)
{
    for (size_t o = 0; o + 4 <= sectsz[2]; o += 4) {
        const uint8_t *cd = (const uint8_t *)rel_direct(sect[2] + o);
        uint32_t flags;
        memcpy(&flags, cd + 12, 4);
        unsigned kind = (flags >> 3) & 7;
        int ind;
        uintptr_t ps = rel_slot(cd, &ind);
        if (!run) {
            if (ind && !in_image(ps)) count_proto++;
            uintptr_t ts = rel_slot(cd + 4, &ind);
            if ((kind == 1 || kind == 3) && !in_image(ts)) count_typeref++;
            const uint8_t *p = cd + 16;
            if (flags & (1u << 6)) p += 4;
            p += 12 * ((flags >> 8) & 0xff);
            p += 8 * ((flags >> 24) & 0xff);
            if (flags & (1u << 16)) {
                uint32_t n;
                memcpy(&n, p, 4);
                for (uint32_t k = 0; k < n; k++) {
                    uintptr_t rs = rel_slot(p + 4 + 8 * k, &ind);
                    if (ind && !in_image(rs)) count_witness++;
                }
            }
            continue;
        }
        uintptr_t proto = ind ? (uintptr_t)through(ps) : ps;
        char pn[512] = "unbound", tn[512] = "unbound";
        const void *md = NULL;
        if (proto) ctx_name(proto, pn, sizeof pn);
        uintptr_t t = 0;
        if (kind == 0) t = rel_direct(cd + 4);
        else if (kind == 1) t = (uintptr_t)through(rel_direct(cd + 4));
        if (kind <= 1 && t) {
            ctx_name(t, tn, sizeof tn);
            if (nongeneric_type(t)) md = type_md(t);
            else strlcat(tn, " (generic)", sizeof tn);
        } else if (kind == 2) {
            const char *cn = (const char *)rel_direct(cd + 4);
            snprintf(tn, sizeof tn, "objc %s", cn ? cn : "?");
            void *cls = cn ? objc_getClass(cn) : NULL;
            if (cls) md = swift_getObjCClassMetadata(cls);
        } else if (kind == 3) {
            const void *cls = through(rel_direct(cd + 4));
            snprintf(tn, sizeof tn, "objc %s", cls ? "class" : "unbound");
            if (cls) md = swift_getObjCClassMetadata(cls);
        }
        printf("conf %s : %s", tn, pn);
        fflush(stdout);
        if (md && proto) printf(" = %s\n", swift_conformsToProtocol(md, (const void *)proto) ? "yes" : "no");
        else printf(" = -\n");
        fflush(stdout);
    }
}

static int in_sect(uintptr_t a, int s)
{
    return sect[s] && a >= (uintptr_t)sect[s] && a < (uintptr_t)sect[s] + sectsz[s];
}

static void keypaths(void)
{
    uintptr_t pats[64];
    int n = 0;
    for (size_t o = 0; tconst && o + 32 <= tconstsz && n < 64; o += 4) {
        const uint8_t *p = tconst + o;
        uint32_t w[8];
        memcpy(w, p, sizeof w);
        if (w[1]) continue;                                          /* generic environment: not generic */
        if (w[0] && !in_image(rel_direct(p))) continue;              /* once variable */
        if (!w[2] || !w[3] || !in_sect(rel_direct(p + 8), 3) || !in_sect(rel_direct(p + 12), 3)) continue;
        if (w[4] && !in_image(rel_direct(p + 16))) continue;         /* KVC string */
        uint32_t size = w[5] & 0x00ffffffu;
        if (size < 8 || size > 0x400 || (size & 3) || (w[5] & 0x0f000000u)) continue;
        if (w[6] != 0) continue;                                     /* external, no generic arguments */
        int ind;
        uintptr_t slot = rel_slot(p + 28, &ind);
        if (!ind || !slot || (slot & 7) || in_image(slot)) continue;
        pats[n++] = (uintptr_t)p;
    }
    printf("key path patterns with an external component through an outside-image slot: %d\n", n);
    fflush(stdout);
    for (int k = 0; k < n; k++) {
        const uint8_t *p = (const uint8_t *)pats[k];
        int ind;
        uintptr_t slot = rel_slot(p + 28, &ind);
        printf("pattern %d:", k);
        fflush(stdout);
        void *kp = swift_getKeyPath(p, NULL);
        print_md("", kp ? *(const void *const *)kp : NULL);
        printf("  descriptor slot %s\n", through(slot) ? "bound" : "unbound");
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "count";
    Dl_info info;
    if (!dladdr(anchor, &info) || !info.dli_fbase) { printf("dladdr failed\n"); return 2; }
    const struct mach_header_64 *mh = info.dli_fbase;
    if (mh->magic != MH_MAGIC_64) { printf("bad header\n"); return 2; }
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (sg->cmd != LC_SEGMENT_64) continue;
        if (!strcmp(sg->segname, "__TEXT")) slide = (uintptr_t)mh - sg->vmaddr;
    }
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (sg->cmd != LC_SEGMENT_64 || !strcmp(sg->segname, "__LINKEDIT") || !sg->vmsize) continue;
        if (nsegs < 16) { segs[nsegs].lo = sg->vmaddr + slide; segs[nsegs++].hi = sg->vmaddr + slide + sg->vmsize; }
        const struct section_64 *sc = (const void *)(sg + 1);
        for (uint32_t k = 0; k < sg->nsects; k++) {
            if (!strcmp(sg->segname, "__TEXT") && !strncmp(sc[k].sectname, "__const", 16)) {
                tconst = (const uint8_t *)(sc[k].addr + slide);
                tconstsz = sc[k].size;
            }
            for (int s = 0; sectname[s]; s++)
                if (!strncmp(sc[k].sectname, sectname[s], 16)) {
                    sect[s] = (const uint8_t *)(sc[k].addr + slide);
                    sectsz[s] = sc[k].size;
                }
        }
    }
    printf("image %s\n", info.dli_fname);
    if (!strcmp(what, "count")) {
        types(0);
        conformances(0);
        printf("outside-image slots: field-type \\x02 %d, ProtocolRef %d, TypeRef %d, resilient witness %d\n", count_c02,
               count_proto, count_typeref, count_witness);
    } else if (!strcmp(what, "fields")) types(1);
    else if (!strcmp(what, "conf")) conformances(1);
    else if (!strcmp(what, "keypath")) keypaths();
    else { printf("usage: swiftgot count|fields|conf|keypath\n"); return 2; }
    return 0;
}
