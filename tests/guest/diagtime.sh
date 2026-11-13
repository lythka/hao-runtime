#!/bin/sh
# HAO_DIAG_TIME (boot argument hao_diag_time=1): diagnostics lines start with "@<mach seconds> ", and the
# startup phases (exec front diag bridge jit thunks load bound registered/aot per image, main) are printed in
# order, each from diag on followed by its usage line (CPU, page-ins, disk reads). Lines written while the log directory's volume is absent (PID 1 before the Data volume) are kept and
# written first when the log opens: diagtime.c makes the missing parent, then aborts; its report opens the log.
# Without the switch nothing is stamped and no phase line is printed.
#   OUT=dir HAO=build/hao [T=30] tests/guest/diagtime.sh     exit 1 on any failure
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-diagtime)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
mkdir -p "$OUT"
clang -arch arm64e -O1 -o "$OUT/diagtime" "$here/diagtime.c" && codesign -s - -f "$OUT/diagtime" 2>/dev/null || exit 2
fail=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }

# 1. kept lines: the log's parent appears only when the guest makes it
rm -rf "$OUT/vol"
( HAO_DIAG_TIME=1 HAO_NO_AOT=1 HAO_DIAG_DIR="$OUT/vol/log" timeout -s KILL "$T" "$HAO" "$OUT/diagtime" "$OUT/vol" \
    >"$OUT/kept.out" 2>"$OUT/kept.err"; exit $? ) 2>/dev/null
log=$OUT/vol/log/console.log
if [ -f "$log" ]; then ok "log opened after the volume appeared"; else bad "no log at $log"; fi
kept=$(grep -n "lines kept from before the Data volume was mounted follow" "$log" 2>/dev/null | head -1 | cut -d: -f1)
main=$(grep -n "phase main at" "$log" 2>/dev/null | head -1 | cut -d: -f1)
sig=$(grep -n "signal 6" "$log" 2>/dev/null | head -1 | cut -d: -f1)
if [ -n "$kept" ] && [ -n "$main" ] && [ -n "$sig" ] && [ "$kept" -lt "$main" ] && [ "$main" -lt "$sig" ]; then
    ok "kept lines (phases) precede the report that opened the log"
else
    bad "order kept=${kept:-none} main=${main:-none} signal=${sig:-none}"
fi
order=$(grep -o "phase [a-z]*" "$log" | awk '{print $2}' | awk '!s[$0]++' | tr '\n' ' ')
case "$order" in
"exec front diag bridge jit thunks load bound registered aot main ") ok "phases in order: $order" ;;
*) bad "phase order: $order" ;;
esac
n=$(grep -c "phase " "$log"); s=$(grep -c "^@[0-9][0-9]*\.[0-9][0-9][0-9][0-9][0-9][0-9] .*phase " "$log")
if [ "$n" -gt 0 ] && [ "$n" = "$s" ]; then ok "$n phase lines, all stamped"; else bad "$s of $n phase lines stamped"; fi
# a usage line follows each phase from diag on, with the same name
pu=$(awk '/ phase .+ at [0-9]/ { for (i = 1; i < NF; i++) if ($i == "phase") p = $(i + 1); if (p == "diag") on = 1; if (on) print "phase " p }
          / usage .+ cpu_user_ms=[0-9]+\.[0-9]{3} cpu_sys_ms=[0-9]+\.[0-9]{3} pageins=[0-9]+ read_kb=[0-9]+$/ {
              for (i = 1; i < NF; i++) if ($i == "usage") print "usage " $(i + 1) }' "$log" |
     awk '$1 == "phase" { if (want != "") bad = 1; want = $2; next } { if ($2 != want) bad = 1; want = ""; n++ } END { print (bad || want != "" ? "bad" : n) }')
case "$pu" in
bad|0|"") bad "usage lines do not pair with the phases from diag on"; grep " usage \| phase " "$log" | head -6 ;;
*) ok "$pu usage lines, one after each phase from diag on" ;;
esac
if grep -q "^@[0-9]*\.[0-9]* hao\[diagtime:[0-9]*\]: signal 6" "$log"; then ok "the report is stamped"; else bad "unstamped report"; fi
if grep -q "phase main" "$OUT/kept.err"; then ok "phases on the console too"; else bad "no phase lines on stderr"; fi

# 2. no switch: no stamps, no phases (the same guest, normal exit)
( timeout -s KILL "$T" "$HAO" "$OUT/diagtime" "$OUT/vol2" x >"$OUT/plain.out" 2>"$OUT/plain.err"; exit $? ) 2>/dev/null
rc=$?
if [ $rc = 0 ] && ! grep -q "phase \|usage \|^@[0-9]" "$OUT/plain.err"; then ok "no switch: no stamps or phases (rc 0)"; else bad "no switch: rc $rc"; head -3 "$OUT/plain.err"; fi
exit $fail
