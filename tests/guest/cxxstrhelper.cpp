// tests/guest/cxxstr.sh: the native helper (x86_64 for hao, arm64 for the reference).
#include "cxxstrhelper.h"
#include <stdexcept>

namespace cxs {
Box::Box(const std::string &s) : v_(s), n_(1) {}
Box::Box(std::string s, long n) : v_(std::move(s)), n_(n) {}
Box::~Box() {}
void Box::set(const std::string &s) { v_ = s; n_++; }
void Box::take(std::string s) { v_ += s; n_++; }
void Box::read(std::string &out) { out = v_ + "/" + std::to_string(n_); }
long Box::len() const { return (long)v_.size() * 1000 + n_; }
const char *Box::c_str() const { return v_.c_str(); }
long Box::count(const std::string &s, char c)
{
    long n = 0;
    for (char x : s) n += x == c;
    return n;
}
long hash(const std::string &s)
{
    unsigned long h = 1469598103934665603ul;
    for (unsigned char x : s) h = (h ^ x) * 1099511628211ul;
    return (long)(h >> 1) + (long)s.size();
}
long byval(std::string s, long k)
{
    s.append((size_t)k, '+');
    return hash(s);
}
void grow(std::string &s, long n)
{
    for (long k = 0; k < n; k++) s.push_back((char)('a' + k % 26));
}
void shrink(std::string &s, long n) { s.resize((size_t)n); }
void assign(std::string &dst, const std::string &src) { dst = src; }
long mix(long a, const std::string &s, long b, std::string &out)
{
    out = std::to_string(a) + ":" + s + ":" + std::to_string(b);
    return a * 7 + b + (long)s.size();
}
void must_fit(const std::string &s)
{
    if (s.size() > 22) throw std::length_error("too long: " + s);
}
}

// As LocationSupport and the other libraries of the class: ObjC metadata keeps the library native
// (src/macho.c hao_macho_guest_cxx_eligible), so its C++ interface is the boundary.
__attribute__((used, section("__DATA,__objc_imageinfo"))) static const unsigned objc_imageinfo[2] = {0, 0};
