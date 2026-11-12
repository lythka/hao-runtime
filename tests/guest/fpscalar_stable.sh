#!/bin/sh
# Informational fixed-source throughput; optional BEFORE names another hao.
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fpscalar-stable)}
HAO=${HAO:-$repo/build/hao}
mkdir -p "$OUT"
for arch in arm64 arm64e; do
    clang -arch "$arch" -O1 -o "$OUT/stable.$arch" "$repo/tests/guest/fpscalar_stable.c"
    codesign -s - -f "$OUT/stable.$arch" 2>/dev/null
done
python3 - "$OUT" "$HAO" "${BEFORE:-}" "${REPEATS:-7}" "${N:-1000000}" <<'PY'
import json, pathlib, re, subprocess, sys
out=pathlib.Path(sys.argv[1]); hao=sys.argv[2]; before=sys.argv[3]
repeats=int(sys.argv[4]); count=sys.argv[5]
ops=['fadd_s','fadd_d','fsub_s','fsub_d','fmul_s','fmul_d','fdiv_s','fdiv_d',
     'fsqrt_s','fsqrt_d','fcvt_ds','fcvt_sd','fcmp_s','fcmp_d','fcmp_s0','fcmp_d0',
     'fcmpe_s','fcmpe_d','fcmpe_s0','fcmpe_d0']
commands={'native':[str(out/'stable.arm64')],
          'hao':['env','HAO_NO_AOT=1',hao,str(out/'stable.arm64e')]}
if before: commands['before']=['env','HAO_NO_AOT=1',before,str(out/'stable.arm64e')]
results={op:{k:[] for k in commands} for op in ops}; checksums={}
for run in range(repeats):
    order=list(commands) if run%2==0 else list(reversed(commands))
    for op in ops:
        for kind in order:
            p=subprocess.run(commands[kind]+[op,count],capture_output=True,text=True,timeout=60)
            assert p.returncode==0,(op,kind,p.returncode,p.stderr)
            m=re.fullmatch(r'\S+ ([0-9.]+) ns/insn ([0-9a-f]+)\n',p.stdout)
            assert m,p.stdout
            results[op][kind].append(float(m[1]))
            assert checksums.setdefault(op,m[2])==m[2],(op,kind,'result mismatch')
(out/'timings.json').write_text(json.dumps(results,indent=2))
columns=['before','hao','native'] if before else ['hao','native']
print('form '+' '.join(columns)+' (ns/insn; minimum of '+str(repeats)+')')
for op in ops:print(op,*[f'{min(results[op][k]):.2f}' for k in columns])
PY
