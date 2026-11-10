// tests/guest/cxxstr.sh: a native C++ library that is not libc++ and takes std::string
// (LocationSupport's, ProtocolBuffer's and caulk's shapes). No member is inline: the guest
// sees a Box only through these functions.
#include <string>

namespace cxs {
class Box {
    std::string v_;
    long n_;
public:
    Box(const std::string &s);                       // C1, const&
    Box(std::string s, long n);                      // C1, by value
    ~Box();
    void set(const std::string &s);                  // member: this-ness only from the code
    void take(std::string s);                        // member, by value
    void read(std::string &out);                     // member, non-const& (PB::Reader::read)
    long len() const;
    const char *c_str() const;
    static long count(const std::string &s, char c); // static member: no this
};
long hash(const std::string &s);                     // namespace function
long byval(std::string s, long k);
void grow(std::string &s, long n);                   // short -> long and long -> longer
void shrink(std::string &s, long n);                 // keep the first n chars
void assign(std::string &dst, const std::string &src);
long mix(long a, const std::string &s, long b, std::string &out);
void must_fit(const std::string &s);                 // throws std::length_error past 22 chars
}
