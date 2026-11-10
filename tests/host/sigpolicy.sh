#!/bin/sh
# Native-only policy reproduction, independent of hao. Optional PROFILE must
# be a default-deny profile lacking SYS_sigreturn (e.g. WindowManager's).
# Imports in PROFILE resolve using the host unless they are absolute paths.
# OUT=dir [PROFILE=/path/profile.sb] [T=10] [HAO=/abs/build/hao] tests/host/sigpolicy.sh
# With HAO and PROFILE, also hao under that profile (sigpolicyguest.c, x86_64
# hardware only): an arm64e guest applies the profile to itself, then makes
# 200 calls that are each an NX fault on_fault redirects. The user-space
# resume (HAO_SIGRETURN_USER=1) must finish as native arm64 does (0), also
# when a sigreturn or __pthread_sigmask kills the process (kill); the kernel
# return (HAO_SIGRETURN_KERNEL=1) must die as on the target: the failed
# __sigreturn report and SIGTRAP (133), SIGKILL (137) with kill. fault: then
# a native write to address 16 that hao gives up on (signals.c
# hao_signal_giveup): with the user-space resume (the hardware default too)
# the process must die of that SIGSEGV (139) as natively, also with kill (the
# give-up needs no denied syscall); the kernel return must die as before the
# fix (the failed __sigreturn: SIGTRAP 133, SIGKILL 137 with kill). throw:
# then an NSException nothing catches out of a dispatch callout, where
# libunwind's call of the callout's personality faults and on_fault edits
# the context (unwind.c hao_eh_fault): with the user-space resume (and by
# default) it must end as native arm64 does, "Terminating app due to
# uncaught exception" and SIGABRT (134); the kernel return dies of the
# failed __sigreturn (133). Under Rosetta
# the hao part is skipped: there a plain x86_64 program under such a profile
# hangs in Rosetta's exception server at its first SIGSEGV (no hao involved).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-sigpolicy)}
T=${T:-10}
mkdir -p "$OUT"
# Select actual hardware even when this shell itself is under Rosetta.
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then a=arm64; else a=x86_64; fi
clang -arch "$a" -O1 -Wall -Wextra -Wno-unused-parameter -o "$OUT/sigpolicy" "$here/sigpolicy.c"
codesign -s - -f "$OUT/sigpolicy" 2>/dev/null
fail=0
check() {
    want=$1; shift
    rc=0
    timeout -s KILL "$T" "$OUT/sigpolicy" "$@" || rc=$?
    if [ "$rc" = "$want" ]; then result=PASS; else result=FAIL; fail=1; fi
    printf '%s %s: rc=%s expected=%s\n' "$result" "$*" "$rc" "$want"
}
check 14 raw none
check 0 handler none
check 1 raw deny
check 81 handler deny
if [ -n "${PROFILE:-}" ]; then
    check 1 raw "$PROFILE"
    check 81 handler "$PROFILE"
    check 14 raw "$PROFILE" allow
    check 0 handler "$PROFILE" allow
fi
[ -n "${PROFILE:-}" ] || echo "SKIP profile cases (PROFILE not set)"
if [ -n "${HAO:-}" ] && [ -n "${PROFILE:-}" ]; then
    if [ "$a" = arm64 ]; then
        echo "SKIP hao under $PROFILE: x86_64 under Rosetta takes no fault inside such a sandbox"
    else
        clang -arch x86_64 -arch arm64e -dynamiclib -O1 -install_name "$OUT/libsigpolicyhelper.dylib" \
            -o "$OUT/libsigpolicyhelper.dylib" "$here/sigpolicyhelper.c"
        clang -arch arm64e -O1 -Wall -o "$OUT/sigpolicyguest" "$here/sigpolicyguest.c" -L"$OUT" -lsigpolicyhelper \
            -framework Foundation
        codesign -s - -f "$OUT/libsigpolicyhelper.dylib" "$OUT/sigpolicyguest" 2>/dev/null
        rm -rf "$OUT/diag"; mkdir -p "$OUT/diag"
        hcheck() {   # want switch|default [kill] [fault]
            want=$1; sw=$2; shift 2
            rc=0
            swenv=; [ "$sw" = default ] || swenv=$sw=1
            env $swenv HAO_NO_AOT=1 HAO_DIAG_DIR="$OUT/diag" timeout -s KILL "$T" "$HAO" "$OUT/sigpolicyguest" "$PROFILE" 200 "$@" \
                >"$OUT/hao.$sw$*.out" 2>&1 || rc=$?
            if [ "$rc" = "$want" ]; then result=PASS; else result=FAIL; fail=1; fi
            printf '%s hao %s %s: rc=%s expected=%s\n' "$result" "$sw" "$*" "$rc" "$want"
        }
        hcheck 0 HAO_SIGRETURN_USER
        hcheck 0 HAO_SIGRETURN_USER kill
        hcheck 133 HAO_SIGRETURN_KERNEL
        hcheck 137 HAO_SIGRETURN_KERNEL kill
        hcheck 139 HAO_SIGRETURN_USER fault
        hcheck 139 HAO_SIGRETURN_USER kill fault
        hcheck 139 default fault
        hcheck 133 HAO_SIGRETURN_KERNEL fault
        hcheck 137 HAO_SIGRETURN_KERNEL kill fault
        hcheck 134 HAO_SIGRETURN_USER throw
        hcheck 134 default throw
        hcheck 133 HAO_SIGRETURN_KERNEL throw
        for sw in HAO_SIGRETURN_USER default; do
            if grep -q "Terminating app due to uncaught exception 'SigpolicyThrow'" "$OUT/hao.${sw}throw.out" 2>/dev/null; then
                echo "PASS hao $sw throw: the Terminating app line"
            else echo "FAIL hao $sw throw: no Terminating app line in $OUT/hao.${sw}throw.out"; fail=1; fi
        done
        if grep -q 'given up: SIG_DFL, the faulting instruction resumed in user space' "$OUT/diag/console.log" 2>/dev/null; then
            echo "PASS hao: the give-up resumed in user space"
        else echo "FAIL hao: no user-space give-up line in $OUT/diag/console.log"; fail=1; fi
        if grep -q 'failed __sigreturn sig=' "$OUT/diag/console.log" 2>/dev/null; then echo "PASS hao: the failed __sigreturn report"
        else echo "FAIL hao: no failed __sigreturn report in $OUT/diag/console.log"; fail=1; fi
    fi
fi
exit "$fail"
