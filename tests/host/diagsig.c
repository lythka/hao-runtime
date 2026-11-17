/*
 * Host test: diagnostics written from signal handlers (src/runtime.c), run by
 * tests/host/diagsig.sh with tests/host/diagsig_interpose.c inserted, which
 * counts formatting and allocation calls while a handler runs.
 *
 *   kept      a log waiting for its directory (PID 1 before the Data volume)
 *             is not opened by hao_diag_write; its lines are kept, with no
 *             formatting or allocation.
 *   fatal     hao_diag_write_fatal (fault, abort and sigreturn reports) opens
 *             it from the handler: the boot header and the kept-lines note
 *             are built without them, and the kept lines come first.
 *   cap       the line that says the log reached its cap, the same.
 *
 *   diagsig DIR INTERPOSER.dylib (it runs itself again with the interposer
 *   inserted); OUT=dir LIB=build tests/host/diagsig.sh. Exit 0 if all pass.
 */
#include "hao/runtime.h"
#include "hao/opts.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int *in_handler;
static unsigned *fmt_calls, *alloc_calls;
static const char *line;
static bool fatal;

static void handler(int sig)
{
    (void)sig;
    __atomic_store_n(in_handler, 1, __ATOMIC_RELAXED);
    (fatal ? hao_diag_write_fatal : hao_diag_write)(line, strlen(line));
    __atomic_store_n(in_handler, 0, __ATOMIC_RELAXED);
}

static void send(const char *l, bool f)
{
    line = l;
    fatal = f;
    raise(SIGUSR1);
}

static bool contains(const char *path, const char *what)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, what) != NULL;
}

static int fails;
static void check(bool ok, const char *name, const char *what)
{
    printf("%-9s %s  %s (formatting %u, allocation %u)\n", name, ok ? "PASS" : "FAIL", what, *fmt_calls, *alloc_calls);
    if (!ok) fails++;
    *fmt_calls = *alloc_calls = 0;
}

int main(int argc, char **argv)
{
    if (argc == 3) {   /* insert the counters (a timeout from /usr/bin drops DYLD_ variables): once */
        setenv("DYLD_INSERT_LIBRARIES", argv[2], 1);
        char *args[] = {argv[0], argv[1], NULL};
        execv(argv[0], args);
        perror("execv");
        return 2;
    }
    in_handler = dlsym(RTLD_DEFAULT, "diagsig_in_handler");
    fmt_calls = dlsym(RTLD_DEFAULT, "diagsig_fmt_calls");
    alloc_calls = dlsym(RTLD_DEFAULT, "diagsig_alloc_calls");
    if (argc < 2 || !in_handler || !fmt_calls || !alloc_calls) {
        printf("usage: diagsig DIR diagsig_interpose.dylib\n");
        return 2;
    }
    /* the counters see libhao's calls */
    __atomic_store_n(in_handler, 1, __ATOMIC_RELAXED);
    hao_log_line("%s", "diagsig: interposer probe\n");
    __atomic_store_n(in_handler, 0, __ATOMIC_RELAXED);
    bool counting = *fmt_calls > 0;
    check(counting, "probe", "the inserted counters see libhao's formatting");
    if (!counting) return 1;

    char parent[1024], dir[1100], log[1200], want[400];
    snprintf(parent, sizeof parent, "%s/later", argv[1]);
    snprintf(dir, sizeof dir, "%s/log", parent);
    snprintf(log, sizeof log, "%s/console.log", dir);
    snprintf(hao_opt.diag_dir, sizeof hao_opt.diag_dir, "%s", dir);
    hao_diag_init();   /* its parent is missing: the log waits */
    signal(SIGUSR1, handler);
    struct stat st;

    send("diagsig kept line one\n", false);
    mkdir(parent, 0700);
    send("diagsig kept line two\n", false);
    check(stat(dir, &st) != 0 && !*fmt_calls && !*alloc_calls, "kept", "hao_diag_write keeps lines, opens nothing");

    send("diagsig fatal line\n", true);
    bool opened = stat(log, &st) == 0;
    bool safe = !*fmt_calls && !*alloc_calls;
    check(opened && safe && contains(log, "hao: console.log of boot ") &&
          contains(log, " bytes of lines kept from before the Data volume was mounted follow\n") &&
          contains(log, "diagsig kept line one") && contains(log, "diagsig kept line two") && contains(log, "diagsig fatal line"),
          "fatal", "a fatal report opens the log: header, note and kept lines without formatting or allocation");

    if (stat(log, &st) == 0) {
        hao_diag_log_cap = (size_t)st.st_size + 2048;
        char big[600];
        memset(big, '.', sizeof big - 2);
        memcpy(big, "diagsig past the cap ", 21);
        big[sizeof big - 2] = '\n';
        big[sizeof big - 1] = 0;
        for (int k = 0; k < 8; k++) send(big, false);
        snprintf(want, sizeof want, "hao[%s:%d]: console.log reached its cap of %zu KB: later lines go to the console only\n",
                 getprogname(), (int)getpid(), hao_diag_log_cap >> 10);
        check(contains(log, want) && !*fmt_calls && !*alloc_calls, "cap", "the cap line without formatting or allocation");
    } else
        check(false, "cap", "no log to cap");
    return fails != 0;
}
