/*
 * os_log from guest code (tests/guest/oslog.sh, src/oslog.c): every entry point that names its format by
 * the caller's __dso_handle, from the main executable and from a guest dylib (osloghelper.c), with public,
 * private, sensitive, numeric, object and masked arguments, every log type, a custom subsystem, signposts
 * and the legacy variadic _os_log_internal. The runner reads the records back with `log show`.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <os/log.h>
#include <os/signpost.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void helper_log(os_log_t log, int n);   /* osloghelper.c */
extern char *_os_log_send_and_compose_impl(uint32_t options, const char **fmt_out, char *out, size_t outlen,
                                           void *dso, os_log_t log, os_log_type_t type, const char *fmt,
                                           uint8_t *buf, uint32_t size);

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "tag";
    os_log_t lg = os_log_create("com.example.hao.oslog", "test");
    const char *pub = "PUBLIC-TEXT", *sec = "SECRET-TEXT";
    int n = 42;
    long long big = -1234567890123LL;
    double d = 2.75;
    CFStringRef obj = CFSTR("cf-object");
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &n);
    printf("%d\n", getpid());
    fflush(stdout);

    os_log(lg, "%{public}s start", tag);
    os_log(lg, "public %{public}s private %{private}s sensitive %{sensitive}s plain %s", pub, sec, sec, sec);
    os_log(lg, "numbers %d %{private}d %lld %u %x %#llx %c %5.2f %e %%", n, n, big, (unsigned)n, n, (unsigned long long)big, 'z', d, d);
    os_log(lg, "objects %{public}@ %@ %{private}@", obj, num, obj);
    os_log(lg, "precision %.*s|%.4s|%{public}.3s", 3, pub, pub, sec);
    os_log(lg, "annotated %{bool}d %{BOOL}d %{errno}d %{time_t}d", 1, 0, ENOENT, 0);
    os_log(lg, "mask %{private, mask.hash}s", sec);
    os_log_info(lg, "info %d", 1);
    os_log_debug(lg, "debug %d", 2);
    os_log_error(lg, "error %{public}s", "e");
    os_log_fault(lg, "fault %{public}s", "f");
    os_log(OS_LOG_DEFAULT, "default log %d", 7);
    errno = EPERM;
    os_log(lg, "errno %m");
    os_log_t sub = os_log_create("com.example.hao.oslog.sub", "other");
    os_log(sub, "custom subsystem %{public}s %d", "sub", 3);

    os_signpost_id_t id = os_signpost_id_generate(lg);
    os_signpost_interval_begin(lg, id, "Interval", "begin %d %{public}s", 1, "b");
    os_signpost_event_emit(lg, id, "Event", "event %{public}s", "x");
    os_signpost_event_emit(lg, id, "Bare");
    os_signpost_interval_end(lg, id, "Interval", "end %d", 2);

    _os_log_internal(&__dso_handle, lg, OS_LOG_TYPE_DEFAULT, "internal %d %{public}s %f %lld", 3, "pub", 1.5, big);

    char out[256];
    char *r = _os_log_send_and_compose_impl(3, NULL, out, sizeof out, &__dso_handle, lg, OS_LOG_TYPE_DEFAULT,
                                            "composed %d", (uint8_t[]){0, 1, 0, 4, 9, 0, 0, 0}, 8);
    printf("composed: %s\n", r ? r : "(null)");

    helper_log(lg, 5);
    os_log(lg, "%{public}s end", tag);
    CFRelease(num);
    return 0;
}
