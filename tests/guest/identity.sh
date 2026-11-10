#!/bin/bash
# The main executable as native code and translated code see it, in both launch
# routes, against the native arm64 run of the same file (tests/guest/identity.m,
# identhelper.c): process name and path, argv, NSProcessInfo, the main bundle
# and its embedded Info.plist, dyld's image 0 and program header, sections read
# by name from the main executable (an Info.plist, a marker segment like
# WindowServer's __SLSERVER, an empty one like __CGPreLoginApp/__RESTRICT),
# sections only the guest has read through getsectiondata/getsegmentdata on
# the program header and used (ExtensionFoundation's __TEXT,__swift5_entry
# call, a __DATA_CONST pointer, a __DATA segment), before main and after,
# code-signing identity, entitlements and the audit token.
#   OUT=dir [HAO=build/hao] [B=build] [T=60] tests/guest/identity.sh
# Routes:
#   embedded  tools/hao-embed --host-test copy run as 'arch -x86_64 FILE' (the
#             stub and its re-exec), through a relative path, an absolute path,
#             "./", and a symlink; JIT, --interp (HAO_FLAGS=i) and an AOT
#             cache (HAO_CACHE, from '$HAO --aot'). Nothing is
#             normalised: every line must equal the native arm64 run. The
#             same program signed with the hardened runtime gets a hardened
#             stub (hao-embed): its identity must match natively too.
#   CLI       '$HAO FILE' (the HaoExec "#!" route's launcher). The kernel's
#             record of the process is hao's there (p_comm, proc_pidpath,
#             KERN_PROCARGS2 exec path, code identity and entitlements, the
#             audit token, dyld's main image file), so exactly those lines are
#             masked on both sides; everything else must match. JIT, --interp
#             and AOT.
# Prints PASS/FAIL per case; exit 1 if any failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-identity}
HAO=${HAO:-$repo/build/hao}
B=${B:-$repo/build}
T=${T:-60}
rm -rf "$OUT"; mkdir -p "$OUT/plain" "$OUT/emb" "$OUT/lnk" "$OUT/hplain" "$OUT/hemb"
cd "$OUT" || exit 2
fails=0
cat >info.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.example.hao.identity</string>
<key>CFBundleName</key><string>identity</string>
<key>HaoIdentityKey</key><string>embedded-info-plist</string>
</dict></plist>
EOF
printf 'HaoIdentity\n' >haoid; : >empty
printf '<?xml version="1.0"?><plist version="1.0"><dict><key>com.apple.security.get-task-allow</key><true/></dict></plist>' >ents.plist
clang -arch x86_64 -arch arm64 -arch arm64e -O1 -dynamiclib -install_name "$OUT/libidenthelper.dylib" \
    -o libidenthelper.dylib "$here/identhelper.c" && codesign -s - -f libidenthelper.dylib 2>/dev/null || exit 2
clang -arch arm64 -arch arm64e -O1 -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework Security -lbsm \
    -o plain/identity "$here/identity.m" libidenthelper.dylib \
    -Wl,-sectcreate,__TEXT,__info_plist,info.plist -Wl,-sectcreate,__HAOID,__haoid,haoid -Wl,-segprot,__HAOID,r,r \
    -Wl,-sectcreate,__HAOMARK,__haomark,empty || exit 2
codesign -s - -f -i com.example.hao.identity --entitlements ents.plist plain/identity 2>/dev/null || exit 2
python3 "$repo/tools/hao-embed" --stub "$B/hao-stub" --host-test -o emb/identity plain/identity >/dev/null || exit 2
ln -s ../emb/identity lnk/alias
# an AOT cache for the program's arm64e slice (the embedded copy's is the same bytes)
rm -rf aot; mkdir -m 700 aot
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot plain/identity >aot.gen 2>&1 || { echo "FAIL AOT generation: $(head -c 300 aot.gen)"; exit 2; }
# hardened runtime (disable-library-validation: natively it must load the ad hoc helper)
printf '<?xml version="1.0"?><plist version="1.0"><dict><key>com.apple.security.get-task-allow</key><true/><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>' >hents.plist
cp plain/identity hplain/identity
codesign -s - -f -o runtime -i com.example.hao.identity --entitlements hents.plist hplain/identity 2>/dev/null || exit 2
python3 "$repo/tools/hao-embed" --stub "$B/hao-stub" --host-test -o hemb/identity hplain/identity >/dev/null || exit 2

# same NAME REF GOT: the two outputs (files) must be identical
same() {
    if [ -s "$2" ] && cmp -s "$2" "$3"; then echo "PASS $1 ($(wc -l <"$3" | tr -d ' ') lines)"
    else echo "FAIL $1"; diff "$2" "$3" | head -40; fails=$((fails + 1)); fi
}
n=0
# embedded route: the stub, under Rosetta, against the arm64 slices run natively
for p in emb/identity "$OUT/emb/identity" ./emb/identity lnk/alias; do
    n=$((n + 1))
    timeout -s KILL "$T" arch -arm64 "$p" >"native.$n" 2>&1
    timeout -s KILL "$T" arch -x86_64 "$p" >"stub.$n" 2>&1
    same "embedded, JIT: $p" "native.$n" "stub.$n"
    HAO_FLAGS=i timeout -s KILL "$T" arch -x86_64 "$p" >"stubi.$n" 2>&1
    same "embedded, --interp: $p" "native.$n" "stubi.$n"
    HAO_CACHE="$OUT/aot" timeout -s KILL "$T" arch -x86_64 "$p" >"stuba.$n" 2>&1
    same "embedded, AOT: $p" "native.$n" "stuba.$n"
done
# embedded route, hardened runtime: the stub is hardened as the program is
if codesign -dv --arch x86_64 hemb/identity 2>&1 | grep -q 'flags=.*runtime'; then echo "PASS embedded, hardened: the stub has the hardened runtime"
else echo "FAIL embedded, hardened: the stub lacks the hardened runtime"; fails=$((fails + 1)); fi
n=$((n + 1))
timeout -s KILL "$T" arch -arm64 hemb/identity >"native.$n" 2>&1
timeout -s KILL "$T" arch -x86_64 hemb/identity >"stub.$n" 2>&1
same "embedded, hardened, JIT" "native.$n" "stub.$n"
HAO_FLAGS=i timeout -s KILL "$T" arch -x86_64 hemb/identity >"stubi.$n" 2>&1
same "embedded, hardened, --interp" "native.$n" "stubi.$n"
# CLI route
mask() {
    sed -E -e 's/^(proc_pidpath|pbi_comm|kinfo_proc\.p_comm|KERN_PROCARGS2\.exec_path)=.*/\1=<kernel: hao>/' \
        -e 's/^(csops\.[a-z]+|SecTask[A-Za-z()-]+|xpc_copy_entitlement_for_self[^=]*|SecCodeCopySelf\.[A-Za-z]+)[=:].*/\1=<code identity: hao>/' \
        -e 's/^(proc_pidpath\(audit_token\)|SecTaskCreateWithAuditToken\.identifier)=.*/\1=<kernel: hao>/' \
        -e 's/^(native\._dyld_get_image_name\(main\)|native\.dladdr\(_NSGetMachExecuteHeader\(\)\)\.dli_fname)=.*/\1=<dyld main image: hao>/' "$1"
}
for p in plain/identity "$OUT/plain/identity" lnk/../plain/identity; do
    n=$((n + 1))
    timeout -s KILL "$T" arch -arm64 "$p" >"native.$n" 2>&1
    timeout -s KILL "$T" "$HAO" "$p" >"cli.$n" 2>&1
    mask "native.$n" >"native.$n.m"; mask "cli.$n" >"cli.$n.m"
    same "CLI, JIT: $p" "native.$n.m" "cli.$n.m"
    timeout -s KILL "$T" "$HAO" --interp "$p" >"clii.$n" 2>&1
    mask "clii.$n" >"clii.$n.m"
    same "CLI, --interp: $p" "native.$n.m" "clii.$n.m"
    HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" "$p" >"clia.$n" 2>&1
    mask "clia.$n" >"clia.$n.m"
    same "CLI, AOT: $p" "native.$n.m" "clia.$n.m"
done
# the AOT runs above used the cache
for r in "arch -x86_64 emb/identity" "$HAO plain/identity"; do
    if HAO_CACHE="$OUT/aot" HAO_VERBOSE=1 timeout -s KILL "$T" $r 2>&1 >/dev/null | grep -q "AOT cache $OUT/aot/.* loaded"; then
        echo "PASS AOT cache loaded: $r"
    else echo "FAIL AOT cache not loaded: $r"; fails=$((fails + 1)); fi
done
# An embedded program never takes its path from HAO_EXEC (the CLI's stage-2 hand-off): with
# HAO_EXEC and argv[0] naming another program, the stub still runs its own file.
clang -arch arm64e -O1 -o other "$here/hello.c" && codesign -s - -f other 2>/dev/null
HAO_EXEC="$OUT/other" timeout -s KILL "$T" arch -x86_64 /bin/bash -c 'exec -a "$1" "$2"' - "$OUT/other" "$OUT/emb/identity" >planted 2>&1
if grep -q "^argv\[0\]=$OUT/other$" planted && grep -q "^_NSGetExecutablePath=$OUT/emb/identity$" planted && grep -q "^native._NSGetExecutablePath=$OUT/emb/identity$" planted \
   && ! grep -q "hello from" planted; then echo "PASS embedded: a planted HAO_EXEC is ignored"
else echo "FAIL embedded: planted HAO_EXEC: $(head -c 300 planted)"; fails=$((fails + 1)); fi
grep -q '^NSBundle.mainBundle\[HaoIdentityKey\]=embedded-info-plist$' native.1 || { echo "FAIL reference run lacks its Info.plist"; fails=$((fails + 1)); }
grep -q '^native.getsectbyname(__HAOID,__haoid)+slide=12 bytes' native.1 || { echo "FAIL reference run lacks __HAOID"; fails=$((fails + 1)); }
grep -q '^native.prog.__swift5_entry(20)=61$' native.1 || { echo "FAIL reference run lacks the __swift5_entry call"; fails=$((fails + 1)); }
echo "identity: $fails failed"
[ "$fails" = 0 ]
