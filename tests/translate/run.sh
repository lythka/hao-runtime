#!/bin/bash
# tests/translate/run.sh [BUILD_DIR] [WORK_DIR]
#   hao-embed --translate round trip on the host's own universal programs.
#   Copies /bin/ls, /usr/bin/plutil and /usr/bin/sw_vers (x86_64 + arm64e) into
#   WORK_DIR (default build/translate-host; system files are never modified),
#   replaces each copy's x86_64 slice with a --host-test stub from BUILD_DIR
#   (default build), and checks:
#     - every arm64 slice of the copy is byte-identical to the original's;
#     - the x86_64 slice is the stub, signed with the program's identifier,
#       with the arm64e slice's LC_BUILD_VERSION (platform, minos, sdk);
#     - 'arch -x86_64 COPY ARGS' (the stub under Rosetta, running the arm64e
#       slice through hao) prints exactly what the system program prints,
#       natively and under arch -x86_64 (an unmodified copy outside the system
#       volume is killed by AMFI, so the system program is the reference).
set -uo pipefail
H=$(cd "$(dirname "$0")/../.." && pwd)
B=$(cd "${1:-$H/build}" && pwd)
W=${2:-$H/build/translate-host}
rm -rf "$W"; mkdir -p "$W/orig" "$W/tr" "$W/data/d/sub"
for p in /bin/ls /usr/bin/plutil /usr/bin/sw_vers; do cp "$p" "$W/orig/"; done
echo hi > "$W/data/d/a.txt"; printf '%5000s' x > "$W/data/d/b.bin"; ln -s a.txt "$W/data/d/link"
touch -t 202601010000 "$W/data/d/a.txt" "$W/data/d/b.bin" "$W/data/d/sub"
plutil -create xml1 "$W/data/p.plist"; plutil -insert k -string v "$W/data/p.plist"
plutil -insert n -integer 42 "$W/data/p.plist"; plutil -insert arr -json '[1,"two",true]' "$W/data/p.plist"
python3 "$H/tools/hao-embed" --stub "$B/hao-stub" --host-test --translate -o "$W/tr" "$W"/orig/* || exit 1
python3 - "$H/tools" "$W" "$B" <<'EOF' || exit 1
import os, subprocess, sys
sys.path.insert(0, sys.argv[1]); import hao_fat
w, b = sys.argv[2], sys.argv[3]
def bv(arch, path):
    out = subprocess.run(['otool', '-arch', arch, '-l', path], capture_output=True, text=True).stdout
    t = out.split('cmd LC_BUILD_VERSION')[1].split()
    return ' '.join(t[t.index(k) + 1] for k in ('platform', 'minos', 'sdk'))
bad = 0
for f in sorted(os.listdir(os.path.join(w, 'orig'))):
    o = hao_fat.slices(open(os.path.join(w, 'orig', f), 'rb').read())
    t = hao_fat.slices(open(os.path.join(w, 'tr', f), 'rb').read())
    arm_same = [s for s in o if s[0] == hao_fat.CPU_ARM64] == [s for s in t if s[0] == hao_fat.CPU_ARM64]
    x86 = [s for s in t if s[0] == hao_fat.CPU_X86_64]
    d = subprocess.run(['codesign', '-d', '--arch', 'x86_64', '-v', os.path.join(w, 'tr', f)], capture_output=True, text=True).stderr
    o_id = subprocess.run(['codesign', '-d', '--arch', 'arm64e', '-v', os.path.join(w, 'orig', f)], capture_output=True, text=True).stderr
    ident = lambda s: [l for l in s.splitlines() if l.startswith('Identifier=')]
    mn, want = bv('x86_64', os.path.join(w, 'tr', f)), bv('arm64e', os.path.join(w, 'orig', f))
    ok = arm_same and len(x86) == 1 and ident(d) == ident(o_id) and mn == want
    bad += not ok
    print(f"{'PASS' if ok else 'FAIL'} {f}: arm slices identical {arm_same}, x86_64 slices {len(x86)}, "
          f"{ident(d)[0] if ident(d) else 'no identifier'}, build version {mn} (arm64e {want})")
sys.exit(1 if bad else 0)
EOF
cd "$W"
pass=0 fail=0
while IFS='|' read -r prog args; do
    sys=$(PATH=/bin:/usr/bin command -v "$prog")
    ref=$(timeout -s KILL 30 "$sys" $args 2>&1; echo "rc=$?")
    refx=$(timeout -s KILL 30 arch -x86_64 "$sys" $args 2>&1; echo "rc=$?")
    got=$(timeout -s KILL 60 arch -x86_64 "tr/$prog" $args 2>&1; echo "rc=$?")
    if [ "$ref" = "$got" ] && [ "$refx" = "$got" ]; then
        pass=$((pass + 1)); echo "PASS $prog $args"
    else
        fail=$((fail + 1)); echo "FAIL $prog $args"; diff <(echo "$ref") <(echo "$got") | head -10
    fi
done <<'LIST'
ls|-la data/d
ls|-1R data
ls|-lO data/d/a.txt
ls|/nonexistent
plutil|-p data/p.plist
plutil|-lint data/p.plist
plutil|-convert json -o - data/p.plist
plutil|-extract arr xml1 -o - data/p.plist
sw_vers|
sw_vers|-productVersion
sw_vers|-buildVersion
LIST
echo "translate: $pass pass, $fail fail"
[ "$fail" = 0 ]
