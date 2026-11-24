#include <os/log.h>
#include <xpc/xpc.h>
static void accept_peer(xpc_connection_t peer) {
    xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
        if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
        xpc_object_t reply = xpc_dictionary_create_reply(message);
        if (!reply) return;
        xpc_dictionary_set_uint64(reply, "answer",
                                  xpc_dictionary_get_uint64(message, "challenge") + 1);
        xpc_connection_send_message(peer, reply);
        xpc_release(reply);
    });
    xpc_connection_resume(peer);
}

int main(void) {
    os_log(OS_LOG_DEFAULT, "hao framework XPC fixture service entered %{public}s", SERVICE_NAME);
    xpc_main(accept_peer);
}
