/* audit_token_t by value to native SPI (TCC, NetworkExtension, libsystem_sandbox)
 * and IOKit. arm64 passes the 32-byte token by reference (a pointer in the
 * next x register), x86_64 on the stack; a call through the plain register
 * bridge hands the native callee the pointer as its next argument and stack
 * garbage as the token. mds crashed this way in TCCAccessCheckAuditToken
 * (objc_msgSend on the token pointer taken as the options
 * dictionary); launchd signals processes through proc_signal_delegate (two
 * tokens by value, libproc.h). tests/guest/atoken.sh builds this for arm64 and arm64e and
 * compares hao (JIT, --interp) with native arm64. Queries only: no prompt,
 * no TCC or sandbox state is changed, and signal 0 delivers nothing. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CMTime.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* SPI. Prototypes from the callee's code in both slices */
extern Boolean TCCAccessCheckAuditToken(CFStringRef service, audit_token_t token, CFDictionaryRef options);
extern int TCCAccessPreflightWithAuditToken(CFStringRef service, audit_token_t token);
extern void *tcc_credential_create_for_process_with_audit_token(audit_token_t token);
extern const CFStringRef kTCCAccessCheckOptionPrompt;
extern uid_t NEUIDForAuditToken(audit_token_t token);
extern int sandbox_container_path_for_audit_token(audit_token_t token, char *buf, size_t size);
/* sandbox_check_network: the token's name carries no "AuditToken"/"token", so the earlier
 * by-name table missed it and launchd (PID 1) crashed under the plain register bridge: the shifted arguments made the native callee dereference the int `type` as the sockaddr. */
extern void *sandbox_checkattr_alloc(void);
extern void sandbox_checkattr_free(void *);
extern int sandbox_check_network(audit_token_t token, void *checkattr, const char *operation,
                                 int type, const struct sockaddr *addr);

/* Members a code-evidence scan over 26A428 found untyped, typed from
 * the callee's code in both slices (no header declares
 * any of them). Under the plain register bridge each lands its by-value aggregate in the wrong
 * place: arm64 passes a >16-byte struct by reference in the next x register, SysV copies it to
 * the stack, so x86_64 reads the guest's outgoing stack area as the aggregate and the pointer as
 * the next scalar. None of these crashes natively or under hao; they return wrong answers. */
/* typed-2.md s1: char *(const char *class, const char *path, uint32_t flags, audit_token_t) = ***IT.
 * arm64e reads [x3,#0x14]/[x3,#0x1c] (pid, pidversion), x86_64 0x24(%rbp)/0x2c(%rbp). */
extern char *sandbox_extension_issue_file_to_process(const char *cls, const char *path, uint32_t flags,
                                                     audit_token_t token);
/* consume: x86 `movq -0x20(%rbp),%rax` / arm `ldur x0,[x29,#-0x18]`, 64-bit handle, -1 with errno;
 * release: the handle in an 8-byte slot (movq %rdi / str x0), int result. Both slices, 26A428. */
extern int64_t sandbox_extension_consume(const char *ext);
extern int sandbox_extension_release(int64_t handle);
/* typed-10.md s8: void *(audit_token_t, int flags) = ^vTi; flags != 0 -> NULL, errno EINVAL in both
 * slices (cbz w19 / testl %ebx). responsibility_identity_release is a tail call to free in both. */
extern void *responsibility_get_attribution_for_audittoken(audit_token_t token, int flags);
extern void responsibility_identity_release(void *identity);
/* typed-5.md s5: int64_t (struct, struct) = q{?=QQQQ}{?=QQQQ}. 32 bytes of 8-byte words; words 0..2
 * compared unsigned (lo/hi; jbe/sbbq), word 3 copied by callers but never read. */
struct ls_version { uint64_t w[4]; };
extern int64_t _LSVersionNumberCompare(struct ls_version a, struct ls_version b);
/* typed-18.md s5: int32_t (CFDictionaryRef, CFStringRef, CMTime) = i^v^v{?=qiIq}; CMTime from CMTime.h
 * (24 bytes), read through x2 / at 0x10(%rbp), stored via CMTimeCopyAsDictionary. */
extern int32_t FigCFDictionarySetCMTime(CFMutableDictionaryRef dict, CFStringRef key, CMTime t);

static long long cmnum(CFDictionaryRef d, CFStringRef key)
{
    long long v = -99;
    CFNumberRef n = d ? CFDictionaryGetValue(d, key) : NULL;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    audit_token_t tok;
    mach_msg_type_number_t n = TASK_AUDIT_TOKEN_COUNT;
    if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&tok, &n) != KERN_SUCCESS) return 1;
    audit_token_t made = tok;
    made.val[1] = 4242;   /* euid */

    printf("NEUIDForAuditToken self=%d made=%u\n", NEUIDForAuditToken(tok) == geteuid(), NEUIDForAuditToken(made));

    /* the credential keeps a copy of the token at +0x40 in both slices */
    unsigned char *cred = tcc_credential_create_for_process_with_audit_token(made);
    printf("tcc_credential token copied=%d\n", cred && !memcmp(cred + 0x40, &made, sizeof made));

    const void *k = kTCCAccessCheckOptionPrompt, *v = kCFBooleanFalse;
    CFDictionaryRef opts = CFDictionaryCreate(NULL, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFStringRef svc = CFSTR("kTCCServiceListenEvent");
    printf("TCCAccessPreflightWithAuditToken=%d\n", TCCAccessPreflightWithAuditToken(svc, tok));
    printf("IOHIDAccessCheckAuditToken=%d\n", (int)IOHIDAccessCheckAuditToken(kIOHIDRequestTypeListenEvent, tok));

    errno = 0;
    int src = proc_signal_delegate(tok, tok, 0);
    printf("proc_signal_delegate self rc=%d errno=%d\n", src, src ? errno : 0);

    char path[1024] = "";
    errno = 0;
    int rc = sandbox_container_path_for_audit_token(tok, path, sizeof path);
    printf("sandbox_container_path_for_audit_token rc=%d errno=%d\n", rc, rc ? errno : 0);
    printf("TCCAccessCheckAuditToken=%d\n", (int)TCCAccessCheckAuditToken(svc, tok, opts));
    CFRelease(opts);

    /* last: without the typed route this is launchd's crash. The token (arg 0, by value) goes
     * by reference in x0 on arm64 and on the stack on x86_64, so the plain register bridge handed
     * checkattr/operation/type/sockaddr to the native callee one register early; it dereferenced
     * the small int `type` as the sockaddr pointer (movzbl 0x1(%r15)) and faulted. */
    void *ca = sandbox_checkattr_alloc();
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_len = sizeof sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(80);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    errno = 0;
    int nr = sandbox_check_network(tok, ca, "connect", 2, (struct sockaddr *)&sin);
    printf("sandbox_check_network rc=%d errno=%d\n", nr, nr ? errno : 0);
    sandbox_checkattr_free(ca);

    /* Scan members. logd issues read extensions to the `log show` client this way (without the
     * typed route, "sandbox_extension_consume failed: 1"): with the process's own token issue and consume
     * succeed; with pid 1 in the token the issue fails ESRCH (pidversion mismatch). Under the plain
     * bridge x86_64 reads pid/pidversion from the stack copy that is not there. */
    char tmpl[] = "/tmp/hao-atoken-XXXXXX";
    int tfd = mkstemp(tmpl);
    if (tfd >= 0) close(tfd);
    for (int v = 0; v < 2; v++) {
        audit_token_t t = tok;
        if (v == 1) t.val[5] = 1;
        errno = 0;
        char *ext = sandbox_extension_issue_file_to_process("com.apple.app-sandbox.read", tmpl, 0, t);
        int ie = errno;
        errno = 0;
        int64_t h = ext ? sandbox_extension_consume(ext) : -1;
        int ce = errno;
        printf("sandbox_extension_issue_file_to_process %s issued=%s errno=%d consumed=%s errno=%d\n",
               v ? "pid1" : "self", ext ? "yes" : "no", ext ? 0 : ie, h >= 0 ? "yes" : "no",
               ext && h < 0 ? ce : 0);
        if (h >= 0) sandbox_extension_release(h);
        free(ext);
    }
    if (tfd >= 0) unlink(tmpl);

    /* flags 0: an identity object for this process. flags 1: NULL, EINVAL, in both slices. Under
     * the plain bridge edi gets the low bits of the token pointer as `flags`: NULL, EINVAL. */
    for (int flags = 0; flags < 2; flags++) {
        errno = 0;
        void *attr = responsibility_get_attribution_for_audittoken(tok, flags);
        printf("responsibility_get_attribution_for_audittoken flags=%d result=%s errno=%d\n", flags,
               attr ? "non-NULL" : "NULL", attr ? 0 : errno);
        if (attr) responsibility_identity_release(attr);
    }

    /* two 32-byte structs by value: x0/x1 pointers on arm64, 64 bytes at 0x10(%rbp) on x86_64 */
    struct ls_version va = {{1, 2, 3, 0xdead}}, vb = {{1, 2, 4, 0xbeef}}, vc = {{1, 3, 0, 0}},
                      vd = {{2, 0, 0, 0}}, vhi = {{0x8000000000000000ull, 0, 0, 0}};
    printf("_LSVersionNumberCompare %lld %lld %lld %lld %lld %lld %lld %lld\n",
           (long long)_LSVersionNumberCompare(va, va), (long long)_LSVersionNumberCompare(va, vb),
           (long long)_LSVersionNumberCompare(vb, va), (long long)_LSVersionNumberCompare(va, vc),
           (long long)_LSVersionNumberCompare(vd, vc), (long long)_LSVersionNumberCompare(vc, vd),
           (long long)_LSVersionNumberCompare(vhi, vd), (long long)_LSVersionNumberCompare(vd, vhi));

    /* last: a 24-byte CMTime by value (x2 by reference / 0x10(%rbp)); the dictionary it stores
     * is read back with the public CMTime keys. CoreMedia needs no daemon for this. */
    CFMutableDictionaryRef cmd = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    CMTime ct = {.value = 90090, .timescale = 30000, .flags = kCMTimeFlags_Valid | kCMTimeFlags_HasBeenRounded,
                 .epoch = 7};
    int32_t frc = FigCFDictionarySetCMTime(cmd, CFSTR("time"), ct);
    CFDictionaryRef td = CFDictionaryGetValue(cmd, CFSTR("time"));
    printf("FigCFDictionarySetCMTime rc=%d stored=%d value=%lld timescale=%lld flags=%lld epoch=%lld\n", frc,
           td != NULL, cmnum(td, kCMTimeValueKey), cmnum(td, kCMTimeScaleKey), cmnum(td, kCMTimeFlagsKey),
           cmnum(td, kCMTimeEpochKey));
    CFRelease(cmd);
    return 0;
}
