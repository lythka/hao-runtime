#!/bin/bash
# The df lines of the 27.0 corpus must give the same output however the host's
# disks change. tools/run27.sh runs the x86_64 reference first and hao second
# and compares the whole output, so a df line that reports a volume whose free
# space moves (`/` shares its APFS container with the Data volume) fails at
# random. This test makes that deterministic: run27.sh is pointed at a wrapper
# build/hao that writes 16 MB to the Data volume before it execs the real hao,
# so the space on `/` always changes between the two runs.
#
# usage: tests/corpus27/corpus27_df.sh [CORPUS]   (default tests/corpus27/corpus27.txt)
# env:   N=runs per line (default 5), OUT=work dir (default: a temporary one)
# Needs a mounted 27.0 System in $HAO_FS, build/hao and the corpus runner tools/run27.sh (not in this tree).
# Exit: 0 all PASS, 1 some FAIL, 2 cannot test, 77 skipped (SKIP line says why).
R=$(cd "$(dirname "$0")/../.." && pwd)
corpus=${1:-$R/tests/corpus27/corpus27.txt}
FS=${HAO_FS:-}
N=${N:-5}
[ -x "$R/build/hao" ] || { echo "corpus27_df: no $R/build/hao (run make)"; exit 2; }
[ -n "$FS" ] && [ -e "$FS/bin/df" ] || { echo "SKIP corpus27_df: HAO_FS: no 27.0 System at '$FS'"; exit 77; }
[ -x "$R/tools/run27.sh" ] || { echo "SKIP corpus27_df: no tools/run27.sh (the corpus runner is not in this tree)"; exit 77; }
if [ -n "$OUT" ]; then W=$OUT; mkdir -p "$W"; else W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; fi
rm -f "$W"/fill.*
mkdir -p "$W/build"
cat > "$W/build/hao" <<EOF
#!/bin/bash
dd if=/dev/zero of="$W/fill.\$\$.\$RANDOM" bs=1m count=16 2>/dev/null; sync
exec "$R/build/hao" "\$@"
EOF
chmod +x "$W/build/hao"

# The perturbation has to reach /: otherwise this test proves nothing.
avail() { df -k / | awk 'NR == 2 { print $4 }'; }
a0=$(avail); dd if=/dev/zero of="$W/fill.probe" bs=1m count=16 2>/dev/null; sync; a1=$(avail)
if [ "$a0" == "$a1" ]; then echo "corpus27_df: writing to $W does not change free space on /"; exit 2; fi

lines=0; pass=0; fail=0
while read -r -a cmd; do
  [ "${cmd[0]}" == bin/df ] || continue
  lines=$((lines+1))
  for i in $(seq "$N"); do
    out=$(cd "$W" && "$R/tools/run27.sh" "${cmd[@]}")
    case "$out" in PASS*) pass=$((pass+1));; *) fail=$((fail+1)); echo "$out";; esac
    rm -f "$W"/fill.*
  done
done < "$corpus"
[ $lines -gt 0 ] || { echo "corpus27_df: no bin/df line in $corpus"; exit 2; }
echo "corpus27_df: $lines df line(s) x $N runs: $pass pass, $fail fail"
[ $fail -eq 0 ]
