// C++ helper of tests/guest/cxxabi.cpp. cxxabi.sh builds it x86_64 + arm64 + arm64e:
// under hao today the guest binds to its x86_64 slice (native), natively to arm64.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <functional>
#include <typeinfo>
#include <cstddef>

size_t h_len(const std::string &s);
std::string h_join(const std::string &a, const std::string &b);
std::string h_make(int n);
size_t h_function_size();
int h_call(const std::function<int(int)> &f, int x);
struct HNT {                         // non-trivial: returned indirectly (x8 / hidden rdi)
    long a, b;
    HNT(long x);
    HNT(const HNT &o);
    ~HNT();
    HNT twice() const;
};
HNT h_make_nt(long x);

// 12: std::string by value in and out, in place, and owned by the helper
std::string h_upper(std::string s);                 // by value in (indirect), by value out (x8)
size_t h_sink(std::string s);                       // by value in, the callee destroys it
void h_append(std::string &s, const char *t);       // the helper grows a caller's string
const std::string &h_static_name();                 // a string the helper owns
struct HRec { std::string name; int n; };
HRec h_rec(int n);                                  // aggregate holding a string, by value out
size_t h_rec_len(HRec r);                           // ... and by value in

// 13: containers of std::string
std::vector<std::string> h_split(const std::string &s, char sep);
size_t h_total(const std::vector<std::string> &v);
std::map<std::string, int> h_count(const std::vector<std::string> &v);
std::pair<std::string, std::string> h_halves(const std::string &s);

// 14: std::function made, stored and called by the helper
std::function<long(long)> h_adder(long k);
long h_call_l(const std::function<long(long)> &f, long x);
void h_store(std::function<int(int)> f);
int h_fire(int x);
void h_clear();
std::function<std::string(const std::string &)> h_wrap(std::function<std::string(const std::string &)> f);

// 15: wchar_t strings
std::wstring h_wjoin(const std::wstring &a, const std::wstring &b);
size_t h_wlen(const std::wstring &s);

// 18: guest-thrown exceptions caught by the helper: 1 out_of_range, 2 logic_error, 3 system_error,
// 4 runtime_error, 5 exception, 6 HErr, 7 int, 8 ...; what() copied into buf
struct HErr { int code; HErr(int c) : code(c) {} virtual ~HErr(); };
int h_catch(void (*f)(int), int k, char *buf, size_t n);

// 19: RTTI across the boundary; key functions in the helper
struct HBase { virtual ~HBase(); virtual int id() const; };
struct HDer : HBase { ~HDer() override; int id() const override; };
HBase *h_new_der();
HBase *h_new_base();
int h_is_der(const HBase *p);                       // dynamic_cast<const HDer *> in the helper
void *h_most_derived(const HBase *p);               // dynamic_cast<const void *>
const std::type_info &h_tid(const HBase *p);        // typeid(*p)
const std::type_info &h_tid_der();
const std::type_info &h_tid_string();               // libc++-owned typeinfos
const std::type_info &h_tid_oor();
int h_same(const std::type_info &a, const std::type_info &b);
