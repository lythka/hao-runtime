#!/bin/sh
# audit_token_t by value to native functions (tests/guest/atoken.c): build for
# arm64e (guest) and arm64 (reference), run arm64 natively and arm64e under hao
# with the JIT, with --interp, and from an AOT cache; each hao run must print
# what native prints. The SPI it calls has no SDK stubs, so small .tbd files
# stand in for them. sandbox_check_network crashes launchd without the typed route.
# Also the members a 26A428 code-evidence scan found
# untyped: sandbox_extension_issue_file_to_process (logd -> `log show`), responsibility_get_attribution_for_audittoken, _LSVersionNumberCompare
# (two 32-byte structs), FigCFDictionarySetCMTime (CMTime, 24 bytes).
#   OUT=dir HAO=build/hao [T=60] tests/guest/atoken.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-atoken}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
tbd() {  # NAME INSTALL_NAME SYMBOL...
    f=$OUT/$1.tbd; shift
    in=$1; shift
    syms=$(printf '%s, ' "$@"); syms=${syms%, }
    printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-macos, arm64e-macos ]\ninstall-name: '%s'\nexports:\n  - targets: [ arm64-macos, arm64e-macos ]\n    symbols: [ %s ]\n...\n" \
        "$in" "$syms" >"$f"
}
set -e
tbd TCC /System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC _TCCAccessCheckAuditToken \
    _TCCAccessPreflightWithAuditToken _tcc_credential_create_for_process_with_audit_token _kTCCAccessCheckOptionPrompt
tbd NE /System/Library/Frameworks/NetworkExtension.framework/Versions/A/NetworkExtension _NEUIDForAuditToken
tbd sbx /usr/lib/system/libsystem_sandbox.dylib _sandbox_container_path_for_audit_token \
    _sandbox_check_network _sandbox_checkattr_alloc _sandbox_checkattr_free \
    _sandbox_extension_issue_file_to_process _sandbox_extension_consume _sandbox_extension_release
# Scan members: libquarantine's
# responsibility SPI, LaunchServices' version compare (two 32-byte structs), CoreMedia's Fig SPI
# (CMTime by value; the public key constants come from the same tbd, so CoreMedia is not linked twice).
tbd qtn /usr/lib/system/libquarantine.dylib _responsibility_get_attribution_for_audittoken \
    _responsibility_identity_release
tbd LS /System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/LaunchServices \
    __LSVersionNumberCompare
tbd CM /System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia _FigCFDictionarySetCMTime \
    _kCMTimeValueKey _kCMTimeScaleKey _kCMTimeFlagsKey _kCMTimeEpochKey
for a in arm64e arm64; do
    clang -arch $a -O1 -framework CoreFoundation -framework IOKit "$OUT/TCC.tbd" "$OUT/NE.tbd" "$OUT/sbx.tbd" \
        "$OUT/qtn.tbd" "$OUT/LS.tbd" "$OUT/CM.tbd" -o "$OUT/atoken.$a" "$here/atoken.c"
    codesign -s - -f "$OUT/atoken.$a" 2>/dev/null
done
set +e
fail=0
timeout -s KILL "$T" "$OUT/atoken.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "atoken native rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] || fail=1
# the reference is deterministic (no pointers or extension tokens are printed): native vs itself
timeout -s KILL "$T" "$OUT/atoken.arm64" >"$OUT/native2.out" 2>/dev/null
if cmp -s "$OUT/native.out" "$OUT/native2.out"; then echo "atoken native-repeat SAME"; else echo "atoken native-repeat DIFF"; fail=1; fi
# Build an AOT cache for the guest, then run from it (HAO_NO_JIT so the cache is exercised).
export HAO_CACHE="$OUT/aot"
mkdir -p "$HAO_CACHE"
timeout -s KILL "$T" "$HAO" --aot "$OUT/atoken.arm64e" >"$OUT/aotgen.out" 2>&1
for mode in jit interp aot; do
    flag=; [ $mode = interp ] && flag=--interp
    timeout -s KILL "$T" "$HAO" $flag "$OUT/atoken.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "atoken $mode rc=$rc $r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -20; grep -m3 "fault\|hao:" "$OUT/$mode.err" | sed 's/^/    /'; }
done
[ $fail = 0 ] && echo "atoken: PASS" || echo "atoken: FAIL"
exit $fail
