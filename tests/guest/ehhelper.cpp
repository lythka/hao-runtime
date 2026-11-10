/* Native helper for tests/guest/eh.mm: C++ code that throws, catches and
 * cleans up around calls to guest callbacks. Built universal (x86_64 +
 * arm64 + arm64e); under hao the guest's imports bind to the x86_64 slice,
 * so every frame here is a native frame between guest frames. */
#include "ehhelper.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
struct Noisy {
    const char *name;
    ~Noisy() { printf("  native cleanup: %s\n", name); }
};
}

extern "C" {

void eh_throw_runtime(const char *msg) { throw std::runtime_error(msg); }

void eh_throw_int(int v) { throw v; }

void eh_call(eh_fn fn, void *arg)
{
    Noisy n{"eh_call"};
    fn(arg);
}

void eh_call_plain(eh_fn fn, void *arg) { fn(arg); }

int eh_catch(eh_fn fn, void *arg, char *buf, size_t len)
{
    Noisy n{"eh_catch"};
    try {
        fn(arg);
        snprintf(buf, len, "no exception");
        return 0;
    } catch (const std::exception &e) {
        snprintf(buf, len, "std::exception: %s", e.what());
        return 1;
    } catch (int v) {
        snprintf(buf, len, "int %d", v);
        return 2;
    } catch (...) {
        snprintf(buf, len, "unknown exception");
        return 3;
    }
}

int eh_catch_rethrow(eh_fn fn, void *arg)
{
    try {
        fn(arg);
    } catch (...) {
        printf("  native catch(...) rethrowing\n");
        throw;
    }
    return 0;
}

}
