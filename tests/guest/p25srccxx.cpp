// P25 step A's C++ source (tools/gen_bridge.py --cxx: the SDK's <new> and cxxabi.h by
// mangled name): typed allocation's operator new/delete (built -ftyped-cxx-new-delete,
// as the 27.0 callers were), the plain and sized operator new/delete, and the
// __cxa_guard_acquire/release pair of a function-local static. tests/guest/p25src.sh.
#include <cstdio>
#include <cstring>
#include <new>

struct Pair { long a, b; explicit Pair(long x) : a(x), b(2 * x) {} };
static volatile long seed = 3;
static long slow_init(long x) { return x * 7 + 1; }
static long guarded(long x) { static long v = slow_init(x); return v; }

static void c_typed_new()
{
    Pair *p = new Pair(seed);             // __ZnwmSt19__type_descriptor_t
    long *v = new long[seed + 2];         // __ZnamSt19__type_descriptor_t
    for (long i = 0; i < seed + 2; i++) v[i] = p->b * i;
    std::printf("typed new %ld %ld %ld\n", p->a, p->b, v[seed + 1]);
    delete p;                             // __ZdlPvSt19__type_descriptor_t
    delete[] v;                           // __ZdaPvSt19__type_descriptor_t
}
static void c_plain_new()
{
    void *a = ::operator new(24);         // __Znwm
    void *b = ::operator new(8);
    std::memset(a, 0x11, 24);
    std::memset(b, 0x22, 8);
    std::printf("plain new %d %d\n", static_cast<unsigned char *>(a)[23], static_cast<unsigned char *>(b)[7]);
    ::operator delete(a, 24);             // __ZdlPvm
    ::operator delete(b);                 // __ZdlPv
}
static void c_guard()
{
    long a = guarded(seed), b = guarded(seed + 1);   // __cxa_guard_acquire / __cxa_guard_release once
    std::printf("cxa_guard %ld %ld\n", a, b);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(); } cases[] = {
        {"cxx-typednew", c_typed_new}, {"cxx-plainnew", c_plain_new}, {"cxx-guard", c_guard}};
    if (argc < 2) { std::fprintf(stderr, "usage: p25srccxx CASE|list\n"); return 2; }
    for (auto &c : cases) {
        if (!std::strcmp(argv[1], "list")) { std::printf("%s\n", c.name); continue; }
        if (!std::strcmp(argv[1], c.name)) { c.fn(); std::fflush(stdout); return 0; }
    }
    return std::strcmp(argv[1], "list") ? 2 : 0;
}
