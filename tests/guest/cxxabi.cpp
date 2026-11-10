// C++ ABI differences at the guest/native boundary, run by tests/guest/cxxabi.sh. Native libc++ (and the helper
// libcxxabihelper.dylib, built x86_64 + arm64) is called from guest code:
//   1 std::length_error thrown and caught by exact type in the guest
//   2 std::ios_base::getloc (class result through x8 vs hidden rdi) and the
//     std::locale copy constructor (C1 this-return), then locale::name()
//   3 std::to_string (std::string result: x8, and the string layout)
//   4 std::ostringstream::str() (libc++ fills a guest-laid-out std::string)
//   5 std::string by const reference and by value to and from the helper
//   6 std::function: sizeof on each side, and the helper calling a guest one
//   7 C2 this-return: a guest class derived from std::runtime_error keeps
//     using x0 after the base constructor (Apple arm64 returns this)
//   8 class results through x8 from the helper (non-trivial class by value)
//   9 coresymbolicationd+0x42dc: the inline __put_character_sequence of
//     operator<<(ostream&, const char*) computes fill() through getloc (x8),
//     use_facet<ctype<char>> and widen; into a guest streambuf (no std::string)
//  10 std::locale copy, assignment and ==, a guest facet in a native locale
//  11 systemkeychain+0xc628..c654: a path scanned backwards for '/', each
//     prefix built by the substring constructor (native), read inline and
//     destroyed inline (operator delete of the long buffer), then operator=
//  12 std::string by value into and out of the helper, grown in place by it,
//     owned by it, inside an aggregate by value
//  13 containers of std::string crossing: vector, map, pair
//  14 std::function made, stored and called by the helper
//  15 wchar_t strings: to_wstring, wstring to and from the helper
//  16 exceptions thrown by native libc++ caught by exact type: vector::at,
//     string::at, stoi, thread::join (system_error), ios_base::failure, locale
//  17 the same caught by base, rethrown through exception_ptr
//  18 guest-thrown exceptions caught by the helper, by exact type and by base
//  19 RTTI: typeid equality and dynamic_cast across the boundary, for types
//     defined in the guest and in the helper, and libc++-owned typeinfos
// Each line is semantic (sizes, contents), never an address. Section N runs
// alone with argument N; no argument runs every section.
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <iostream>
#include <locale>
#include <functional>
#include <vector>
#include <map>
#include <iomanip>
#include <system_error>
#include <thread>
#include <exception>
#include <cstring>
#include <cwchar>
#include "cxxabihelper.h"

__attribute__((noinline)) static void thrower(int k) {
    if (k == 1) throw std::length_error("len-err");
    if (k == 2) throw std::out_of_range("oor");
}
struct MyErr : std::runtime_error {
    int code;
    __attribute__((noinline)) MyErr(const char *m, int c) : std::runtime_error(m), code(c) {}
};

// A guest streambuf: output collected in a char array (no std::string), input from one.
struct GBuf : std::streambuf {
    char out[256];
    size_t n = 0;
    GBuf() { out[0] = 0; }
    explicit GBuf(const char *in) { out[0] = 0; char *p = const_cast<char *>(in); setg(p, p, p + strlen(in)); }
    int overflow(int c) override {
        if (c != EOF && n + 1 < sizeof out) { out[n++] = (char)c; out[n] = 0; }
        return c == EOF ? 0 : c;
    }
};
// A guest facet: decimal point ',' and grouping by 3 with '.'
struct GPunct : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
// The substring constructor basic_string(const basic_string&, pos, n, const allocator&),
// called through libc++'s exported C2 symbol as systemkeychain's import does (this SDK's
// headers inline it). Signature from the mangled name; the arm64 this-return is unused.
extern "C" void sk_substr_ctor(void *self, const std::string &s, size_t pos, size_t n, const std::allocator<char> &a)
    __asm__("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC2ERKS5_mmRKS4_");
// The systemkeychain shape: does this prefix name a directory we know?
__attribute__((noinline)) static int known_dir(const char *p) {
    return !strcmp(p, "/private/var/db") || !strcmp(p, "/private/var/db/SystemKey.d");
}
static std::string pstr(const std::vector<std::string> &v) {
    std::string r;
    for (auto &s : v) { r += s; r += ','; }
    return r;
}
__attribute__((noinline)) static void thr16(int k) {
    switch (k) {
    case 1: { std::vector<int> v(3); (void)v.at(7); break; }
    case 2: { std::string s("abc"); (void)s.at(9); break; }
    case 3: (void)std::stoi(std::string("zz")); break;
    case 4: { std::thread t; t.join(); break; }
    case 5: { GBuf b("abc"); std::istream in(&b); in.exceptions(std::ios::failbit); int x; in >> x; break; }
    case 6: { std::locale l("no-such-locale.hao"); break; }
    }
}
struct GErr : std::runtime_error {
    int k;
    GErr(int x) : std::runtime_error("gerr"), k(x) {}
};
__attribute__((noinline)) static void thr18(int k) {
    switch (k) {
    case 1: throw std::out_of_range("g-oor");
    case 2: throw std::length_error("g-len");
    case 3: throw std::system_error(std::make_error_code(std::errc::permission_denied), "g-sys");
    case 4: throw GErr(4);
    case 5: throw std::bad_alloc();
    case 6: throw HErr(66);
    case 7: throw 7;
    case 8: throw 1.5;
    }
}
struct GDer : HDer { int id() const override; };     // key function below: typeinfo unique in the guest
int GDer::id() const { return 3; }
struct GSib : HBase { int id() const override { return 4; } };   // inline only: weak typeinfo

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // a crash keeps the lines before it
    int sec = argc > 1 ? atoi(argv[1]) : 0;
    if (!sec || sec == 1) {
        try { thrower(1); }
        catch (std::length_error &e) { printf("1 caught length_error: %s\n", e.what()); }
        catch (std::exception &e) { printf("1 caught other: %s\n", e.what()); }
    }
    if (!sec || sec == 2) {
        std::locale l = std::cout.getloc();
        std::locale c(l);
        printf("2 getloc name: %s copy: %s same: %d\n", l.name().c_str(), c.name().c_str(), (int)(l == c));
    }
    if (!sec || sec == 3) {
        std::string s = std::to_string(12345), t = std::to_string(-1.5), u = std::to_string(123456789012345678LL);
        printf("3 to_string: %s %zu | %s | %s %zu\n", s.c_str(), s.size(), t.c_str(), u.c_str(), u.size());
    }
    if (!sec || sec == 4) {
        std::ostringstream o;
        o << "abc" << 42;
        std::string s = o.str();
        o << std::string(40, 'z');
        std::string l = o.str();
        printf("4 ostringstream: %s (%zu) long %zu\n", s.c_str(), s.size(), l.size());
    }
    if (!sec || sec == 5) {
        std::string a("hi"), b(40, 'y');
        std::string j = h_join(a, b);
        std::string m = h_make(3), n = h_make(30);
        printf("5 string: len %zu %zu join %zu %c%c make %zu %s %zu\n", h_len(a), h_len(b), j.size(), j[0], j[j.size() - 1],
               m.size(), m.c_str(), n.size());
    }
    if (!sec || sec == 6) {
        std::function<int(int)> f = [](int x) { return x * 3 + 1; };
        int k = 9;
        std::function<int(int)> g = [k](int x) { return x + k; };
        printf("6 function: guest sizeof %zu native sizeof %zu call %d %d\n", sizeof f, h_function_size(), h_call(f, 4), h_call(g, 4));
    }
    if (!sec || sec == 7) {
        MyErr e("c2", 77);
        printf("7 this-return: %d %s\n", e.code, e.what());
    }
    if (!sec || sec == 8) {
        HNT a = h_make_nt(5);
        HNT b = a.twice();
        printf("8 class result: %ld %ld %ld %ld\n", a.a, a.b, b.a, b.b);
    }
    if (!sec || sec == 9) {
        GBuf b;
        std::ostream os(&b);
        os << "csd" << std::setw(8) << "pad" << std::left << std::setw(5) << "L" << '|';
        std::locale l = os.getloc();
        const std::ctype<char> &ct = std::use_facet<std::ctype<char> >(l);
        char up[8] = "lower";
        ct.toupper(up, up + 5);
        printf("9 fill-path: [%s] fill %d widen %c toupper %s numpunct %d dp %c\n", b.out, (int)os.fill(), ct.widen('w'), up,
               (int)std::has_facet<std::numpunct<char> >(l), std::use_facet<std::numpunct<char> >(l).decimal_point());
    }
    if (!sec || sec == 10) {
        std::locale c = std::locale::classic();
        std::locale d(c);
        std::locale g(c, new GPunct);
        std::locale e;
        e = g;
        GBuf b;
        std::ostream os(&b);
        os.imbue(g);
        os << 1234567 << ' ' << std::fixed << std::setprecision(2) << 3.25;
        printf("10 locale: c==d %d c==g %d e==g %d hasG %d [%s] dp %c\n", (int)(c == d), (int)(c == g), (int)(e == g),
               (int)std::has_facet<GPunct>(e), b.out, std::use_facet<std::numpunct<char> >(e).decimal_point());
    }
    if (!sec || sec == 11) {
        std::string path("/private/var/db/SystemKey.d/nested/component/system.keychain");
        std::string found;
        int tries = 0;
        size_t lens = 0;
        for (long i = (long)path.size() - 1; i >= 1; i--) {
            if (path[i] != '/') continue;
            alignas(std::string) unsigned char raw[sizeof(std::string)];
            std::allocator<char> al;
            sk_substr_ctor(raw, path, 0, (size_t)i, al);
            std::string &pre = *reinterpret_cast<std::string *>(raw);
            tries++;
            lens += strlen(pre.c_str());
            int hit = known_dir(pre.c_str());
            if (hit) found = pre;
            pre.~basic_string();          // inline: operator delete of the long buffer (+0xc650)
            if (hit) break;
        }
        std::string copy;
        copy = path;
        printf("11 prefix: tries %d lens %zu found %s (%zu) copy %zu %d\n", tries, lens, found.c_str(), found.size(), copy.size(),
               (int)(copy == path));
    }
    if (!sec || sec == 12) {
        std::string a("short"), b(30, 'b');
        std::string ua = h_upper(a), ub = h_upper(b);
        size_t sa = h_sink(a), sb = h_sink(std::string(40, 's'));
        std::string g("grow");
        h_append(g, " past the short buffer of either layout");
        const std::string &sn = h_static_name();
        HRec r = h_rec(25);
        size_t rl = h_rec_len(HRec{std::string("rec"), 2});
        printf("12 byval: %s %zu %c | sink %zu %zu | grown %zu %s | owned %zu %.6s | rec %zu %c %d %zu | a %s\n", ua.c_str(),
               ub.size(), ub[0], sa, sb, g.size(), g.c_str() + g.size() - 6, sn.size(), sn.c_str(), r.name.size(), r.name[24], r.n, rl,
               a.c_str());
    }
    if (!sec || sec == 13) {
        std::vector<std::string> v = h_split("alpha,beta,,a-very-long-element-of-the-list,beta", ',');
        std::vector<std::string> w = {"x", std::string(30, 'y'), "x", "z"};
        std::map<std::string, int> m = h_count(w);
        std::pair<std::string, std::string> p = h_halves("left-half|right-half-longer-than-22");
        printf("13 containers: %zu [%s] total %zu %zu | map %zu x%d y%d | %s / %s\n", v.size(), pstr(v).c_str(), h_total(v), h_total(w),
               m.size(), m["x"], m[std::string(30, 'y')], p.first.c_str(), p.second.c_str());
    }
    if (!sec || sec == 14) {
        std::function<long(long)> add = h_adder(40);
        std::function<long(long)> copy = add;
        long big[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        struct Big { long v[8]; } bg;
        memcpy(bg.v, big, sizeof big);
        std::function<long(long)> heavy = [bg](long x) { long s = x; for (long i : bg.v) s += i; return s; };
        int base = 100;
        h_store([base](int x) { return base + x * 2; });
        int f1 = h_fire(5), f2 = h_fire(-1);
        h_clear();
        int f3 = h_fire(1);
        auto wrapped = h_wrap([](const std::string &s) { return s + s; });
        std::string ws = wrapped("ab");
        printf("14 function: adder %ld copy %ld heavy %ld | stored %d %d cleared %d | wrap %s\n", add(2), h_call_l(copy, 3),
               h_call_l(heavy, 0), f1, f2, f3, ws.c_str());
    }
    if (!sec || sec == 15) {
        std::wstring a = std::to_wstring(-4096), b(L"wide string longer than the short buffer");
        std::wstring j = h_wjoin(a, b);
        char nar[96];
        size_t k = 0;
        for (wchar_t c : j) if (k + 1 < sizeof nar) nar[k++] = (char)c;
        nar[k] = 0;
        printf("15 wstring: %zu %zu wcslen %zu join %zu %zu [%s]\n", a.size(), h_wlen(b), wcslen(a.c_str()), j.size(), h_wlen(j), nar);
    }
    if (!sec || sec == 16) {
        for (int k = 1; k <= 6; k++) {
            try { thr16(k); printf("16.%d no throw\n", k); }
            catch (std::out_of_range &e) { printf("16.%d out_of_range %s\n", k, e.what()); }
            catch (std::invalid_argument &e) { printf("16.%d invalid_argument %s\n", k, e.what()); }
            catch (std::ios_base::failure &e) {
                printf("16.%d ios_base::failure stream %d %s\n", k, (int)(e.code() == std::io_errc::stream), e.what());
            }
            catch (std::system_error &e) {
                printf("16.%d system_error %d generic %d %s | %s\n", k, e.code().value(),
                       (int)(&e.code().category() == &std::generic_category()), e.code().message().c_str(), e.what());
            }
            catch (std::runtime_error &e) { printf("16.%d runtime_error %.40s\n", k, e.what()); }
            catch (std::exception &e) { printf("16.%d other exception\n", k); }
        }
    }
    if (!sec || sec == 17) {
        for (int k = 1; k <= 6; k++) {
            std::exception_ptr p;
            try { thr16(k); }
            catch (std::logic_error &e) { printf("17.%d logic_error %s\n", k, e.what()); p = std::current_exception(); }
            catch (std::system_error &e) { printf("17.%d system_error %d\n", k, e.code().value()); p = std::current_exception(); }
            catch (std::exception &e) { printf("17.%d exception %.20s\n", k, e.what()); p = std::current_exception(); }
            try { if (p) std::rethrow_exception(p); }
            catch (std::out_of_range &) { printf("17.%d again out_of_range\n", k); }
            catch (std::invalid_argument &) { printf("17.%d again invalid_argument\n", k); }
            catch (std::ios_base::failure &) { printf("17.%d again failure\n", k); }
            catch (std::system_error &) { printf("17.%d again system_error\n", k); }
            catch (std::runtime_error &) { printf("17.%d again runtime_error\n", k); }
            catch (...) { printf("17.%d again other\n", k); }
        }
        std::exception_ptr q = std::make_exception_ptr(std::out_of_range("made"));
        try { std::rethrow_exception(q); }
        catch (std::logic_error &e) { printf("17 made %s\n", e.what()); }
    }
    if (!sec || sec == 18) {
        for (int k = 1; k <= 8; k++) {
            char buf[96];
            int r = h_catch(thr18, k, buf, sizeof buf);
            printf("18.%d helper caught %d %s\n", k, r, buf);
        }
    }
    if (!sec || sec == 19) {
        HBase *d = h_new_der(), *b = h_new_base();
        GDer *gd = new GDer;
        GSib *gs = new GSib;
        HBase *gdb = gd, *gsb = gs;
        printf("19.1 helper object: tid==HDer %d tid!=HBase %d dyn %d dynGDer %d h_tid %d id %d\n", (int)(typeid(*d) == typeid(HDer)),
               (int)(typeid(*d) != typeid(HBase)), dynamic_cast<HDer *>(d) != nullptr, dynamic_cast<GDer *>(d) != nullptr,
               (int)(h_tid(d) == typeid(HDer)), d->id());
        printf("19.2 helper rtti: h_tid_der==HDer %d same %d base %d isder %d %d\n", (int)(h_tid_der() == typeid(HDer)),
               h_same(h_tid_der(), typeid(HDer)), (int)(h_tid(b) == typeid(HBase)), h_is_der(d), h_is_der(b));
        printf("19.3 guest objects: isder %d %d mostderived %d %d h_tid==GDer %d same %d h_tid==GSib %d same %d id %d %d\n", h_is_der(gdb),
               h_is_der(gsb), h_most_derived(gdb) == (void *)gd, h_most_derived(gsb) == (void *)gs, (int)(h_tid(gdb) == typeid(GDer)),
               h_same(h_tid(gdb), typeid(GDer)), (int)(h_tid(gsb) == typeid(GSib)), h_same(h_tid(gsb), typeid(GSib)), gdb->id(),
               gsb->id());
        printf("19.4 names: %s %s %s\n", h_tid(gdb).name(), h_tid(gsb).name(), h_tid(d).name());
        printf("19.5 libc++ typeinfos: string %d %d oor %d %d\n", (int)(h_tid_string() == typeid(std::string)),
               h_same(h_tid_string(), typeid(std::string)), (int)(h_tid_oor() == typeid(std::out_of_range)),
               h_same(h_tid_oor(), typeid(std::out_of_range)));
        delete d; delete b; delete gdb; delete gsb;
    }
    return 0;
}
