#include <dispatch/dispatch.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <xpc/xpc.h>

extern const char *fixture_service_name(void);

int main(int argc, char **argv) {
    const char *name = fixture_service_name();
    fprintf(stderr, "client framework loaded, looking up %s\n", name);
    if (argc != 2 || !dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
        fprintf(stderr, "observer load failed: %s\n", argc == 2 ? dlerror() : "expected dylib path");
        return 3;
    }
    xpc_connection_t peer = xpc_connection_create(name, NULL);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) { (void)event; });
    xpc_connection_resume(peer);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(request, "challenge", 73);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int status = 1;
    xpc_connection_send_message_with_reply(peer, request, NULL, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
            xpc_dictionary_get_uint64(reply, "answer") == 74) {
            puts("service replied 74");
            status = 0;
        } else {
            char *description = xpc_copy_description(reply);
            fprintf(stderr, "XPC reply %s\n", description);
            free(description);
        }
        dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC))) {
        fputs("XPC reply timed out\n", stderr);
        return 2;
    }
    return status;
}
