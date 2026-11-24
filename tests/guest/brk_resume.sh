#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
hao=${HAO:-$root/build/hao}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang -arch arm64 -Wall -Werror "$root/tests/guest/brk_resume.c" -o "$out/probe"
codesign -fs - "$out/probe" 2>/dev/null
for mode in moved repeat once; do
    timeout -k 2 15 "$out/probe" "$mode" > "$out/reference"
    for engine in jit interp; do
        args=("$hao")
        if [ "$engine" = interp ]; then args+=(--interp); fi
        timeout -k 2 15 "${args[@]}" "$out/probe" "$mode" > "$out/actual" 2> "$out/errors"
        if grep -q "guest brk at pc=" "$out/errors"; then exit 1; fi
        diff -u "$out/reference" "$out/actual"
    done
done
python3 - "$out/probe" "$hao" <<'PYTHON'
import os
import select
import signal
import subprocess
import sys
probe, hao = sys.argv[1:]
for command in ([probe], [hao, probe], [hao, "--interp", probe]):
    result = subprocess.run([*command, "default"], timeout=15)
    if result.returncode != -signal.SIGTRAP:
        raise SystemExit(f"default breakpoint returned {result.returncode}")
    if command[0] == hao:
        env = dict(os.environ, HAO_CRASH_TRACE="1")
        result = subprocess.run([*command, "default"], env=env, stderr=subprocess.PIPE, timeout=15)
        if result.returncode != -signal.SIGTRAP or b"guest brk at pc=" not in result.stderr:
            raise SystemExit("traced fatal breakpoint lost its guest instruction diagnostic")
    for mode in ("ignored", "blocked"):
        with subprocess.Popen([*command,mode], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as process:
            try:
                if not select.select([process.stdout], [], [], 15)[0] or process.stdout.read(1) != b"R":
                    raise SystemExit(f"{mode}: target did not reach its BRK")
                try:
                    status = process.wait(timeout=0.3)
                except subprocess.TimeoutExpired:
                    continue
                raise SystemExit(f"{mode}: unexpectedly passed the BRK, status {status}")
            finally:
                if process.poll() is None: process.kill()
PYTHON
