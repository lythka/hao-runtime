#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__arm64__)
#include <mach/arm/thread_status.h>
#include <sys/ucontext.h>
extern const struct mach_header_64 _mh_execute_header;
extern void stop_at_marker(uint64_t);
extern const char guest_marker[], guest_after_marker[];
__asm__(".text\n.align 2\n.globl _stop_at_marker\n_stop_at_marker:\n"
        "mov x9, x0\nadd x10,x9,#1\nadd x11,x9,#2\ncmp x9,x9\n.globl _guest_marker\n_guest_marker:\nbrk #0\n"
        ".globl _guest_after_marker\n_guest_after_marker:\nret\n");
#endif

/* The caller launches this fixture directly, with a dedicated input pipe
 * and a dedicated output pipe. Both records and replies use native little endian. */
typedef struct {
    uint32_t version, architecture, pid, count;
    uint64_t tid, generation, image_base;
} header;
static header current;
#if defined(__arm64__)
static int corrupt_pc;
static uintptr_t wrong_mapping;
#endif
#if defined(__arm64__)
static sigjmp_buf resumed;
#endif

static int transfer(int fd, void *buffer, size_t length, int writing) {
    char *p = buffer;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        length -= (size_t)n;
    }
    return 1;
}
#if defined(__arm64__)
static void stopped(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    arm_thread_state64_t *regs = &uc->uc_mcontext->__ss;
    if (sig != SIGTRAP || arm_thread_state64_get_pc(*regs) != (uintptr_t)guest_marker) _exit(3);
    arm_thread_state64_t copy = *regs;
    if (corrupt_pc) arm_thread_state64_set_pc_fptr(copy, (void *)1);
    if (wrong_mapping) {
        uintptr_t offset = (uintptr_t)guest_marker - (uintptr_t)&_mh_execute_header;
        arm_thread_state64_set_pc_fptr(copy, (void *)(wrong_mapping + offset));
        current.image_base = wrong_mapping;
    }
    if (!transfer(1, &current, sizeof current, 1) ||
        !transfer(1, &copy, sizeof copy, 1)) _exit(4);
    uint64_t acknowledged;
    if (!transfer(0, &acknowledged, sizeof acknowledged, 0) || acknowledged != current.generation) _exit(5);
    /* End this authored stop and return to its generation loop. */
    siglongjmp(resumed, 1);
}
#endif
int main(void) {
    struct { uint64_t base, dev, ino; unsigned char uuid[16]; } image = {0};
#if defined(__arm64__)
    Dl_info loaded;
    struct stat file;
    if (!dladdr((const void *)guest_marker, &loaded) || !loaded.dli_fbase ||
        stat(loaded.dli_fname, &file) || loaded.dli_fbase != &_mh_execute_header) return 7;
    const struct mach_header_64 *mh = loaded.dli_fbase;
    const uint8_t *cmd = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const void *)cmd;
        if (lc->cmd == LC_UUID) memcpy(image.uuid, ((const struct uuid_command *)lc)->uuid, 16);
        cmd += lc->cmdsize;
    }
    image.dev = (uint32_t)file.st_dev;
    image.ino = file.st_ino;
    image.base = (uintptr_t)loaded.dli_fbase;
    if (getenv("FIXTURE_WRONG_MAPPING")) {
        void *mapping = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (mapping == MAP_FAILED) return 8;
        wrong_mapping = (uintptr_t)mapping;
    }
#endif
    if (!transfer(1, &image, sizeof image, 1)) return 4;
    if (getenv("FIXTURE_TRUNCATE")) {
        uint32_t prefix = 1;
        return !transfer(1, &prefix, sizeof prefix, 1);
    }
    current.version = getenv("FIXTURE_BAD_VERSION") ? 2 : 1;
    current.pid = getpid();
    if (pthread_threadid_np(NULL, &current.tid)) return 1;
#if defined(__arm64__)
    corrupt_pc = getenv("FIXTURE_CORRUPT_PC") != NULL;
    current.architecture = CPU_TYPE_ARM64;
    current.count = ARM_THREAD_STATE64_COUNT;
    current.image_base = (uintptr_t)&_mh_execute_header;
    struct sigaction action = {.sa_sigaction = stopped, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, NULL)) return 2;
    for (current.generation = 1; current.generation <= 2; current.generation++)
        if (!sigsetjmp(resumed, 1)) stop_at_marker(0x730 + current.generation);
#else
    current.architecture = CPU_TYPE_X86_64;
    if (!transfer(1, &current, sizeof current, 1)) return 4;
#endif
    return 0;
}
