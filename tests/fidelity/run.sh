#!/bin/bash
# The process fidelity oracle's own oracle (P23): fixture programs and TSVs with
# KNOWN differences, so the run fails if tools/fidelity/observe or
# tools/fidelity/diff.py miss a difference or invent one.
#   HAO=/abs/build/hao OBSERVE=/abs/observe DIFF=/abs/diff.py OUT=dir tests/fidelity/run.sh
#   [KNOWN=known.tsv (default: beside DIFF)] [EMBED=tools/hao-embed] [STUB=hao-stub beside HAO] [T=60]
# tests/fidelity/target.c (modes a and b, four deliberate differences) runs as
# native arm64, under hao (CLI route) and embedded (hao-embed --host-test); each
# running process is observed from outside and the observations are diffed.
# Cases:
#  - the observer records the state target.c set (dispositions, nice, darwinbg,
#    process group, fds; rlimit and signal mask as unreadable: xnu has no
#    interface for them) on every route;
#  - native a vs native a, hao a vs hao a: no DIFF (nothing that changes
#    between runs is recorded); native a vs a copy with one more p_sigignore
#    bit: exactly that key DIFFs, exit 1;
#  - native a vs native b: exactly the deliberate keys (expect-ab.txt) DIFF;
#  - native a vs hao a (CLI route, and embedded by hao-embed --host-test against
#    native a launched through arch(1) too) with KNOWN: nice, darwinbg, rlimit,
#    process group and the program's fds SAME; every signal disposition SAME but
#    SIGSEGV/SIGBUS, which KNOWN must explain; every remaining difference is
#    printed as a NOTE line (explained or UNEXPLAINED), not judged;
#  - native a vs hao b: KNOWN explains none of the deliberate keys away;
#  - the differ on hand-written TSVs (fixtures/): other pids and row order, one
#    more signal bit, a key on one side only, UTF-8 and escaped tabs in values,
#    a program on one side only (an unexplained difference: exit 1), known
#    entries without a reason or with a bad regex (exit 2), value and status
#    matching of known entries, malformed observations (exit 2, never 1).
# Exit 1 if any case failed, 2 if a fixture could not be built.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
HAO=${HAO:-$repo/build/hao}
OBSERVE=${OBSERVE:-$repo/build/fidelity/observe}
DIFF=${DIFF:-$repo/tools/fidelity/diff.py}
KNOWN=${KNOWN:-$(dirname "$DIFF")/known.tsv}
EMBED=${EMBED:-$repo/tools/hao-embed}
STUB=${STUB:-$(dirname "$HAO")/hao-stub}
OUT=${OUT:-$(mktemp -d -t hao-fidelity)}
T=${T:-60}
fails=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
note() { echo "NOTE $*"; }
[ -e "$HAO" ] || { echo "missing $HAO" >&2; exit 2; }
# The observer and diff tool are not part of this repository: without them the
# run is a skip, said as one.
for f in "$OBSERVE" "$DIFF"; do [ -e "$f" ] || { echo "SKIP fidelity oracle: no $f (set OBSERVE and DIFF)"; exit 77; }; done
rm -rf "$OUT"; mkdir -p "$OUT/native" "$OUT/guest" "$OUT/emb" "$OUT/fx"
OUT=$(cd "$OUT" && pwd -P)   # proc_pidpath and fd paths are real paths
# one file name on every route, so the observed program is the same; one signing
# identifier (an ad hoc default one embeds the slice's LC_UUID, and the arm64
# reference and the arm64e guest are different builds)
ID=com.example.hao.fidelity.target
clang -arch arm64 -O1 -Wall -o "$OUT/native/target" "$here/target.c" &&
    codesign -s - -f -i $ID "$OUT/native/target" 2>/dev/null || exit 2
clang -arch arm64e -O1 -Wall -o "$OUT/guest/target" "$here/target.c" &&
    codesign -s - -f -i $ID "$OUT/guest/target" 2>/dev/null || exit 2
emb=1
python3 "$EMBED" --host-test --stub "$STUB" -o "$OUT/emb/target" "$OUT/guest/target" >"$OUT/embed.log" 2>&1 &&
    [ -f "$OUT/emb/target" ] || { emb=0; note "hao-embed failed, embedded cases skipped: $(head -c 300 "$OUT/embed.log")"; }

# observe NAME MODE BINARY [LAUNCHER...]: copy BINARY to OUT/run/target, start
# LAUNCHER... OUT/run/target MODE T OUT/run/target.data from OUT/run (every
# route has the same path, argv, cwd and fds), wait for its "ready PID", observe
# that pid into OUT/NAME.tsv, kill it. Returns 1 if it never got ready.
R=$OUT/run
observe() {
    local name=$1 mode=$2 bin=$3 pid="" i; shift 3
    rm -rf "$R"; mkdir -p "$R"; cp "$bin" "$R/target"
    (cd "$R" && exec timeout -s KILL "$T" "$@" "$R/target" "$mode" "$T" "$R/target.data" \
        >"$R/out" 2>"$R/err" </dev/null) &
    local w=$!
    for i in $(seq 1 600); do
        pid=$(awk '/^ready [0-9]+$/ {print $2; exit}' "$R/out")
        [ -n "$pid" ] && break
        kill -0 $w 2>/dev/null || break
        sleep 0.1
    done
    if [ -z "$pid" ]; then
        kill -KILL $w 2>/dev/null; wait $w 2>/dev/null
        bad "$name: target never got ready: $(head -c 300 "$R/err")"; return 1
    fi
    timeout -s KILL 60 "$OBSERVE" "$pid" >"$OUT/$name.tsv" 2>"$OUT/$name.obs.err"
    local rc=$?
    kill -KILL "$pid" 2>/dev/null; wait $w 2>/dev/null
    cp "$R/err" "$OUT/$name.err"
    if [ $rc != 0 ] || ! grep -q "^$pid	" "$OUT/$name.tsv"; then
        bad "$name: observe exit $rc, no rows for pid $pid: $(head -c 300 "$OUT/$name.obs.err")"; return 1
    fi
    return 0
}
# val NAME CLASS KEY: the observed value (rows: pid exe label class key value)
val() { awk -F'\t' -v c="$2" -v k="$3" '$1 ~ /^[0-9]+$/ && $4 == c && $5 == k {print $6; exit}' "$OUT/$1.tsv"; }
# want NAME CLASS KEY VALUE...: the observer recorded KEY with VALUE ("" = no row)
want() {
    local name=$1 c=$2 k=$3 v=$4 got
    got=$(val "$name" "$c" "$k")
    if [ "$got" = "$v" ]; then return 0; fi
    bad "$name: observer: $c $k is '${got}', the program set '${v}'"; return 1
}
# the state target.c set in MODE, as the observer must record it
check_state() {
    local name=$1 m=$2 ok=1 s   # check_state NAME MODE [hao]
    if [ "$m" = a ]; then want "$name" signal sig.SIGUSR1 ignore || ok=0
    else want "$name" signal sig.SIGUSR1 default || ok=0; fi
    want "$name" signal sig.SIGTERM catch || ok=0
    want "$name" signal sig.SIGUSR2 catch || ok=0
    want "$name" signal sig.SIGPIPE ignore || ok=0
    for s in SIGHUP SIGINT SIGQUIT SIGABRT SIGALRM; do want "$name" signal sig.$s default || ok=0; done
    # under hao, SIGSEGV/SIGBUS are on_fault's (the diff cases check the known reason)
    [ "${3:-}" = hao ] || for s in SIGSEGV SIGBUS; do want "$name" signal sig.$s default || ok=0; done
    # xnu sets p_sigignore for a SIG_DFL signal whose default action is to
    # ignore it, so from outside these read "ignore" (SIG_DFL and SIG_IGN alike)
    for s in SIGCHLD SIGWINCH SIGINFO SIGURG; do want "$name" signal sig.$s ignore || ok=0; done
    want "$name" signal sigmask.main_thread "unreadable(no-interface)" || ok=0
    want "$name" resource rlimit "unreadable(no-interface)" || ok=0
    want "$name" resource nice "$([ "$m" = a ] && echo 5 || echo 7)" || ok=0
    want "$name" resource procflag:PROC_FLAG_DARWINBG set || ok=0
    # getpriority(PRIO_DARWIN_PROCESS, other pid) reads the external policy only
    # (xnu get_background_proc), so it says 0 for a process that put itself in
    # the background: a "darwinbg" key must not report that as "no"
    [ "$(val "$name" resource darwinbg)" = no ] &&
        { bad "$name: observer: darwinbg 'no' for a process in darwinbg (PROC_FLAG_DARWINBG set; the external policy only)"; ok=0; }
    want "$name" identity pgrp.leader "$([ "$m" = a ] && echo yes || echo no)" || ok=0
    want "$name" identity pgrp.same_as_parent "$([ "$m" = a ] && echo no || echo yes)" || ok=0
    want "$name" fd "fd.path:$R/target.data" 1 || ok=0
    want "$name" fd fd.count.pipe 2 || ok=0
    want "$name" fd fd.count.socket "$([ "$m" = a ] && echo 1 || echo)" || ok=0
    want "$name" fd "fd.socket:unix/stream:-" "$([ "$m" = a ] && echo 1 || echo)" || ok=0
    want "$name" fd fd.0 "vnode:/dev/null" || ok=0
    [ $ok = 1 ] && pass "$name: observer records the state the program set (mode $m)"
}

# differ NAME REF HAO [diff.py options]: diff.py --out OUT/NAME; exit status in $drc.
# Without a --known option the empty known file is used, so nothing is explained away.
differ() {
    local name=$1 ref=$2 hao=$3; shift 3
    local k=(--known "$EMPTY")
    case " $* " in *" --known "*) k=() ;; esac
    rm -rf "$OUT/$name"
    python3 "$DIFF" "$ref" "$hao" ${k[@]+"${k[@]}"} "$@" --out "$OUT/$name" >"$OUT/$name.log" 2>&1
    drc=$?
}
# rows NAME: OUT/NAME/diff.tsv without its header
# (program ref_pid hao_pid class key status ref_value hao_value known)
rows() { tail -n +2 "$OUT/$1/diff.tsv" 2>/dev/null; }
# notsame NAME: "class<TAB>key<TAB>status" of every difference (with or without a known reason)
notsame() { rows "$1" | awk -F'\t' '$6 != "SAME" {print $4 "\t" $5 "\t" $6}' | sort; }
# unexplained NAME: the same, only those with no known reason
unexplained() { rows "$1" | awk -F'\t' '$6 ~ /^(DIFF|MISSING-REF|MISSING-HAO)$/ && ($9 == "" || $9 == "-") {print $4 "\t" $5 "\t" $6}' | sort; }
# status NAME CLASS KEY: the status diff.py gave KEY ("" if not compared)
status() { rows "$1" | awk -F'\t' -v c="$2" -v k="$3" '$4 == c && $5 == k {print $6; exit}'; }
# reason NAME CLASS KEY: its known reason on any program ("-" if none)
reason() { rows "$1" | awk -F'\t' -v c="$2" -v k="$3" '$4 == c && $5 == k && $9 != "" && $9 != "-" {r = $9; exit} END {print (r == "" ? "-" : r)}'; }
flat() { awk -F'\t' '{$1 = $1; printf "%s%s", (NR > 1 ? "; " : ""), $0}'; }
expect_ab() { grep -v '^#' "$here/expect-ab.txt" | grep . | sort; }
EMPTY=$OUT/empty-known.tsv
: >"$EMPTY"

echo "== observer: target.c's state, native arm64 and under hao"
ok_n1=0 ok_n2=0 ok_nb=0 ok_na=0 ok_h=0 ok_h2=0 ok_hb=0 ok_e=0
observe native-a1 a "$OUT/native/target" && ok_n1=1 && check_state native-a1 a
observe native-a2 a "$OUT/native/target" && ok_n2=1 && check_state native-a2 a
observe native-b b "$OUT/native/target" && ok_nb=1 && check_state native-b b
observe hao-a a "$OUT/guest/target" "$HAO" && ok_h=1 && check_state hao-a a hao
observe hao-a2 a "$OUT/guest/target" "$HAO" && ok_h2=1 && check_state hao-a2 a hao
observe hao-b b "$OUT/guest/target" "$HAO" && ok_hb=1 && check_state hao-b b hao
# the embedded route needs arch(1), whose CoreFoundation rewrites
# __CF_USER_TEXT_ENCODING before its exec: its reference goes through arch too
observe native-arch-a a "$OUT/native/target" arch -arm64 && ok_na=1 && check_state native-arch-a a
[ $emb = 1 ] && observe emb-a a "$OUT/emb/target" arch -x86_64 && ok_e=1 && check_state emb-a a hao
# a broken observer (drops the socket, loses SIGUSR1's ignore bit) must fail check_state
cat >"$OUT/broken-observe" <<SH
#!/bin/sh
"$OBSERVE" "\$@" | awk -F'\t' 'BEGIN {OFS = FS} \$5 == "fd.socket:unix/stream:-" {next} \$5 == "sig.SIGUSR1" {\$6 = "default"} {print}'
SH
chmod +x "$OUT/broken-observe"
if [ $ok_n1 = 1 ]; then
    o=$OBSERVE; OBSERVE=$OUT/broken-observe
    observe broken-a a "$OUT/native/target" && {
        f0=$fails; c=$(check_state broken-a a | grep '^FAIL')
        if echo "$c" | grep -q "sig.SIGUSR1 is 'default'" && echo "$c" | grep -q "fd.socket:unix/stream:- is ''"; then
            pass "a broken observer (socket dropped, SIGUSR1 bit lost) fails the state check"
        else bad "a broken observer passed the state check: $(echo "$c" | flat)"; fi
        fails=$f0
    }
    OBSERVE=$o
fi

echo "== differ on observed processes"
if [ $ok_n1 = 1 ] && [ $ok_n2 = 1 ]; then
    differ stable "$OUT/native-a1.tsv" "$OUT/native-a2.tsv"
    d=$(notsame stable)
    n=$(rows stable | wc -l | tr -d ' ')
    if [ $drc = 0 ] && [ -z "$d" ] && [ "$n" -gt 50 ]; then pass "native a twice: $n keys, all SAME (nothing that changes between runs is recorded)"
    else bad "native a twice: exit $drc, $n keys, not SAME: $(echo "$d" | flat)"; fi
    # one more bit in p_sigignore, as a broken observation would have it
    awk -F'\t' 'BEGIN {OFS = FS} $4 == "signal" && $5 == "sig.SIGHUP" {$6 = "ignore"} {print}' \
        "$OUT/native-a2.tsv" >"$OUT/native-a2-extrabit.tsv"
    differ extrabit-observed "$OUT/native-a1.tsv" "$OUT/native-a2-extrabit.tsv"
    d=$(notsame extrabit-observed)
    if [ $drc = 1 ] && [ "$d" = "$(printf 'signal\tsig.SIGHUP\tDIFF')" ]; then pass "native a vs a copy with SIGHUP ignored: exit 1, exactly sig.SIGHUP DIFF"
    else bad "native a vs SIGHUP-ignored copy: exit $drc, differences: $(echo "$d" | flat)"; fi
fi
if [ $ok_h = 1 ] && [ $ok_h2 = 1 ]; then
    differ stable-hao "$OUT/hao-a.tsv" "$OUT/hao-a2.tsv"
    d=$(notsame stable-hao)
    [ $drc = 0 ] && [ -z "$d" ] && pass "hao a twice: $(rows stable-hao | wc -l | tr -d ' ') keys, all SAME" ||
        bad "hao a twice: exit $drc, not SAME: $(echo "$d" | flat)"
fi
if [ $ok_n1 = 1 ] && [ $ok_nb = 1 ]; then
    differ modes "$OUT/native-a1.tsv" "$OUT/native-b.tsv"
    d=$(notsame modes)
    e=$(expect_ab)
    if [ $drc = 1 ] && [ "$d" = "$e" ]; then pass "native a vs native b: exit 1, exactly the $(echo "$e" | wc -l | tr -d ' ') deliberate keys differ"
    else
        bad "native a vs native b: exit $drc; missed: $(comm -13 <(echo "$d") <(echo "$e") | flat); invented: $(comm -23 <(echo "$d") <(echo "$e") | flat)"
    fi
    for k in "signal	sigmask.main_thread" "resource	rlimit"; do
        st=$(status modes "${k%%	*}" "${k#*	}")
        [ "$st" = SAME ] && pass "native a vs b: ${k#*	} differs but has no outside interface: SAME (unreadable on both sides, no invented value)" ||
            bad "native a vs b: ${k#*	} is '$st', want SAME (unreadable on both sides)"
    done
fi

# fidelity NAME ROUTE REF_TSV HAO_TSV [ALIAS]: native a vs hao a with the builder's known list,
# judged as ROUTE (cli: hao's own identity is known by design; embedded: every class)
fidelity() {
    local name=$1 route=$2 ref=$3 h=$4 al=${5:-} k ok=1 st rs
    [ -f "$KNOWN" ] || { bad "$name: no known list $KNOWN"; return; }
    differ "$name" "$ref" "$h" --known "$KNOWN" --route "$route" ${al:+--alias "$al"}
    if ! grep -q "	MATCHED	" "$OUT/$name/programs.tsv" 2>/dev/null; then
        bad "$name: diff.py exit $drc, the program was not matched: $(head -c 300 "$OUT/$name.log")"; return
    fi
    # what the program set itself: hao must not change it
    for k in "resource	nice" "resource	procflag:PROC_FLAG_DARWINBG" "resource	rlimit" \
             "identity	pgrp.leader" "identity	pgrp.same_as_parent" \
             "fd	fd.0" "fd	fd.1" "fd	fd.2" "fd	fd.path:$R/target.data" "fd	fd.count.pipe" \
             "fd	fd.count.socket" "fd	fd.socket:unix/stream:-"; do
        st=$(status "$name" "${k%%	*}" "${k#*	}")
        [ "$st" = SAME ] || { bad "$name: ${k#*	}: '$st', the program set it and hao must keep it"; ok=0; }
    done
    [ $ok = 1 ] && pass "$name: nice, darwinbg, rlimit, process group and the program's fds are SAME"
    # signal dispositions: SAME but for on_fault's SIGSEGV/SIGBUS, which KNOWN must give a reason
    ok=1
    while IFS=$'\t' read -r k st rs; do
        case $k in
            sig.SIGSEGV|sig.SIGBUS)
                if [ "$st" != DIFF ] || [ -z "$rs" ] || [ "$rs" = - ]; then bad "$name: $k $st without a known reason ('$rs')"; ok=0; fi ;;
            *) [ "$st" = SAME ] || { bad "$name: signal $k: $st"; ok=0; } ;;
        esac
    done < <(rows "$name" | awk -F'\t' '$4 == "signal" {print $5 "\t" $6 "\t" $9}')
    for k in sig.SIGSEGV sig.SIGBUS sig.SIGUSR1 sig.SIGTERM sig.SIGUSR2 sig.SIGPIPE; do
        [ -n "$(status "$name" signal $k)" ] || { bad "$name: signal $k not compared"; ok=0; }
    done
    [ $ok = 1 ] && pass "$name: every signal disposition SAME but SIGSEGV/SIGBUS (catch, known: $(reason "$name" signal sig.SIGSEGV))"
    note "$name: diff.py exit $drc: $(tail -1 "$OUT/$name.log")"
    rows "$name" | awk -F'\t' -v n="$name" '$6 != "SAME" {printf "NOTE %s:   %s %s %s: %s -> %s [%s]\n", n, $4, $6, $5, $7, $8, ($9 == "" || $9 == "-" ? "UNEXPLAINED" : "known: " $9)}' | cut -c1-400
}
echo "== native arm64 vs hao (known list $KNOWN)"
HREAL=$(cd "$(dirname "$HAO")" && pwd -P)/$(basename "$HAO")
[ $ok_n1 = 1 ] && [ $ok_h = 1 ] && fidelity cli cli "$OUT/native-a1.tsv" "$OUT/hao-a.tsv" "$HREAL=$R/target"
[ $ok_na = 1 ] && [ $ok_e = 1 ] && fidelity embedded embedded "$OUT/native-arch-a.tsv" "$OUT/emb-a.tsv"
# the known list must not explain the deliberate differences away
if [ $ok_n1 = 1 ] && [ $ok_hb = 1 ] && [ -f "$KNOWN" ]; then
    differ cli-modes "$OUT/native-a1.tsv" "$OUT/hao-b.tsv" --known "$KNOWN" --route cli --alias "$HREAL=$R/target"
    # fd.count is a quantity KNOWN may call activity-dependent; the socket's
    # presence keys (fd.count.socket, fd.socket:...) carry that difference
    miss=$(comm -13 <(unexplained cli-modes) <(expect_ab | grep -v "^fd	fd.count	") | flat)
    [ $drc = 1 ] && [ -z "$miss" ] && pass "native a vs hao b: every deliberate key is an unexplained difference (exit 1)" ||
        bad "native a vs hao b: exit $drc, deliberate keys not reported: $miss"
fi

echo "== differ on hand-written observations (tests/fidelity/fixtures)"
FX=$here/fixtures
# fx NAME HAO_FIXTURE WANT_EXIT WANT_DIFFERENCES [diff.py options]: vs fixtures/ref.tsv
fx() {
    local name=$1 h=$2 want=$3 wd=$4 d; shift 4
    differ "fx-$name" "$FX/ref.tsv" "$FX/$h" "$@"
    d=$(notsame "fx-$name" | flat)
    if [ $drc = "$want" ] && [ "$d" = "$wd" ]; then pass "fixture $name: exit $drc${wd:+, $wd}"; return 0; fi
    bad "fixture $name: exit $drc (want $want), differences '$d' (want '$wd'): $(tail -1 "$OUT/fx-$name.log" | cut -c1-200)"
    return 1
}
n=$(($(grep -c . "$FX/ref.tsv") - 2))
fx same same.tsv 0 "" && { [ "$(rows fx-same | wc -l | tr -d ' ')" = $n ] &&
    pass "fixture same: other pids and row order, all $n keys compared" || bad "fixture same: $(rows fx-same | wc -l | tr -d ' ') keys compared, want $n"; }
fx extrabit extrabit.tsv 1 "signal sig.SIGHUP DIFF"
fx onekey onekey.tsv 1 "fd fd.socket:inet/dgram MISSING-REF; fd fd.socket:unix/stream:- MISSING-HAO"
fx utf8 utf8.tsv 1 "identity env:LANG DIFF" &&
    { rows fx-utf8 | awk -F'\t' '$5 == "env:LANG"' | grep -q "日本	fr_FR.UTF-8 été 日末" &&
        pass "fixture utf8: both UTF-8 values kept as written" || bad "fixture utf8: values: $(rows fx-utf8 | awk -F'\t' '$5 == "env:LANG"')"; }
fx tab tab.tsv 1 "identity argv.1 DIFF" &&
    { [ "$(rows fx-tab | awk -F'\t' '$5 == "argv.1" {print $7 "|" $8}')" = 'a\tb|a b' ] &&
        pass "fixture tab: an escaped tab stays one field, written escaped" || bad "fixture tab: $(rows fx-tab | awk -F'\t' '$5 == "argv.1"')"; }
# a program on one side only: the hao side lost a process the reference has
differ fx-noprog "$FX/ref.tsv" "$FX/noprog.tsv"
if grep -q "^/usr/bin/fixturetool	ONLY-REF	" "$OUT/fx-noprog/programs.tsv" 2>/dev/null; then
    pass "fixture noprog: fixturetool reported reference-only"
    [ $drc = 1 ] && pass "fixture noprog: a program missing under hao is unexplained (exit 1)" ||
        bad "fixture noprog: a program missing under hao exits $drc (an unexplained difference must exit 1)"
else bad "fixture noprog: fixturetool not reported reference-only: $(cat "$OUT/fx-noprog/programs.tsv" 2>/dev/null | flat)"; fi
# the known list: entries need every field and a reason
k=$OUT/fx-known
mkdir -p "$k"
printf 'KNOWN\tDIFF\t^sig\\.SIGHUP$\t*\tfixture: SIGHUP ignored by design\n' >"$k/good.tsv"
printf 'KNOWN\tDIFF\t^sig\\.SIGHUP$\t^catch$\tfixture: only a caught SIGHUP is expected\n' >"$k/value.tsv"
printf 'KNOWN\tMISSING-REF\t^sig\\.SIGHUP$\t*\tfixture: wrong status\n' >"$k/status.tsv"
printf 'KNOWN\tDIFF\t^sig\\.SIGHUP$\t*\t\n' >"$k/noreason.tsv"
printf 'KNOWN\tDIFF\t^sig\\.SIGHUP$\t*\t   \n' >"$k/blankreason.tsv"
printf 'KNOWN\tDIFF\t^sig\\.SIGHUP$\t*\n' >"$k/fourfields.tsv"
printf 'NORM\tvalue\t^ignore$\tdefault\t\n' >"$k/normnoreason.tsv"
printf 'KNOWN\tDIFF\t^sig\\.(SIGHUP$\t*\tfixture: bad regex\n' >"$k/badregex.tsv"
fx known-good extrabit.tsv 0 "signal sig.SIGHUP DIFF" --known "$k/good.tsv" &&
    { [ "$(reason fx-known-good signal sig.SIGHUP)" = "fixture: SIGHUP ignored by design" ] &&
        pass "fixture known-good: the reason is recorded" || bad "fixture known-good: reason '$(reason fx-known-good signal sig.SIGHUP)'"; }
fx known-value extrabit.tsv 1 "signal sig.SIGHUP DIFF" --known "$k/value.tsv"
fx known-status extrabit.tsv 1 "signal sig.SIGHUP DIFF" --known "$k/status.tsv"
for b in noreason blankreason fourfields normnoreason badregex; do
    differ "fx-known-$b" "$FX/ref.tsv" "$FX/extrabit.tsv" --known "$k/$b.tsv"
    [ $drc = 2 ] && grep -q "$k/$b.tsv:1" "$OUT/fx-known-$b.log" && pass "fixture known $b: rejected (exit 2, names the line)" ||
        bad "fixture known $b: exit $drc (want 2): $(head -c 200 "$OUT/fx-known-$b.log")"
done
# malformed observations are bad input (2), never a difference (1)
python3 -I - "$FX/ref.tsv" "$k" <<'PY'
import sys
src, d = sys.argv[1], sys.argv[2]
L = open(src, encoding="utf-8").read().splitlines(True)
open(d + "/fields.tsv", "w").write("".join(L[:4]) + "100\t/usr/libexec/fixtured\t-\tsignal\tsig.SIGHUP\n" + "".join(L[4:]))
open(d + "/escape.tsv", "w").write("".join(L[:4]) + "100\t/usr/libexec/fixtured\tsystem/com.example.fixtured\tidentity\targv.2\tbad\\xZZ\n" + "".join(L[4:]))
open(d + "/header.tsv", "w").write("".join(L[2:]))
open(d + "/empty.tsv", "w").write(L[0])
PY
for b in fields escape header empty; do
    differ "fx-bad-$b" "$FX/ref.tsv" "$k/$b.tsv"
    [ $drc = 2 ] && pass "fixture malformed $b: exit 2" || bad "fixture malformed $b: exit $drc (want 2): $(tail -1 "$OUT/fx-bad-$b.log" | cut -c1-200)"
done

echo "$fails failed"
[ $fails = 0 ]
