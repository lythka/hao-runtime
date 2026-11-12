#!/bin/sh
# Layout contracts of hao's Swift value walk (tests/host/swiftlayout.swift), measured in native arm64 and
# x86_64 programs, and the walk's verdicts over the x86_64 values (libhao's
# hao_sv_collect_empty_patches_detail):
# A. an Array buffer word supplies a struct's extra inhabitants for any Element (XI0 0; XI k k on arm64,
#    2k on x86_64); Optional<S>.none writes only that word; a live Array's elements are not walked.
# B. a multi-payload enum of class-reference words with its tag in bits 60-63 and no empty case (also the
#    SwiftUI type of that shape) is walked through native witnesses; a 17-case enum (tag bits differ), an
#    enum with an empty case and an Optional of such an enum (its nil encodings differ) stay refused.
# C. a multi-payload enum with a trailing tag byte equal to the case index is walked by layout; two empty
#    cases, a size or xi that does not match and a tag byte past the cases stay refused.
# D. a value of fewer than 8 bytes (a single-payload or spare-bit enum, a struct of them, an Optional of
#    one, Optional<Bool>) is walked with no patch.
# Every measured line must be equal on both architectures except the listed keys, the contract lines must
# hold on each, and the walk lines must be exactly the expected ones.
#   OUT=dir [LIB=build] tests/host/swiftlayout.sh   (Apple silicon host: the arm64 program runs natively)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
LIB=$(cd "${LIB:-${B:-$repo/build}}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftlayout)}
mkdir -p "$OUT"
for a in arm64 x86_64; do
    xcrun swiftc -O -target $a-apple-macos27.0 -o "$OUT/swiftlayout.$a" "$here/swiftlayout.swift" 2>"$OUT/build.$a.err" ||
        { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/swiftlayout.$a" 2>/dev/null
done
timeout -s KILL 60 arch -arm64 "$OUT/swiftlayout.arm64" >"$OUT/arm64" 2>"$OUT/arm64.err" || { echo "FAIL arm64 run"; cat "$OUT/arm64.err"; exit 1; }
timeout -s KILL 60 "$OUT/swiftlayout.x86_64" "$LIB/libhao.dylib" >"$OUT/x86_64" 2>"$OUT/x86_64.err" || { echo "FAIL x86_64 run"; cat "$OUT/x86_64.err"; exit 1; }
fail=0
a5="a5a5a5a5a5a5a5a5"; z="0000000000000000"

# Measurements: keys (kind, label, index) whose lines may differ between the architectures.
key() { grep -v '^W ' "$1" | awk '{ k = $1 " " $2; if ($1 == "X" || $1 == "C") k = k " " $3; print k " | " $0 }' | sort; }
key "$OUT/arm64" >"$OUT/arm64.keyed"; key "$OUT/x86_64" >"$OUT/x86_64.keyed"
comm -3 "$OUT/arm64.keyed" "$OUT/x86_64.keyed" | sed 's/^[[:space:]]*//; s/ | .*//' | sort -u >"$OUT/differ"
{
    for e in Entry Trend Box AnyObject NSObject Any; do echo "X Array<$e> 1"; echo "X Array<$e> 2"; done
    echo "X Sparkline 1"; echo "X Sparkline 2"; echo "C Optional<Optional<Holder<Box>>>#1 f"
    for t in Content Five Seventeen SpareEmpty SpareArray SpareNested Optional\<Content\> SwiftUI.Content \
             Optional\<SwiftUI.Content\>; do echo "L $t"; done
    echo "X Content 0"; echo "X SwiftUI.Content 0"
    for c in 1 3 5 7 9 11 13 15; do echo "C Seventeen#$c z"; done
    echo "V SpareNested.b(outerNil)"; echo "V TextNil.two(outerNil)"
} | sort -u >"$OUT/differ.expected"
if ! cmp -s "$OUT/differ" "$OUT/differ.expected"; then
    echo "FAIL arm64/x86_64 differences other than expected:"; diff "$OUT/differ.expected" "$OUT/differ" | sed 's/^/    /'; fail=1
fi

# Contract lines on both architectures, then on each.
both=$(cat <<EOF
L Array<Entry> size=8 stride=8 flags=0x10007 xi=0x7fffffff
L Array<Trend> size=8 stride=8 flags=0x10007 xi=0x7fffffff
L Array<Box> size=8 stride=8 flags=0x10007 xi=0x7fffffff
L Array<AnyObject> size=8 stride=8 flags=0x10007 xi=0x7fffffff
L Array<NSObject> size=8 stride=8 flags=0x10007 xi=0x7fffffff
L Array<Any> size=8 stride=8 flags=0x10007 xi=0x7fffffff
X Array<Entry> 0 $z
X Array<Box> 0 $z
X Array<Any> 0 $z
C Optional<Sparkline>#1 f $z $a5 $a5 $a5 $a5 $a5 $a5 $a5 a5 back=1
C Optional<Holder<Box>>#1 f $z $a5 back=1
C Content#0 z $z $z back=0
C Content#1 z $z 8000000000000000 back=1
C SwiftUI.Content#0 z $z $z back=0
C SwiftUI.Content#1 z $z 8000000000000000 back=1
C Five#1 z 2000000000000000 back=1
C Five#2 z 4000000000000000 back=2
C Five#3 z 6000000000000000 back=3
C Five#4 z 8000000000000000 back=4
V Content.view P $z
V Content.styled P 8000000000000000|P
L SecondaryText size=33 stride=40 flags=0x2230007 xi=0xfd
C SecondaryText#0 f $a5 $a5 $a5 $a5 00 back=0
C SecondaryText#1 f $a5 $a5 $a5 $a5 01 back=1
C SecondaryText#2 f $z $z $z $z 02 back=2
X SecondaryText 0 $a5 $a5 $a5 $a5 ff
X SecondaryText 1 $a5 $a5 $a5 $a5 fe
L TwoEmpty size=33 stride=40 flags=0x2230007 xi=0xfd
C TwoEmpty#3 f 0000000000000001 $z $z $z 02 back=3
L TextNil size=25 stride=32 flags=0x2230007 xi=0xfd
L Kind size=1 stride=1 flags=0x200000 xi=0xfc
L Flags size=2 stride=2 flags=0x200000 xi=0x3ffc
L Small size=6 stride=6 flags=0x1 xi=0x3ffc
L Optional<Kind> size=1 stride=1 flags=0x200000 xi=0xfb
EOF
)
arm=$(cat <<EOF
X Array<Entry> 1 0000000000000001
X Array<Entry> 2 0000000000000002
X Array<Box> 1 0000000000000001
X Array<Any> 1 0000000000000001
X Sparkline 1 0000000000000001 $a5 $a5 $a5 $a5 $a5 $a5 $a5 a5
L Content size=16 stride=16 flags=0x210007 xi=0x3ffe
L SwiftUI.Content size=16 stride=16 flags=0x210007 xi=0x3ffe
X Content 0 f000000000000007 f000000000000007
X SwiftUI.Content 0 f000000000000007 f000000000000007
C Seventeen#1 z 0000000000000004 back=1
V TextNil.two(outerNil) 0000000000000001 0000000000000078 e100000000000000 01
EOF
)
intel=$(cat <<EOF
X Array<Entry> 1 0000000000000002
X Array<Entry> 2 0000000000000004
X Array<Box> 1 0000000000000002
X Array<Any> 1 0000000000000002
X Sparkline 1 0000000000000002 $a5 $a5 $a5 $a5 $a5 $a5 $a5 a5
L Content size=16 stride=16 flags=0x210007 xi=0x3ffffe
L SwiftUI.Content size=16 stride=16 flags=0x210007 xi=0x3ffffe
X Content 0 ff00000000000007 ff00000000000007
X SwiftUI.Content 0 ff00000000000007 ff00000000000007
C Seventeen#1 z 0800000000000000 back=1
V TextNil.two(outerNil) 0000000000000002 0000000000000078 e100000000000000 01
EOF
)
need() {   # ARCH LINES
    printf '%s\n' "$2" | while IFS= read -r line; do
        grep -qxF -- "$line" "$OUT/$1" || echo "FAIL $1: missing: $line"
    done
}
need arm64 "$both" >"$OUT/contract"; need x86_64 "$both" >>"$OUT/contract"
need arm64 "$arm" >>"$OUT/contract"; need x86_64 "$intel" >>"$OUT/contract"
[ -s "$OUT/contract" ] && { cat "$OUT/contract"; fail=1; }

# Walk verdicts over the x86_64 values.
cat >"$OUT/walk.expected" <<'EOF'
W Optional<Sparkline> nil walk 1 0:0>0
W Optional<Sparkline> some walk 0
W Optional<Holder<Entry>> nil walk 1 0:0>0
W Optional<Holder<Trend>> nil walk 1 0:0>0
W Optional<Holder<Box>> nil walk 1 0:0>0
W Optional<Holder<Box>> some walk 0
W Optional<Holder<AnyObject>> nil walk 1 0:0>0
W Optional<Holder<NSObject>> nil walk 1 0:0>0
W Optional<Holder<NSObject>> some walk 0
W Optional<Holder<Any>> nil walk 1 0:0>0
W Optional<Optional<Holder<Box>>> outer nil walk 1 0:2>2
W Optional<Optional<Holder<Box>>> inner nil walk 1 0:0>0
W Optional<Holder<NSObject??>> some, elements nil walk 0
W Content.view walk 0
W Content.styled walk 0
W Five#0 walk 0
W Five#1 walk 0
W Five#2 walk 0
W Five#3 walk 0
W Five#4 walk 0
W SpareArray.b(nil) walk 1 0:0>0
W SpareArray.b(some) walk 0
W SpareArray.a walk 0
W Seventeen.c1 (stays refused) refused Seventeen
W Optional<Content> nil (stays refused) refused Optional<Content>
W SpareEmpty.b (stays refused) refused SpareEmpty
W SpareNested.b(outer nil) walk 1 8:2>2
W SpareNested.b(inner nil) walk 1 8:0>0
W SwiftUI.Content view walk 0
W SwiftUI.Content styled walk 0
W Optional<SwiftUI.Content> nil (stays refused) refused Optional<Content>
W SecondaryText.one(small) walk 0
W SecondaryText.one(large) walk 0
W SecondaryText.two(small) walk 0
W SecondaryText.two(large) walk 0
W SecondaryText.none walk 0
W TextNil.one walk 0
W TextNil.two(outer nil) walk 1 0:2>2
W TextNil.two(inner nil) walk 1 0:0>0
W TextNil.two(object) walk 0
W TextNil.none walk 0
W TwoEmpty.one (stays refused) refused TwoEmpty
W Optional<SecondaryText> nil (stays refused) refused Optional<SecondaryText>
W SecondaryText size+1 (stays refused) refused SecondaryText
W SecondaryText xi-1 (stays refused) refused SecondaryText
W SecondaryText tag 3 (stays refused) refused SecondaryText
W Kind.clear walk 0
W Kind.cloudy walk 0
W Kind.snow walk 0
W Flags.a walk 0
W Flags.c walk 0
W Flags.none walk 0
W Small walk 0
W Optional<Kind> nil walk 0
W Optional<Bool> nil walk 0
W Optional<Bool> some walk 0
EOF
grep '^W ' "$OUT/x86_64" >"$OUT/walk" || true
if ! cmp -s "$OUT/walk.expected" "$OUT/walk"; then
    echo "FAIL walk verdicts:"; diff "$OUT/walk.expected" "$OUT/walk" | sed 's/^/    /'; fail=1
fi
echo "measured $(grep -vc '^W ' "$OUT/arm64") lines arm64, $(grep -vc '^W ' "$OUT/x86_64") x86_64; $(wc -l <"$OUT/walk" | tr -d ' ') walks"
[ $fail = 0 ] && echo "swiftlayout: PASS" || echo "swiftlayout: FAIL"
exit $fail
