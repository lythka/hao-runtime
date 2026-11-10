/* A guest dylib's own os_log records and signposts (tests/guest/oslog.sh): its __dso_handle is its header. */
#include <os/log.h>
#include <os/signpost.h>

void helper_log(os_log_t log, int n)
{
    os_log(log, "helper %d %{public}s %{private}s", n, "dylib-public", "dylib-secret");
    os_log_error(log, "helper error %ld", (long)n * 3);
    os_signpost_event_emit(log, OS_SIGNPOST_ID_EXCLUSIVE, "HelperEvent", "helper signpost %d", n);
}
