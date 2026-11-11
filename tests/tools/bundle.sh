#!/bin/bash
# hao-embed on a bundle's main executable: the stub slice's signature binds the
# bundle's Info.plist (special slot -1) and resource seal (slot -3) as the
# arm64e slice's does, so code-signing clients (libsecinit's app sandbox
# container lookup, LaunchServices, TCC, SecCode*) read the same identity on
# x86_64 as natively; hao-install.py embeds such a stub only into a bundle
# whose files match. Static checks only: nothing built here is launched.
#   OUT=dir [B=build] [EMBED=tools/hao-embed] [HAO_FS=/path/to/mounted/macOS-27-System] tests/tools/bundle.sh
# With the 27.0 System mounted at HAO_FS, the same checks run on its
# MenuBarAgent.app (copied; app sandbox, secinit SIGILL without the bundle binding). Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools-bundle)}
B=${B:-$repo/build}
EMBED=${EMBED:-$repo/tools/hao-embed}
HAO_FS=${HAO_FS:-}
fails=0
# the x86_64 shared cache payload stubs (--stubs) are linked against: this host's
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT"
CHK=$OUT/sigcheck
clang -arch x86_64 -O1 -o "$CHK" "$here/sigcheck.c" -framework Security -framework CoreFoundation &&
    codesign -s - -f "$CHK" 2>/dev/null || exit 2

# slot ARCH BUNDLE N: special slot -N of that slice's CodeDirectory (zeros if not bound)
slot() { timeout 30 codesign -d -vvvvvv --arch "$1" "$2" 2>&1 | sed -n "s/^ *-$3=//p"; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
# every file of a bundle but its main executable, with its hash
others() { (cd "$1" && find . -type f ! -path "./Contents/MacOS/$2" -exec shasum -a 256 {} + | sort); }

# same_slices SRC DST: every slice of SRC (thin or fat) is in DST byte for byte
same_slices() {
    local a n=0
    for a in $(lipo -archs "$1"); do
        n=$((n + 1))
        if [ "$(lipo -archs "$1")" = "$a" ]; then cp "$1" "$OUT/slice.src"; else lipo -thin "$a" "$1" -output "$OUT/slice.src"; fi
        lipo -thin "$a" "$2" -output "$OUT/slice.dst" 2>/dev/null && cmp -s "$OUT/slice.src" "$OUT/slice.dst" || return 1
    done
    [ $n -gt 0 ]
}

# check NAME APP EXE: the static checks on an embedded bundle APP (main executable EXE)
check() {
    local name=$1 app=$2 exe=$3 ip cr id
    ip=$(sha "$app/Contents/Info.plist")
    [ "$(slot x86_64 "$app" 1)" = "$ip" ] && pass "$name: x86_64 slot -1 = sha256(Contents/Info.plist)" ||
        bad "$name: x86_64 slot -1 '$(slot x86_64 "$app" 1)', Info.plist $ip"
    if [ "$(slot arm64e "$app" 3 | tr -d 0)" ]; then
        cr=$(sha "$app/Contents/_CodeSignature/CodeResources")
        [ "$(slot x86_64 "$app" 3)" = "$cr" ] && pass "$name: x86_64 slot -3 = sha256(_CodeSignature/CodeResources)" ||
            bad "$name: x86_64 slot -3 '$(slot x86_64 "$app" 3)', CodeResources $cr"
    fi
    id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$app/Contents/Info.plist")
    timeout 30 codesign -d -v --arch x86_64 "$app" >"$OUT/$name.cs" 2>&1
    if grep -q "^Info.plist entries=[1-9]" "$OUT/$name.cs" && grep -qx "Identifier=$id" "$OUT/$name.cs" &&
        [ "$(grep '^Sealed Resources' "$OUT/$name.cs")" = "$(timeout 30 codesign -d -v --arch arm64e "$app" 2>&1 | grep '^Sealed Resources')" ]; then
        pass "$name: codesign -d --arch x86_64: $(grep -h '^Info.plist entries' "$OUT/$name.cs"), Identifier=$id, same resource seal as arm64e"
    else bad "$name: codesign -d --arch x86_64: $(tr '\n' ' ' <"$OUT/$name.cs")"; fi
    timeout 60 "$CHK" "$app" x86_64 >"$OUT/$name.x86" 2>&1
    timeout 60 "$CHK" "$app" arm64e >"$OUT/$name.arm" 2>&1
    if grep -qx "plist.CFBundleIdentifier=$id" "$OUT/$name.x86" && cmp -s "$OUT/$name.x86" "$OUT/$name.arm"; then
        pass "$name: SecStaticCode x86_64 = arm64e: $(tr '\n' ' ' <"$OUT/$name.x86")"
    else bad "$name: SecStaticCode x86_64: $(tr '\n' ' ' <"$OUT/$name.x86") arm64e: $(tr '\n' ' ' <"$OUT/$name.arm")"; fi
}

# --- fixture: an arm64e app bundle with the app-sandbox entitlement, signed as a bundle
F=$OUT/fixture/Fix.app
mkdir -p "$F/Contents/MacOS" "$F/Contents/Resources"
clang -arch arm64e -O1 -o "$F/Contents/MacOS/Fix" "$repo/tests/guest/hello.c" || exit 2
cat >"$F/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>Fix</string>
	<key>CFBundleIdentifier</key>
	<string>org.hao.test.bundlefix</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>LSUIElement</key>
	<true/>
</dict>
</plist>
PL
echo resource >"$F/Contents/Resources/data.txt"
cat >"$OUT/sandbox.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.app-sandbox</key><true/></dict></plist>
PL
codesign -s - -f --entitlements "$OUT/sandbox.plist" "$F" 2>/dev/null || exit 2

# --- hao-embed --in-place on a copy
E=$OUT/embedded/Fix.app; mkdir -p "$OUT/embedded"; cp -R "$F" "$E"
"$EMBED" --stub "$B/hao-stub" --in-place "$E/Contents/MacOS/Fix" >"$OUT/embed.log" 2>&1
grep -q "^ok" "$OUT/embed.log" && pass "hao-embed: $(head -1 "$OUT/embed.log" | cut -f1,3-)" || bad "hao-embed: $(cat "$OUT/embed.log")"
check fixture "$E" Fix
timeout 60 codesign --verify --strict --arch x86_64 "$E" 2>"$OUT/verify.log" &&
    pass "fixture: codesign --verify --strict --arch x86_64 on the bundle" || bad "fixture verify: $(cat "$OUT/verify.log")"
timeout 30 codesign -d --entitlements - --xml --arch x86_64 "$E" 2>/dev/null | grep -q "com.apple.security.app-sandbox" &&
    pass "fixture: the stub keeps the app-sandbox entitlement" || bad "fixture: app-sandbox entitlement lost"
if same_slices "$F/Contents/MacOS/Fix" "$E/Contents/MacOS/Fix" && [ "$(others "$F" Fix)" = "$(others "$E" Fix)" ]; then
    pass "fixture: arm64e slice and every other bundle file byte-identical"
else bad "fixture: arm64e slice or bundle files changed"; fi

# --- a bundle whose Info.plist no longer matches its signature: refused
X=$OUT/stale/Fix.app; mkdir -p "$OUT/stale"; cp -R "$F" "$X"; echo >>"$X/Contents/Info.plist"
"$EMBED" --stub "$B/hao-stub" --in-place "$X/Contents/MacOS/Fix" >"$OUT/stale.log" 2>&1
if grep -q "^fail.*Info.plist" "$OUT/stale.log" && ! lipo -archs "$X/Contents/MacOS/Fix" | grep -q x86_64; then
    pass "Info.plist changed after signing: hao-embed refuses ($(cut -f3 "$OUT/stale.log"))"
else bad "stale Info.plist: $(cat "$OUT/stale.log")"; fi

# --- the payload route: the manifest records the bound files; hao-install.py embeds
# only into a bundle whose Info.plist and CodeResources match (fail closed)
d=$OUT/install; mkdir -p "$d/src/Applications" "$d/pay/root/usr/lib/hao"
cp -R "$F" "$d/src/Applications/"
"$EMBED" --stub "$B/hao-stub" --stubs "$d/pay/embed" --root "$d/src" --x86-cache "$X86" --require Applications/Fix.app/Contents/MacOS/Fix \
    "$d/src/Applications/Fix.app/Contents/MacOS/Fix" >"$d/embed.log" 2>&1
python3 -I - "$d/pay/embed/manifest.json" "$d/src/Applications/Fix.app/Contents" <<'PY' &&
import hashlib, json, sys
e = json.load(open(sys.argv[1]))["files"][0]
h = lambda n: hashlib.sha256(open(sys.argv[2] + "/" + n, "rb").read()).hexdigest()
want = [{"slot": -1, "sha256": h("Info.plist"), "file": "Applications/Fix.app/Contents/Info.plist"},
        {"slot": -3, "sha256": h("_CodeSignature/CodeResources"), "file": "Applications/Fix.app/Contents/_CodeSignature/CodeResources"}]
sys.exit(0 if e["bound"] == want else "bound " + repr(e["bound"]))
PY
    pass "manifest: bound Info.plist and CodeResources with their hashes" || bad "manifest bound list"
for kind in same infoplist coderesources; do
    s=$d/sys-$kind; mkdir -p "$s/usr/lib/hao"; cp -R "$d/src/Applications" "$s/"
    case $kind in
    infoplist) echo >>"$s/Applications/Fix.app/Contents/Info.plist" ;;
    coderesources) echo >>"$s/Applications/Fix.app/Contents/_CodeSignature/CodeResources" ;;
    esac
    python3 -I "$repo/tools/hao-install.py" "$s" "$d/pay" >"$d/$kind.log" 2>&1; r=$?
    archs=$(lipo -archs "$s/Applications/Fix.app/Contents/MacOS/Fix")
    if [ $kind = same ]; then
        if [ $r = 0 ] && echo "$archs" | grep -q x86_64 && grep -q '"bundle_bound": 1' "$s/usr/lib/hao/install.json"; then
            pass "hao-install.py, matching bundle: embedded, bundle_bound 1"
            check installed "$s/Applications/Fix.app" Fix
        else bad "hao-install.py, matching bundle: rc $r, $archs: $(cat "$d/$kind.log")"; fi
    elif [ $r = 1 ] && ! echo "$archs" | grep -q x86_64 && grep -q "differs from the one the stub binds; not embedded" "$d/$kind.log"; then
        pass "hao-install.py, changed $kind: refused, not embedded"
    else bad "hao-install.py, changed $kind: rc $r, $archs: $(cat "$d/$kind.log")"; fi
done

# --- the 27.0 MenuBarAgent.app (arm64-only, app sandbox, secinit SIGILL without the bundle binding)
M=$HAO_FS/System/Library/CoreServices/MenuBarAgent.app
if [ -n "$HAO_FS" ] && [ -f "$M/Contents/MacOS/MenuBarAgent" ]; then
    mkdir -p "$OUT/mba"; cp -R "$M" "$OUT/mba/"; A=$OUT/mba/MenuBarAgent.app
    "$EMBED" --stub "$B/hao-stub" -o "$OUT/mba/MenuBarAgent" "$M/Contents/MacOS/MenuBarAgent" >"$OUT/mba.log" 2>&1
    cp "$OUT/mba/MenuBarAgent" "$A/Contents/MacOS/MenuBarAgent"
    grep -q "^ok" "$OUT/mba.log" && pass "MenuBarAgent: $(cut -f1,3- "$OUT/mba.log")" || bad "MenuBarAgent: $(cat "$OUT/mba.log")"
    check MenuBarAgent "$A" MenuBarAgent
    if same_slices "$M/Contents/MacOS/MenuBarAgent" "$A/Contents/MacOS/MenuBarAgent" && [ "$(others "$M" MenuBarAgent)" = "$(others "$A" MenuBarAgent)" ]; then
        pass "MenuBarAgent: arm64e slices ($(lipo -archs "$M/Contents/MacOS/MenuBarAgent")) and every other bundle file byte-identical"
    else bad "MenuBarAgent: arm64e slice or bundle files changed"; fi
else echo "SKIP MenuBarAgent (HAO_FS: no macOS 27 System at '$HAO_FS')"; fi
echo "bundle: $fails failed"
exit $((fails != 0))
