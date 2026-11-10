#!/bin/sh
# Build tests/guest/eh.mm (+ the native helper libehhelper.dylib) for arm64e
# (guest) and arm64 (reference). Run every section on its own: natively,
# then under hao with the JIT, with --interp and with an AOT cache. A section
# passes when its stdout and exit status match native arm64. Fatal sections
# (20 and up) also compare the termination lines of stderr ("*** Terminating
# app due to uncaught exception ...", "libc++abi: terminating ...", instance
# addresses masked): the rest of their stderr (throw call stacks) differs by
# design.
# Sections 22-37 throw an exception that nothing catches into a libdispatch
# client callout or a CFRunLoop observer callout. Natively on arm64 that is
# std::terminate: SIGABRT, the reason logged, the uncaught-exception handler
# run. The x86_64 libdispatch that hao's native calls reach has a CIE whose
# personality slot points into the CIE itself, so native
# x86_64 dies of SIGSEGV there; hao must still behave as arm64.
# Sections in X86REF are compared with the native x86_64 build instead.
# The ehpers leg runs sections 22-37 again (JIT, --stats, the diagnostics
# log, whose abort report prints the "signal returns" counters) with
# HAO_SIGRETURN_USER=1 and HAO_SIGRETURN_KERNEL=1: each must still match
# native arm64, the context on_fault edits after that personality call
# (runtime.c, unwind.c hao_eh_fault) must be resumed in user space in the
# user run (ehpers user = the kernel run's ehpers kernel count, kernel 0) and
# through the kernel in the kernel run (ehpers user 0), and some section must
# take that path at all. EHPERS=0 skips it.
#   OUT=dir HAO=build/hao [T=30] [SECTIONS="1 2 .."] [MODES="jit interp aot"] [X86REF=""] [EHPERS=1] tests/guest/eh.sh
# Exit status: number of failing runs.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-eh)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
SECTIONS=${SECTIONS:-"1 2 3 4 5 6 7 8 9 10 11 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 all"}   # all: every non-fatal section in one process
MODES=${MODES:-"jit interp aot"}
X86REF=${X86REF-}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)   # the helper's install name is absolute
set -e
clang++ -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -std=c++17 -I"$here" \
    -install_name "$OUT/libehhelper.dylib" -o "$OUT/libehhelper.dylib" "$here/ehhelper.cpp"
codesign -s - -f "$OUT/libehhelper.dylib" 2>/dev/null
for a in arm64e arm64 x86_64; do
    clang++ -arch $a -O1 -std=c++17 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lehhelper \
        -o "$OUT/eh.$a" "$here/eh.mm"
    codesign -s - -f "$OUT/eh.$a" 2>/dev/null
done
# AOT leg: a private cache directory (owner-only, as hao's trust rules want)
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
case " $MODES " in *" aot "*)
    HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/eh.arm64e" >/dev/null 2>&1 ;;
esac
set +e
# the termination lines of a fatal run's stderr
term() { grep -E '^(\*\*\* Terminating app due to uncaught exception|libc\+\+abi: terminating)' "$1" | sed -E 's/0x[0-9a-fA-F]+/0x/g'; }
fail=0
for s in $SECTIONS; do
    ref=arm64
    case " $X86REF " in *" $s "*) ref=x86_64 ;; esac
    a=$s
    [ "$s" = all ] && a=
    ( timeout -s KILL "$T" "$OUT/eh.$ref" $a >"$OUT/s$s.native.out" 2>"$OUT/s$s.native.err"; exit $? ) 2>/dev/null
    nrc=$?
    echo $nrc >"$OUT/s$s.native.rc"
    fatal=0
    [ "$s" != all ] && [ "$s" -ge 20 ] && fatal=1
    [ $fatal = 1 ] && term "$OUT/s$s.native.err" >"$OUT/s$s.native.term"
    line="s$s native $ref rc=$nrc"
    for mode in $MODES; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        esac
        ( timeout -s KILL "$T" "$@" "$OUT/eh.arm64e" $a >"$OUT/s$s.$mode.out" 2>"$OUT/s$s.$mode.err"; exit $? ) 2>/dev/null
        rc=$?
        r=SAME
        if [ $rc != $nrc ] || ! cmp -s "$OUT/s$s.native.out" "$OUT/s$s.$mode.out"; then r=DIFF; fi
        if [ $fatal = 1 ]; then
            term "$OUT/s$s.$mode.err" >"$OUT/s$s.$mode.term"
            cmp -s "$OUT/s$s.native.term" "$OUT/s$s.$mode.term" || r=DIFF
        fi
        [ $r = DIFF ] && fail=$((fail + 1))
        line="$line | $mode rc=$rc $r"
    done
    echo "$line"
done
# the ehpers counters of a run's diagnostics log: "U K"
ehp() { sed -n 's/^hao: signal returns: .*ehpers \([0-9]*\)\/\([0-9]*\).*/\1 \2/p' "$1" 2>/dev/null | tail -1; }
hits=0
if [ "${EHPERS-1}" != 0 ]; then
    for s in $SECTIONS; do
        [ "$s" = all ] && continue
        [ "$s" -ge 22 ] && [ "$s" -le 37 ] || continue
        line="s$s ehpers"
        kk=
        for k in kern user; do
            case $k in user) sw=HAO_SIGRETURN_USER=1 ;; kern) sw=HAO_SIGRETURN_KERNEL=1 ;; esac
            d=$OUT/diag.s$s.$k; rm -rf "$d"; mkdir -p "$d"
            ( env $sw HAO_NO_AOT=1 HAO_DIAG_DIR="$d" timeout -s KILL "$T" "$HAO" --stats "$OUT/eh.arm64e" $s \
                >"$OUT/s$s.$k.out" 2>"$OUT/s$s.$k.err"; exit $? ) 2>/dev/null
            rc=$?
            nrc=$(cat "$OUT/s$s.native.rc" 2>/dev/null)
            set -- $(ehp "$d/console.log")
            u=${1:-0}; kn=${2:-0}
            r=SAME
            [ "$rc" = "$nrc" ] && cmp -s "$OUT/s$s.native.out" "$OUT/s$s.$k.out" || r=DIFF
            term "$OUT/s$s.$k.err" >"$OUT/s$s.$k.term"
            cmp -s "$OUT/s$s.native.term" "$OUT/s$s.$k.term" || r=DIFF
            case $k in
            kern) [ "$u" = 0 ] || r="$r (user-space resumes with the switch off)"; kk=$kn ;;
            user) [ "$kn" = 0 ] && [ "$u" = "$kk" ] || r="$r (ehpers user $u, kernel $kn; the kernel run: $kk)"
                  [ "$u" -gt 0 ] && hits=$((hits + 1)) ;;
            esac
            [ "$r" = SAME ] || fail=$((fail + 1))
            line="$line | $k rc=$rc ehpers=$u/$kn $r"
        done
        echo "$line"
    done
    case " $SECTIONS " in *" 2"[2-9]" "*|*" 3"[0-7]" "*)
        if [ $hits = 0 ]; then echo "ehpers: no section resumed libunwind in user space"; fail=$((fail + 1)); fi ;;
    esac
fi
echo "eh: $fail failing run(s)"
exit $fail
