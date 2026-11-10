// tests/guest/cxxstr.sh: guest code builds std::strings inline (the arm64 alternate layout) and
// passes them to a native C++ library that is not libc++ (cxxstrhelper.cpp), by const&, by value
// and by non-const&, to constructors, members, a static member and namespace functions, short
// (0, 3, 22 chars) and long (23, 100 chars). Each case prints what the guest reads back.
//   cxxstr CASE      (cxxstr list: the cases)
#include "cxxstrhelper.h"
#include <stdexcept>
#include <stdio.h>
#include <string.h>

static std::string mk(size_t n, char base)
{
    std::string s;
    for (size_t k = 0; k < n; k++) s.push_back((char)(base + k % 23));
    return s;
}
static const size_t LENS[] = {0, 3, 22, 23, 100};

static void show(const char *what, const std::string &s)
{
    printf("%s: size=%zu [%s]\n", what, s.size(), s.c_str());
}

static void c_ctor()
{
    for (size_t n : LENS) {
        cxs::Box b(mk(n, 'A'));              // const&
        cxs::Box v(mk(n, 'a'), (long)n);     // by value
        printf("ctor %zu: %ld [%s] %ld [%s]\n", n, b.len(), b.c_str(), v.len(), v.c_str());
    }
}

static void c_member()
{
    cxs::Box b(std::string("start"));
    for (size_t n : LENS) {
        b.set(mk(n, 'k'));
        printf("set %zu: %ld [%s]\n", n, b.len(), b.c_str());
        b.take(mk(n, '0'));
        printf("take %zu: %ld [%s]\n", n, b.len(), b.c_str());
    }
    std::string out = "previous";
    b.read(out);
    show("read", out);
    std::string small = mk(40, 'Q');
    cxs::Box c(std::string("x"));
    c.read(small);                           // long guest string, shorter result: kept buffer
    show("read into long", small);
}

static void c_static()
{
    for (size_t n : LENS) {
        std::string s = mk(n, 'a');
        printf("count %zu: %ld %ld\n", n, cxs::Box::count(s, 'a'), cxs::Box::count(s, 'b'));
    }
}

static void c_free()
{
    for (size_t n : LENS) {
        std::string s = mk(n, 'm');
        printf("hash %zu: %ld byval %ld %ld\n", n, cxs::hash(s), cxs::byval(s, 0), cxs::byval(mk(n, 'n'), 30));
        show("after byval", s);
    }
}

static void c_write()
{
    for (size_t n : LENS) {
        std::string s = mk(n, 'g');
        cxs::grow(s, 5);
        show("grow 5", s);
        cxs::grow(s, 30);
        show("grow 30", s);
        cxs::shrink(s, 2);
        show("shrink 2", s);
        cxs::grow(s, 0);
        show("grow 0", s);
        std::string d = mk(n, 'd');
        cxs::assign(d, mk(100 - n, 's'));
        show("assign", d);
        s += "|guest";                       // the guest's own libc++ on the written-back string
        show("append", s);
    }
}

static void c_mix()
{
    for (size_t n : LENS) {
        std::string out = mk(n, 'o');
        long r = cxs::mix((long)n, mk(n, 'z'), -3, out);
        printf("mix %zu: %ld ", n, r);
        show("out", out);
    }
}

static void c_throw()
{
    for (size_t n : LENS) {
        std::string s = mk(n, 't');
        try {
            cxs::must_fit(s);
            printf("fit %zu: ok\n", n);
        } catch (const std::length_error &e) {
            printf("fit %zu: length_error %s\n", n, e.what());
        }
        show("still", s);
    }
}

static const struct { const char *name; void (*fn)(); } cases[] = {
    {"ctor", c_ctor}, {"member", c_member}, {"static", c_static}, {"free", c_free},
    {"write", c_write}, {"mix", c_mix}, {"throw", c_throw},
};

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "list")) {
        for (auto &c : cases) printf("%s\n", c.name);
        return 0;
    }
    for (auto &c : cases)
        if (!strcmp(argv[1], c.name)) { c.fn(); return 0; }
    return 2;
}
