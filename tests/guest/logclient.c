/* tests/guest/logclient.sh: os_log_set_client_type, called first thing in main, then a log line. Linked
 * with GSS.framework (as GSS's helpers gssd, GSSCred and kcm are), which loads CoreFoundation: CF's
 * initializer logs, starting libtrace, when __CF_USER_TEXT_ENCODING does not name this user. */
#include <os/log.h>
#include <stdio.h>

extern void os_log_set_client_type(unsigned char type, unsigned int flags);

int main(void)
{
    os_log_set_client_type(1, 0);
    os_log(OS_LOG_DEFAULT, "logclient: after os_log_set_client_type");
    printf("logclient: running\n");
    return 0;
}
