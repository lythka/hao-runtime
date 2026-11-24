/* Authored ordinary LaunchServices and XPC entry points for the Intel bench. */
#include <stdio.h>
#ifdef LS_MAIN
extern int branch(void);
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *out = fopen(argv[1], "w");
    if (!out) return 3;
    fprintf(out, "app %d\n", branch());
    return fclose(out) != 0;
}
#else
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#define NAME "org.hao.fixture.armselection.user"
#ifdef SERVICE
extern int branch(void);
static void accept_peer(xpc_connection_t peer) {
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) return;
        xpc_object_t reply = xpc_dictionary_create_reply(event);
        if (!reply) return;
        xpc_dictionary_set_int64(reply, "value", branch());
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
    });
    xpc_connection_resume(peer);
}
int main(void) { xpc_main(accept_peer); }
#else
int main(void) {
    xpc_connection_t peer = xpc_connection_create(NAME, NULL);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t x) { (void)x; });
    xpc_connection_resume(peer);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int status = 1;
    xpc_connection_send_message_with_reply(peer, request, NULL, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
            long long value = xpc_dictionary_get_int64(reply, "value");
            printf("service %lld\n", value);
            status = value == 11 ? 0 : 1;
        } else {
            fprintf(stderr, "XPC reply %s\n", xpc_copy_description(reply));
        }
        dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC))) return 2;
    return status;
}
#endif
#endif
