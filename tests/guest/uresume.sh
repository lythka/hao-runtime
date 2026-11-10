#!/bin/sh
# The contexts hao's fault handler resumes (runtime.c on_fault: the NX
# redirect of a native call into guest code to hao_callback_entry, a native
# call of a trap address; signals.c: a fault forwarded to a native handler
# that returns) must be resumed exactly: tests/guest/uresume.cpp (+ the
# native helper uresumehelper.cpp/.S, x86_64 + arm64e for the guest, arm64
# for the reference). Each mode runs natively (arm64) and under hao with the
# JIT, --interp and an AOT cache, each twice: HAO_SIGRETURN_USER=1 (the
# user-space resume, which is off by default under Rosetta) and
# HAO_SIGRETURN_KERNEL=1 (the kernel's __sigreturn). stdout and the exit
# status must match the native run; every mode but fwd must take the NX
# path (HAO_NX_STATS: native->guest or native->trap faults > 0); with a hao
# that has the resume counters ("hao: signal returns: U user space, K
# kernel"), U must be > 0 in the user runs of the NX modes and 0 in every
# kernel run. A hang is killed after T seconds (rc 137).
# The stats leg: state with --stats prints the counters, user space > 0
# with HAO_SIGRETURN_USER=1 and 0 with HAO_SIGRETURN_KERNEL=1.
# The fault resumes are on_fault's (no SA_ONSTACK), so the altstack mode
# checks a native sigaltstack's survival, not the on-stack fallback. The
# commpage emulation is not reachable here: under Rosetta the arm64 commpage
# address is Rosetta's own, mapped, and hao does not emulate it.
# alarmh under Rosetta: every fault of that mode happens while the guest's
# SIGALRM handler runs (inside tramp), and with HAO_SIGRETURN_USER=1 a fault
# inside a handler goes back through the kernel under Rosetta (kernel reason
# `nested`: Rosetta keeps one frame per delivery until its __sigreturn), so
# its user runs need exactness only, not user-space resumes.
# Expected failures are listed in EXPECT (modes): the script fails when a
# mode outside EXPECT fails, and when one in EXPECT passes in every run
# (remove it from EXPECT in the commit that fixes it).
#   OUT=dir HAO=/abs/build/hao [T=60] [MODES="state trap ..."] [VARIANTS="jit interp aot"] [EXPECT="alarm .."] tests/guest/uresume.sh
# Exit status: 0 when the results match EXPECT.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-uresume}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
MODES=${MODES:-state trap mask alarm alarmnx alarmh nest eh threads nthreads altstack fwd stats}
VARIANTS=${VARIANTS:-jit interp aot}
EXPECT=${EXPECT-""}
set -e
# two helpers: the guest's (x86_64, which hao binds, and arm64e to link
# against) and the reference's (arm64 alone, as tests/guest/nativesig.sh)
mkdir -p "$OUT/g" "$OUT/arm"
clang++ -arch x86_64 -arch arm64e -dynamiclib -O1 -std=c++17 -I"$here" \
    -install_name "$OUT/g/liburesumehelper.dylib" -o "$OUT/g/liburesumehelper.dylib" \
    "$here/uresumehelper.cpp" "$here/uresumehelper.S"
clang++ -arch arm64 -dynamiclib -O1 -std=c++17 -I"$here" \
    -install_name "$OUT/arm/liburesumehelper.dylib" -o "$OUT/arm/liburesumehelper.dylib" \
    "$here/uresumehelper.cpp" "$here/uresumehelper.S"
codesign -s - -f "$OUT/g/liburesumehelper.dylib" "$OUT/arm/liburesumehelper.dylib" 2>/dev/null
clang++ -arch arm64e -O1 -std=c++17 -I"$here" -L"$OUT/g" -luresumehelper -o "$OUT/uresume.arm64e" "$here/uresume.cpp"
clang++ -arch arm64 -O1 -std=c++17 -I"$here" -L"$OUT/arm" -luresumehelper -o "$OUT/uresume.arm64" "$here/uresume.cpp"
codesign -s - -f "$OUT/uresume.arm64e" "$OUT/uresume.arm64" 2>/dev/null
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
case " $VARIANTS " in *" aot "*)
    HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/uresume.arm64e" >/dev/null 2>&1 ;;
esac
set +e
# the NX fault counts of a run: "N M" (native->guest, native->trap)
nx() { sed -n 's/^hao: nx faults: \([0-9]*\) native->guest, \([0-9]*\) native->trap$/\1 \2/p' "$1" | tail -1; }
# the resume counters: "U K" (user space, kernel), empty when not printed
res() { sed -n 's/^hao: signal returns: \([0-9]*\) user space, \([0-9]*\) kernel .*/\1 \2/p' "$1" | tail -1; }
counters=0
grep -q 'signal returns:' "$(dirname "$HAO")/libhao.dylib" 2>/dev/null && counters=1
[ $counters = 0 ] && echo "($HAO has no resume counters: they are not checked)"
# hao is x86_64: on an arm64 host it runs under Rosetta
rosetta=0
[ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] && rosetta=1
# a mode's verdict against EXPECT, from nfail (its failed runs)
verdict() {
    case " $EXPECT " in *" $m "*) exp=1 ;; *) exp=0 ;; esac
    if [ $exp = 1 ] && [ $nfail = 0 ]; then echo "$m: NOW PASSES (remove from EXPECT)"; bad=$((bad + 1)); fi
    if [ $exp = 0 ] && [ $nfail != 0 ]; then bad=$((bad + 1)); fi
    if [ $exp = 1 ] && [ $nfail != 0 ]; then echo "$m: $nfail failing run(s), expected (EXPECT)"; fi
}
bad=0
for m in $MODES; do
    nfail=0
    if [ $m = stats ]; then
        for k in user kern; do
            case $k in user) sw=HAO_SIGRETURN_USER=1 ;; kern) sw=HAO_SIGRETURN_KERNEL=1 ;; esac
            env HAO_NO_AOT=1 $sw timeout -s KILL "$T" "$HAO" --stats "$OUT/uresume.arm64e" state \
                >"$OUT/stats.$k.out" 2>"$OUT/stats.$k.err"
            rc=$?
            set -- $(res "$OUT/stats.$k.err")
            u=${1:--}; kk=${2:--}
            r=DIFF
            if [ $rc = 0 ] && [ "$u" != - ]; then
                [ $k = user ] && [ "$u" -gt 0 ] && r=SAME
                [ $k = kern ] && [ "$u" = 0 ] && r=SAME
            fi
            [ $r = DIFF ] && nfail=$((nfail + 1))
            printf '%-9s %-12s hao rc=%-3s resumes user=%-6s kernel=%-6s %s\n' stats "$k" "$rc" "$u" "$kk" "$r"
        done
        verdict
        continue
    fi
    timeout -s KILL "$T" "$OUT/uresume.arm64" $m >"$OUT/$m.native.out" 2>"$OUT/$m.native.err"
    nrc=$?
    for v in $VARIANTS; do
        for k in user kern; do
            case $v in
            jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
            interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
            aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
            esac
            case $k in user) sw=HAO_SIGRETURN_USER=1 ;; kern) sw=HAO_SIGRETURN_KERNEL=1 ;; esac
            tag=$v-$k
            env HAO_NX_STATS=1 $sw timeout -s KILL "$T" "$@" "$OUT/uresume.arm64e" $m >"$OUT/$m.$tag.out" 2>"$OUT/$m.$tag.err"
            rc=$?
            set -- $(nx "$OUT/$m.$tag.err")
            g=${1:-0}; tr=${2:-0}
            set -- $(res "$OUT/$m.$tag.err")
            u=${1:--}; kk=${2:--}
            r=SAME
            [ $rc = $nrc ] && cmp -s "$OUT/$m.native.out" "$OUT/$m.$tag.out" || r=DIFF
            [ $m != fwd ] && [ $((g + tr)) = 0 ] && r="$r (no NX fault)"
            if [ $counters = 1 ] && [ $rc = 0 ]; then
                case $k in
                user) [ $m != fwd ] && ! [ $m = alarmh -a $rosetta = 1 ] && [ "${u%-}" = "" -o "$u" = 0 ] &&
                    r="$r (no user-space resume)" ;;
                kern) [ "$u" != - ] && [ "$u" != 0 ] && r="$r (user-space resumes with the switch off)" ;;
                esac
            fi
            [ "$r" = SAME ] || nfail=$((nfail + 1))
            printf '%-9s %-12s native rc=%-3s hao rc=%-3s nx=%-5s trap=%-4s resumes user=%-5s kernel=%-5s %s\n' \
                "$m" "$tag" "$nrc" "$rc" "$g" "$tr" "$u" "$kk" "$r"
        done
    done
    verdict
done
[ $bad = 0 ] && echo "uresume: PASS (expected failures: ${EXPECT:-none})" || echo "uresume: FAIL ($bad mode(s) unexpected)"
[ $bad = 0 ]
