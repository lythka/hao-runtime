#include "cxxabihelper.h"
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <cctype>
#include <cstdio>

size_t h_len(const std::string &s) { return s.size(); }
std::string h_join(const std::string &a, const std::string &b) { return a + "+" + b; }
std::string h_make(int n) { return std::string((size_t)n, 'x') + "!"; }
size_t h_function_size() { return sizeof(std::function<int(int)>); }
int h_call(const std::function<int(int)> &f, int x) { return f ? f(x) : -1; }
HNT::HNT(long x) : a(x), b(x * 3) {}
HNT::HNT(const HNT &o) : a(o.a + 1), b(o.b + 1) {}
HNT::~HNT() { a = -1; }
HNT HNT::twice() const { HNT r(a * 2); r.b = b * 2; return r; }
HNT h_make_nt(long x) { return HNT(x + 10); }

std::string h_upper(std::string s) {
    for (auto &c : s) c = (char)toupper((unsigned char)c);
    return s;
}
size_t h_sink(std::string s) { s += "#"; return s.size(); }
void h_append(std::string &s, const char *t) { s += t; }
const std::string &h_static_name() {
    static const std::string n("helper-owned string, longer than the short buffer");
    return n;
}
HRec h_rec(int n) { return HRec{std::string((size_t)n, 'r'), n}; }
size_t h_rec_len(HRec r) { return r.name.size() + (size_t)r.n; }

std::vector<std::string> h_split(const std::string &s, char sep) {
    std::vector<std::string> v;
    size_t b = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || s[i] == sep) { v.push_back(s.substr(b, i - b)); b = i + 1; }
    return v;
}
size_t h_total(const std::vector<std::string> &v) {
    size_t t = 0;
    for (auto &s : v) t += s.size();
    return t;
}
std::map<std::string, int> h_count(const std::vector<std::string> &v) {
    std::map<std::string, int> m;
    for (auto &s : v) m[s]++;
    return m;
}
std::pair<std::string, std::string> h_halves(const std::string &s) {
    return {s.substr(0, s.size() / 2), s.substr(s.size() / 2)};
}

std::function<long(long)> h_adder(long k) { return [k](long x) { return x + k; }; }
long h_call_l(const std::function<long(long)> &f, long x) { return f(x); }
static std::function<int(int)> stored;
void h_store(std::function<int(int)> f) { stored = std::move(f); }
int h_fire(int x) { return stored ? stored(x) : -1; }
void h_clear() { stored = nullptr; }
std::function<std::string(const std::string &)> h_wrap(std::function<std::string(const std::string &)> f) {
    return [f](const std::string &s) { return "[" + f(s) + "]"; };
}

std::wstring h_wjoin(const std::wstring &a, const std::wstring &b) { return a + L"|" + b; }
size_t h_wlen(const std::wstring &s) { return s.size(); }

HErr::~HErr() {}
int h_catch(void (*f)(int), int k, char *buf, size_t n) {
    buf[0] = 0;
    try { f(k); }
    catch (const std::out_of_range &e) { strlcpy(buf, e.what(), n); return 1; }
    catch (const std::logic_error &e) { strlcpy(buf, e.what(), n); return 2; }
    catch (const std::system_error &e) { strlcpy(buf, e.what(), n); return 3; }
    catch (const std::runtime_error &e) { strlcpy(buf, e.what(), n); return 4; }
    catch (const std::exception &e) { strlcpy(buf, e.what(), n); return 5; }
    catch (const HErr &e) { snprintf(buf, n, "herr %d", e.code); return 6; }
    catch (int i) { snprintf(buf, n, "int %d", i); return 7; }
    catch (...) { return 8; }
    return 0;
}

HBase::~HBase() {}
int HBase::id() const { return 1; }
HDer::~HDer() {}
int HDer::id() const { return 2; }
HBase *h_new_der() { return new HDer; }
HBase *h_new_base() { return new HBase; }
int h_is_der(const HBase *p) { return dynamic_cast<const HDer *>(p) != nullptr; }
void *h_most_derived(const HBase *p) { return const_cast<void *>(dynamic_cast<const void *>(p)); }
const std::type_info &h_tid(const HBase *p) { return typeid(*p); }
const std::type_info &h_tid_der() { return typeid(HDer); }
const std::type_info &h_tid_string() { return typeid(std::string); }
const std::type_info &h_tid_oor() { return typeid(std::out_of_range); }
int h_same(const std::type_info &a, const std::type_info &b) { return (a == b) | (a.hash_code() == b.hash_code()) << 1; }
