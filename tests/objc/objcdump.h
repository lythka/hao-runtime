/*
 * Deterministic dump of Objective-C runtime state through the public runtime
 * API: classes (superclass, instance size, ivars, properties, protocols,
 * methods), protocols and method lookups. Shared by the guest test program
 * (objcmeta.m, built for arm64 and arm64e) and the x86_64 host driver
 * (objcreg.c), so their outputs compare byte for byte. Lists are sorted and
 * deduplicated: list order and in-place category replacement are runtime
 * details, not results.
 */
#ifndef OBJCDUMP_H
#define OBJCDUMP_H
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int od_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* print n heap strings sorted and unique with a prefix, then free them */
static void od_emit(const char *pfx, char **v, unsigned n)
{
    qsort(v, n, sizeof *v, od_cmp);
    for (unsigned k = 0; k < n; k++) {
        if (!k || strcmp(v[k], v[k - 1])) printf("  %s %s\n", pfx, v[k]);
    }
    for (unsigned k = 0; k < n; k++) free(v[k]);
    free(v);
}

static char *od_fmt2(const char *a, const char *b)
{
    size_t la = strlen(a), lb = b ? strlen(b) : 0;
    char *s = malloc(la + lb + 2);
    memcpy(s, a, la);
    s[la] = 0;
    if (b) {
        s[la] = ' ';
        memcpy(s + la + 1, b, lb);
        s[la + 1 + lb] = 0;
    }
    return s;
}

static void od_methods(Class k, const char *pfx)
{
    unsigned n = 0;
    Method *m = class_copyMethodList(k, &n);
    char **v = malloc((n + 1) * sizeof *v);
    for (unsigned j = 0; j < n; j++) v[j] = od_fmt2(sel_getName(method_getName(m[j])), method_getTypeEncoding(m[j]));
    free(m);
    od_emit(pfx, v, n);
}

static void od_properties(Class k, const char *pfx)
{
    unsigned n = 0;
    objc_property_t *p = class_copyPropertyList(k, &n);
    char **v = malloc((n + 1) * sizeof *v);
    for (unsigned j = 0; j < n; j++) v[j] = od_fmt2(property_getName(p[j]), property_getAttributes(p[j]));
    free(p);
    od_emit(pfx, v, n);
}

static void od_protocols(Protocol *const *pl, unsigned n, const char *pfx)
{
    char **v = malloc((n + 1) * sizeof *v);
    for (unsigned j = 0; j < n; j++) v[j] = od_fmt2(protocol_getName(pl[j]), NULL);
    od_emit(pfx, v, n);
}

static void od_class(const char *name)
{
    Class c = objc_getClass(name);
    if (!c) { printf("class %s absent\n", name); return; }
    Class s = class_getSuperclass(c), meta = object_getClass((id)c);
    printf("class %s : %s size=%zu meta=%d metasuper=%s\n", name, s ? class_getName(s) : "-", class_getInstanceSize(c),
           class_isMetaClass(meta), class_getSuperclass(meta) ? class_getName(class_getSuperclass(meta)) : "-");
    unsigned n = 0;
    Ivar *iv = class_copyIvarList(c, &n);
    for (unsigned j = 0; j < n; j++)
        printf("  ivar %s %s +%ld\n", ivar_getName(iv[j]), ivar_getTypeEncoding(iv[j]), (long)ivar_getOffset(iv[j]));
    free(iv);
    od_properties(c, "prop");
    od_properties(meta, "classprop");
    Protocol **pl = class_copyProtocolList(c, &n);
    od_protocols(pl, n, "proto");
    free(pl);
    od_methods(c, "-");
    od_methods(meta, "+");
}

/* exported by libobjc (Foundation's NSXPCInterface uses it) */
extern const char *_protocol_getMethodTypeEncoding(Protocol *, SEL, BOOL isRequiredMethod, BOOL isInstanceMethod);

static void od_protocol(const char *name)
{
    Protocol *p = objc_getProtocol(name);
    if (!p) { printf("protocol %s absent\n", name); return; }
    printf("protocol %s\n", name);
    unsigned n = 0;
    Protocol **pl = protocol_copyProtocolList(p, &n);
    od_protocols(pl, n, "inherits");
    free(pl);
    for (int req = 1; req >= 0; req--)
        for (int inst = 1; inst >= 0; inst--) {
            struct objc_method_description *d = protocol_copyMethodDescriptionList(p, req, inst, &n);
            char **v = malloc((n + 1) * sizeof *v);
            for (unsigned j = 0; j < n; j++) {
                const char *ext = _protocol_getMethodTypeEncoding(p, d[j].name, req, inst);
                char *t = od_fmt2(d[j].types, ext ? ext : "(no extended types)");
                v[j] = od_fmt2(sel_getName(d[j].name), t);
                free(t);
            }
            free(d);
            od_emit(req ? (inst ? "req-" : "req+") : (inst ? "opt-" : "opt+"), v, n);
        }
    objc_property_t *pp = protocol_copyPropertyList(p, &n);
    char **v = malloc((n + 1) * sizeof *v);
    for (unsigned j = 0; j < n; j++) v[j] = od_fmt2(property_getName(pp[j]), property_getAttributes(pp[j]));
    free(pp);
    od_emit("prop", v, n);
}

/* method lookup through the class hierarchy (categories included) */
static void od_lookup(const char *cls, const char *sel, int meta)
{
    Class c = objc_getClass(cls);
    if (c && meta) c = object_getClass((id)c);
    Method m = c ? (meta ? class_getClassMethod(objc_getClass(cls), sel_registerName(sel))
                         : class_getInstanceMethod(c, sel_registerName(sel)))
                 : NULL;
    printf("lookup %c[%s %s] %s\n", meta ? '+' : '-', cls, sel, m ? method_getTypeEncoding(m) : "none");
}

static void od_conforms(const char *cls, const char *proto)
{
    Class c = objc_getClass(cls);
    Protocol *p = objc_getProtocol(proto);
    printf("conforms %s %s %d\n", cls, proto, c && p ? (int)class_conformsToProtocol(c, p) : -1);
}

/* the synthetic image's metadata (objcmeta.m) */
static const char *const om_classes[] = {"OMBase", "OMDerived", "OMLeaf", "OMMissingSub", "OMMissingSubSub", "NSObject"};
static const char *const om_protocols[] = {"OMBaseProto", "OMProto", "OMCatProto", "OMNativeCatProto", "OMXPCProto"};
static const struct { const char *cls, *sel; int meta; } om_lookups[] = {
    {"OMBase", "baseReq", 0}, {"OMBase", "protoReq:", 0}, {"OMBase", "name", 0}, {"OMBase", "setName:", 0},
    {"OMBase", "count", 0}, {"OMBase", "rect", 0}, {"OMBase", "catMethod", 0}, {"OMBase", "catProtoMethod", 0},
    {"OMBase", "catClassMethod", 1}, {"OMBase", "classReq", 1}, {"OMBase", "shared", 1}, {"OMBase", "setShared:", 1},
    {"OMBase", "om_nativeCat", 0}, {"OMDerived", "baseReq", 0}, {"OMDerived", "derivedOnly:with:", 0},
    {"OMDerived", "catMethod", 0}, {"OMDerived", "catClassMethod", 1}, {"OMDerived", "description", 0},
    {"OMLeaf", "leaf", 0}, {"OMLeaf", "derivedOnly:with:", 0}, {"OMLeaf", "om_nativeCat", 1},
    {"OMMissingSub", "missingSub", 0}, {"OMMissingSubSub", "missingSubSub", 0}, {"NSObject", "om_nativeCat", 0},
    {"NSObject", "om_nativeCat", 1}, {"NSObject", "om_missingSubCat", 0},
};
static const struct { const char *cls, *proto; } om_conformance[] = {
    {"OMBase", "OMProto"}, {"OMBase", "OMBaseProto"}, {"OMBase", "NSObject"}, {"OMBase", "OMCatProto"},
    {"OMDerived", "OMCatProto"}, {"OMLeaf", "NSCopying"}, {"NSObject", "OMNativeCatProto"}, {"OMMissingSub", "OMProto"},
};

static void om_dump(void)
{
    for (size_t k = 0; k < sizeof om_classes / sizeof *om_classes; k++) {
        if (!strcmp(om_classes[k], "NSObject")) {  /* native class: only what the guest's category added */
            for (size_t j = 0; j < sizeof om_lookups / sizeof *om_lookups; j++)
                if (!strcmp(om_lookups[j].cls, "NSObject")) od_lookup(om_lookups[j].cls, om_lookups[j].sel, om_lookups[j].meta);
            continue;
        }
        od_class(om_classes[k]);
    }
    for (size_t k = 0; k < sizeof om_protocols / sizeof *om_protocols; k++) od_protocol(om_protocols[k]);
    for (size_t k = 0; k < sizeof om_lookups / sizeof *om_lookups; k++)
        if (strcmp(om_lookups[k].cls, "NSObject")) od_lookup(om_lookups[k].cls, om_lookups[k].sel, om_lookups[k].meta);
    for (size_t k = 0; k < sizeof om_conformance / sizeof *om_conformance; k++)
        od_conforms(om_conformance[k].cls, om_conformance[k].proto);
}
#endif
