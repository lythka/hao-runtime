#!/bin/bash
# Run every test script that runs unattended on a development host
# (tests/check.list), in parallel where the list allows, each with a timeout
# and a private output directory. One line per script:
#   PASS  NAME  SECONDS
#   FAIL  NAME  SECONDS  reason (exit status, timeout) and the last output line
#   SKIP  NAME  SECONDS  the script's SKIP lines (expected or UNEXPECTED)
#   XFAIL NAME  SECONDS  an entry listed as a known failure (MODE xfail) failed
#   XPASS NAME  SECONDS  it passed: counted as a failure until the mark goes
# A script skips by printing lines that start with "SKIP " (and exiting 0, 2
# or 77). A skip is expected only if tests/expected-skips.txt (and the file
# named by EXPECTED_SKIPS, if set) has a line "NAME<tab>ERE" whose pattern
# matches that SKIP line. Exit 1 on any failure, unexpected skip or XPASS.
#   [B=build] [HAO=B/hao] [J=jobs] [KEEP=1] [CHECK_LIST=file] tests/check.sh [NAME...]
# Logs and outputs go to a mktemp -d directory, removed when every script
# passed or skipped as expected (KEEP=1 keeps it); its path is printed last.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

if [ "${1:-}" = --one ]; then
    # --one RUN NAME TIMEOUT MODE CMD: run one entry; write RUN/NAME.result
    run=$2 name=$3 limit=$4 mode=$5 cmd=$6
    key=$(printf %s "$name" | tr / _); dir=$run/$key; mkdir -p "$dir/out"; log=$dir/log
    start=$(date +%s)
    # Own process group, so a timeout kills every process the script started.
    set -m
    (cd "$repo" && OUT="$dir/out" exec bash -c "$cmd") </dev/null >"$log" 2>&1 &
    pid=$!
    set +m
    ( i=0; while kill -0 "$pid"; do
          sleep 1; i=$((i + 1))
          [ $i -ge "$limit" ] && { kill -KILL -- -"$pid"; echo timeout >"$dir/timedout"; break; }
      done ) >/dev/null 2>&1 &
    dog=$!
    wait "$pid" 2>/dev/null; rc=$?
    wait "$dog"
    kill -KILL -- -"$pid" 2>/dev/null   # strays left in the group
    secs=$(( $(date +%s) - start ))
    last=$(grep -v '^[[:space:]]*$' "$log" | tail -1 | cut -c1-160)
    skips=$(grep -E '^SKIP ' "$log" | sort -u)
    if [ -f "$dir/timedout" ]; then
        st=FAIL why="timeout after ${limit}s: $last"
    elif [ -n "$skips" ] && { [ $rc = 0 ] || [ $rc = 2 ] || [ $rc = 77 ]; }; then
        st=SKIP why=
        unexpected=0
        while IFS= read -r s; do
            ok=0
            while IFS="$(printf '\t')" read -r n pat; do
                case $n in ''|'#'*) continue ;; esac
                [ "$n" = "$name" ] && printf '%s\n' "$s" | grep -Eq -- "$pat" && { ok=1; break; }
            done < <(cat "$here/expected-skips.txt" ${EXPECTED_SKIPS:+"$EXPECTED_SKIPS"})
            [ $ok = 1 ] || { unexpected=1; s="UNEXPECTED $s"; }
            why="${why:+$why; }${s#SKIP }"
        done <<<"$skips"
        [ $unexpected = 1 ] && st=SKIP!
    elif [ $rc = 0 ]; then
        st=PASS why=
    else
        st=FAIL why="exit $rc: $last"
    fi
    if [ "$mode" = xfail ]; then
        case $st in
        FAIL) st=XFAIL ;;
        PASS) st=XPASS why="known failure passed: remove its xfail mark in tests/check.list" ;;
        esac
    fi
    printf '%s\t%s\t%s\t%s\n' "$st" "$name" "$secs" "$why" >"$run/$key.result"
    printf '%-5s %-28s %4ss %s\n' "${st%!}" "$name" "$secs" "$why"
    exit 0
fi

B=$(cd "${B:-$repo/build}" 2>/dev/null && pwd) || { echo "check: no build directory (run make)" >&2; exit 2; }
HAO=${HAO:-$B/hao}
[ -x "$HAO" ] || { echo "check: no $HAO (run make)" >&2; exit 2; }
J=${J:-$(( $(sysctl -n hw.ncpu) / 2 ))}
[ "$J" -ge 1 ] 2>/dev/null || J=1
export B HAO LIB=$B EXPECTED_SKIPS=${EXPECTED_SKIPS:-}
run=$(mktemp -d -t hao-check) || exit 2

# entries: NAME TIMEOUT MODE CMD (MODE par, xfail (parallel, known to fail) or
# serial (run alone, after the rest))
entries() {
    while read -r n t m cmd; do
        case $n in ''|'#'*) continue ;; esac
        if [ $# -gt 0 ]; then
            want=0; for a in "$@"; do [ "$a" = "$n" ] && want=1; done
            [ $want = 1 ] || continue
        fi
        printf '%s\t%s\t%s\t%s\n' "$n" "$t" "$m" "$cmd"
    done <"${CHECK_LIST:-$here/check.list}"
}
entries "$@" >"$run/entries"
[ -s "$run/entries" ] || { echo "check: no such entries: $*" >&2; exit 2; }
# xargs gets each job as seven NUL-separated arguments.
jobs() {
    awk -F'\t' -v m="$1" 'index(" " m " ", " " $3 " ")' "$run/entries" |
    while IFS="$(printf '\t')" read -r n t m cmd; do
        printf '%s\0' "$here/check.sh" --one "$run" "$n" "$t" "$m" "$cmd"
    done
}
start=$(date +%s)
jobs "par xfail" | xargs -0 -n 7 -P "$J" bash
jobs serial | xargs -0 -n 7 -P 1 bash

pass=0 fail=0 skip=0 bad=0 xfail=0
for r in "$run"/*.result; do
    [ -f "$r" ] || continue
    case $(cut -f1 "$r") in
    PASS) pass=$((pass + 1)) ;;
    FAIL|XPASS) fail=$((fail + 1)) ;;
    XFAIL) xfail=$((xfail + 1)) ;;
    SKIP) skip=$((skip + 1)) ;;
    SKIP!) skip=$((skip + 1)) bad=$((bad + 1)) ;;
    esac
done
total=$(wc -l <"$run/entries" | tr -d ' ')
missing=$(( total - pass - fail - skip - xfail ))
echo "check: $pass passed, $fail failed, $xfail known failures, $skip skipped ($bad unexpected), $missing did not report, of $total in $(( $(date +%s) - start ))s"
if [ $fail = 0 ] && [ $bad = 0 ] && [ $missing = 0 ]; then
    if [ "${KEEP:-0}" = 1 ]; then echo "check: logs in $run"; else rm -rf "$run"; fi
    exit 0
fi
echo "check: logs in $run"
exit 1
