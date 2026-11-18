#!/bin/sh
# Guest-defined types' rows of the Swift value walk (tests/guest/svguest.swift), from a fixture written here
# with the built binary's own LC_UUID and descriptor offsets (test switch HAO_SWIFT_GUEST_FACTS, a
# TEST_SWITCHES build), its layouts and case images measured by the program itself natively on arm64 and
# x86_64, beside walk rows for the native types it holds (HAO_SWIFT_WALK_FACTS). With the rows "status" (a
# SAME row), "fault" (a CONVERT enum: case images, a Data? payload) and "record" (a CONVERT struct by its
# fields' rows) and "holder" (a SAME enum whose payload type has static metadata) must equal native arm64
# under JIT, --interp and AOT; without them, and with rows of another UUID, cdhash, offset, size or metadata
# initialization or a field the live type does not have, each is refused, and so is "holder" with a row
# naming other static metadata in the slice or built with its static metadata in writable __DATA.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svguest.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svguest)}
HAO=${HAO:-$repo/build-ts/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
# svguest.writable: the arm64e guest with its constant data (static metadata, witness tables) linked into
# writable __DATA instead of __DATA_CONST.
for a in arm64e arm64 x86_64 writable; do
    case $a in writable) t=arm64e; set -- -Xlinker -rename_section -Xlinker __DATA_CONST -Xlinker __const \
                             -Xlinker __DATA -Xlinker __const ;; *) t=$a; set -- ;; esac
    xcrun swiftc -O -parse-as-library -module-name svguest -target $t-apple-macos27.0 "$@" -o "$OUT/svguest.$a" \
        "$here/svguest.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svguest.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svguest.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svguest: aot generation failed"
# The rows: identity from the arm64e binary, layouts and images from both architectures' own runs.
timeout -s KILL "$T" "$OUT/svguest.arm64" images >"$OUT/arm64.images" &&
timeout -s KILL "$T" "$OUT/svguest.x86_64" images >"$OUT/x86_64.images" || { echo "svguest: images failed"; exit 2; }
identity() {   # uuid, cdhash and __TEXT address of $BIN
    uuid=$(dwarfdump --uuid "$BIN" | awk '{print $2; exit}')
    cdhash=$(codesign -dvvv "$BIN" 2>&1 | sed -n 's/^CDHash=//p')
    text=$(otool -l "$BIN" | awk '/segname __TEXT$/{getline; print $2; exit}')
}
offset() { printf '%x' $(( 0x$(nm "$BIN" | awk -v s="_\$s7svguest$1Mn" '$3==s{print $1}') - text )); }
metadata() {   # the static metadata's offset of type $1 (its symbol N), "-" for one the runtime completes
    m=$(nm "$BIN" | awk -v s="_\$s7svguest$1N" '$3==s{print $1}')
    case $(init $(echo $1 | sed 's/^[0-9]*//; s/[OV]$//' | tr A-Z a-z)) in
    NONE) printf '%x' $(( 0x$m - text + ${META:-0} )) ;; *) echo - ;; esac
}
layout() { awk -v n=$1 '$1=="layout" && $2==n{print $4, $5, $6, $7}' "$OUT/arm64.images"; }
init() { awk -v n=$1 -v f="${2:-}" '$1=="layout" && $2==n{print f ? ($3 == "NONE" ? "SINGLETON" : "NONE") : $3}' "$OUT/arm64.images"; }
images() {   # the cases of $1: arm64 image and mask, then x86_64's
    awk -v n=$1 -v p="$2" 'FNR==NR && $1==n{a[$2]=$3" "$4; next} $1==n{print p, $2, a[$2], $3, $4}' \
        "$OUT/arm64.images" "$OUT/x86_64.images"
}
# A row of type $1 (mangled suffix $2) with verdict $3: UUID $4, cdhash $5, its offset plus $6 and size plus
# $7, its metadata initialization the other one when INIT is set.
row() {
    set -- "$@" $(layout $1)
    printf 'guest %s %s %s %s %x - %s %s %s %s %s' "$4" "$5" $(init $1 "${INIT:-}") $(metadata $2) \
        $(( 0x$(offset $2) + $6 )) "$3" \
        $(( $8 + $7 )) "$9" "${10}" "${11}"
}
pnc=10Foundation20PersonNameComponentsV
write() {   # $1 the file, $2 UUID, $3 cdhash, $4 added to each offset, $5 to each size, $6 a field key the types lack
    k=${6:-$pnc}; d=${6:-10Foundation4DataVSg}
    {
        echo "$(row status 6StatusO SAME "$2" "$3" $4 $5) SAME -1"
        echo "field 0 $k - SAME"; echo "field 1 $pnc - SAME"; echo "field 2 - - SAME"; echo "field 3 - - SAME"
        echo "$(row fault 5FaultO CONVERT "$2" "$3" $4 $5) SAME -1"
        echo "field 0 $k - SAME"; echo "field 1 $d - CONVERT"; echo "field 2 - - SAME"; echo "field 3 - - SAME"
        images fault image
        row record 6RecordV CONVERT "$2" "$3" $4 $5; echo
        echo "field 0 Sq @0 CONVERT"; echo "field 1 $d - CONVERT"; echo "field 2 SSSg - SAME"; echo "field 3 Sa @1 SAME"
        row code 4CodeO SAME "$2" "$3" $4 $5; echo
        echo "field 0 SSSg - SAME"; echo "field 1 Si - SAME"; echo "field 2 - - SAME"; echo "field 3 - - SAME"
        echo "$(row holder 6HolderO SAME "$2" "$3" $4 $5) SAME -1"
        echo "field 0 - @3 SAME"; echo "field 1 $k - SAME"; echo "field 2 - - SAME"; echo "field 3 - - SAME"
    } >"$1"
}
BIN=$OUT/svguest.writable; identity
write "$OUT/writable.facts" "$uuid" "$cdhash" 0 0
BIN=$OUT/svguest.arm64e; identity
write "$OUT/guest.facts" "$uuid" "$cdhash" 0 0
write "$OUT/uuid.facts" "00000000-0000-0000-0000-000000000000" "$cdhash" 0 0
write "$OUT/cdhash.facts" "$uuid" 0000000000000000000000000000000000000000 0 0
write "$OUT/offset.facts" "$uuid" "$cdhash" 4 0
write "$OUT/size.facts" "$uuid" "$cdhash" 0 1
write "$OUT/field.facts" "$uuid" "$cdhash" 0 0 10Foundation14DateComponentsV
INIT=1; write "$OUT/init.facts" "$uuid" "$cdhash" 0 0; INIT=
META=16; write "$OUT/metadata.facts" "$uuid" "$cdhash" 0 0; META=
{   # the native types' rows, which the guest rows' fields name
    echo "type $pnc SAME"; echo "type SSSg SAME"; echo "type Si SAME"; echo "type 10Foundation4DataVSg CONVERT"
    images data "image 10Foundation4DataVSg"
} >"$OUT/walk.facts"
fail=0
for part in status holder nil fault record; do
    timeout -s KILL "$T" "$OUT/svguest.arm64" $part >"$OUT/native.$part.out" 2>/dev/null; nrc=$?
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot nofacts uuid cdhash offset size field init metadata writable; do
        facts=$OUT/guest.facts
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        nofacts) facts=/dev/null; set -- env HAO_NO_AOT=1 "$HAO" ;;
        *) facts=$OUT/$mode.facts; set -- env HAO_NO_AOT=1 "$HAO" ;;
        esac
        bin=$OUT/svguest.arm64e
        case $mode in metadata|writable) [ $part = holder ] || continue ;; esac
        [ $mode = writable ] && bin=$OUT/svguest.writable
        timeout -s KILL "$T" env HAO_SWIFT_WALK_FACTS="$OUT/walk.facts" HAO_SWIFT_GUEST_FACTS="$facts" "$@" \
            "$bin" $part >"$OUT/$mode.$part.out" 2>"$OUT/$mode.$part.err"; rc=$?
        case $mode in
        jit|interp|aot)
            if [ $rc = $nrc ] && cmp -s "$OUT/native.$part.out" "$OUT/$mode.$part.out"; then r=SAME; else r=DIFF; fail=1; fi ;;
        *)
            if [ $rc != 0 ] && [ ! -s "$OUT/$mode.$part.out" ] &&
               grep -qE 'Swift value boundary refused: .*type=(Swift.Optional<)?svguest.(Status|Fault|Record|Holder)' "$OUT/$mode.$part.err"
            then r=REFUSED; else r=NOT-REFUSED; fail=1; fi ;;
        esac
        printf '%-7s %-8s rc=%s %s\n' $part $mode "$rc" "$r"
        case $r in SAME|REFUSED) ;; *) tail -4 "$OUT/$mode.$part.err" | cut -c1-300 | sed 's/^/    /' ;; esac
    done
done
[ $fail = 0 ] && echo "svguest: PASS" || echo "svguest: FAIL"
exit $fail
