#!/bin/bash
# The stub's load-time identity (hao-embed): every stub's LC_BUILD_VERSION is
# its program's arm slice's (platform, minos, sdk), so dyld gives the process
# the program's platform and SDK. A Mac Catalyst program (platform 6) may load
# /System/iOSSupport images that dyld refuses in a macOS process ("wrong
# platform to load into process"); 69 of the 933 arm64-only 27.0 programs are.
#   OUT=dir [B=build] [EMBED=tools/hao-embed] tests/tools/platform.sh
# Cases (tests/tools/platform.c, command-line only, no UI):
#  - catalyst: Mac Catalyst arm64e program at minos 18.0, sdk 26.0, linking a
#    Catalyst-only iOSSupport library: the stub's LC_BUILD_VERSION equals the
#    arm64e slice's; the embedded program under hao (JIT and --interp) prints
#    what the same source built for native arm64 prints (platform, sdk, minos,
#    the linked library loaded, a dlopen of another one);
#  - macos: a macOS program at minos 11.0, sdk 15.0: the same checks (the
#    dlopen of the Catalyst-only library fails as natively);
#  - --stubs: the manifest records the stub's platform;
#  - zippered: a program with two build versions is refused.
# Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-tools-platform}
B=${B:-$repo/build}
EMBED=${EMBED:-$repo/tools/hao-embed}
fails=0
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT" || exit 2
LIB=/System/iOSSupport/System/Library/PrivateFrameworks/AccessibilitySharedUISupport.framework/Versions/A/AccessibilitySharedUISupport
[ -e "$LIB" ] || dyld_info -platform "$LIB" >/dev/null 2>&1 || { echo "SKIP no $LIB on this host"; exit 2; }
# install-name-only text stub for linking the Catalyst-only library
printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-maccatalyst, arm64e-maccatalyst ]\ninstall-name: '%s'\n...\n" "$LIB" >asus.tbd
bv_of() { otool -arch "$1" -l "$2" | awk '/cmd LC_BUILD_VERSION/ {getline; getline; p=$2; getline; m=$2; getline; s=$2; print p, m, s; exit}'; }

build() {  # NAME TARGET-OS PLATFORM-VERSION-ARGS EXTRA...
    local n=$1 os=$2 pv=$3; shift 3
    for arch in arm64 arm64e; do
        clang -target $arch-apple-$os -O1 -Wl,-platform_version,$pv -o $n.$arch "$here/platform.c" "$@" &&
            codesign -s - -f $n.$arch 2>/dev/null || exit 2
    done
}
build catalyst ios18.0-macabi mac-catalyst,18.0,26.0 asus.tbd
build macos macos11.0 macos,11.0,15.0

for n in catalyst macos; do
    timeout -s KILL 60 ./$n.arm64 >$n.native 2>&1
    if ! python3 "$EMBED" --host-test --stub "$B/hao-stub" -o $n.emb $n.arm64e >embed-$n.log 2>&1; then
        bad "$n: hao-embed: $(tr '\n' ' ' <embed-$n.log)"; continue
    fi
    a=$(bv_of arm64e $n.emb) x=$(bv_of x86_64 $n.emb)
    if [ -n "$a" ] && [ "$a" = "$x" ]; then pass "$n: stub LC_BUILD_VERSION is the arm64e slice's (platform minos sdk: $x)"
    else bad "$n: stub LC_BUILD_VERSION '$x', arm64e slice '$a'"; fi
    for mode in jit interp; do
        env=(); [ $mode = interp ] && env=(HAO_INTERP=1)
        timeout -s KILL 120 env ${env[@]+"${env[@]}"} arch -x86_64 ./$n.emb >$n.$mode 2>$n.$mode.err
        if [ -s $n.native ] && diff $n.native $n.$mode >/dev/null; then
            pass "$n $mode: embedded program as native arm64: $(tr '\n' ';' <$n.$mode)"
        else bad "$n $mode: native '$(tr '\n' ';' <$n.native)', embedded '$(tr '\n' ';' <$n.$mode) $(head -c 400 $n.$mode.err)'"; fi
    done
done
grep -q "^platform 6 sdk 26.0 minos 18.0" catalyst.native && grep -q "^linked loaded" catalyst.native &&
    grep -q "^dlopen ok" catalyst.native && pass "catalyst: native arm64 is a Catalyst process that loads iOSSupport libraries" ||
    bad "catalyst: native arm64 printed '$(tr '\n' ';' <catalyst.native)'"
grep -q "^dlopen failed: wrong platform" macos.native && pass "macos: native arm64 refuses the Catalyst-only library" ||
    bad "macos: native arm64 printed '$(tr '\n' ';' <macos.native)'"

# --- the manifest records each stub's platform
mkdir -p root/usr/bin && cp catalyst.arm64e root/usr/bin/catalyst && cp macos.arm64e root/usr/bin/macos
python3 "$EMBED" --stub "$B/hao-stub" --stubs pay --root root --x86-cache "$X86" --scan >pay.log 2>&1 || bad "--stubs: $(cat pay.log)"
python3 - pay/manifest.json <<'PY' && pass "--stubs: manifest platform: maccatalyst 18.0/26.0, macos 11.0/15.0" || bad "--stubs: manifest platform"
import json, sys
m = {e["path"]: e["platform"] for e in json.load(open(sys.argv[1]))["files"]}
assert m == {"usr/bin/catalyst": ["maccatalyst", "18.0.0", "26.0.0"], "usr/bin/macos": ["macos", "11.0.0", "15.0.0"]}, m
PY

# --- a zippered program (two build versions) is refused
vtool -set-build-version macos 27.0 27.0 -set-build-version maccatalyst 27.0 27.0 -replace -output zip.arm64e catalyst.arm64e 2>/dev/null &&
    codesign -s - -f zip.arm64e 2>/dev/null || exit 2
python3 "$EMBED" --host-test --stub "$B/hao-stub" -o zip.emb zip.arm64e >zip.log 2>&1
grep -q "^fail.*2 build versions" zip.log && [ ! -e zip.emb ] && pass "zippered: refused ($(cut -f3 zip.log | head -1))" ||
    bad "zippered: $(cat zip.log)"

[ $fails = 0 ] && echo "platform: all cases passed" || echo "platform: $fails failed"
[ $fails = 0 ]
