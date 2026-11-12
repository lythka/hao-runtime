#!/bin/sh
# Layout contracts of hao's Swift value walk (tests/host/swiftlayout.swift), measured in native arm64 and
# x86_64 programs, and the walk's verdicts over the x86_64 values (libhao's
# hao_sv_collect_empty_patches_detail):
# A. an Array buffer word supplies a struct's extra inhabitants for any Element (XI0 0; XI k k on arm64,
#    2k on x86_64); Optional<S>.none writes only that word; a live Array's elements are not walked.
# B. a multi-payload enum of class-reference words with its tag in bits 60-63 and no empty case (also the
#    SwiftUI type of that shape) is walked through native witnesses; a 17-case enum (tag bits differ), an
#    enum with an empty case and an Optional of such an enum (its nil encodings differ) stay refused,
#    unless a table row gives both layouts (the SwiftUI type's Optional nil is translated, see N2).
# C. a multi-payload enum with a trailing tag byte equal to the case index is walked by layout; two empty
#    cases, a size or xi that does not match and a tag byte past the cases stay refused.
# F. a function value other than a block, under any number of Optionals, has identical nils on both and
#    is walked with no patch; a block's XIs differ (k against 2k). So does an Optional of a struct whose
#    XIs come from such a function (closure-typed fields of every kind: global actor, inout, async).
# G. a Clang-imported enum (ComparisonResult, NSLineBreakMode: native metadata) is its integer.
# S. survey mode (hao_sv_collect_survey_patches): a value with two refusing fields prints both on stderr
#    and is still refused.
# D. a value of fewer than 8 bytes (a single-payload or spare-bit enum, a struct of them, an Optional of
#    one, Optional<Bool>) is walked with no patch.
# E. a tag-byte enum with two or five empty cases is walked (its empty cases share a tag and keep their
#    index in the payload area); a bad index, tag or xi stays refused. Spare-bit tags decided by a table
#    row (mpenum_gen.inc): rows given explicitly (R lines: flags 1|2 admit empty cases and tags in a
#    Bool's spare bits; a row without flag 1 or 2 refuses even the 16-byte enum rule B admits; a row of
#    another XI count is ignored), the tables' lookups (T lookup), and native types of this host found in
#    them: spare-bit enums walked by their rows; types declared differently on arm64 (archdiff_gen.inc)
#    walked as before where resilient (guest code holds them only through native witnesses), refused
#    where frozen (Float16: alone, in a tuple, as an Optional nil) unless inside a native unfrozen struct.
#    Where this host's image is not the build the tables describe, the program prints "T LABEL none" and
#    that label's lines are skipped.
# N. nils of standard pointer-word types (Set/Dictionary storage, Any, any P, AnyHashable, AnyObject, Swift
#    class references, metatypes, URL; DateComponents' second level) are planned like Array's buffer word on
#    the plain walk too; raw pointers, OpaquePointer, Selector and ObjectIdentifier have one XI, 0, and
#    only their first Optional is planned; IndexPath's XIs are tag bytes, the same on both; Data's differ
#    and are translated from the mpenum table's masks (N2: native and arm64 nils of Data?, Data??, a struct
#    holding Data?, the patches' arm64 words after "~"; an XI arm64 has no encoding for is refused).
# Every measured line must be equal on both architectures except the listed keys, the contract lines must
# hold on each, and the walk, row and table lines must be exactly the expected ones.
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
# The tables' lookups are binary searches: rows strictly ascending by UUID, then offset.
for t in mpenum archdiff; do
    awk -F'[{", ]+' '/^    \{"/ { h = sprintf("%16s", substr($3, 3)); gsub(/ /, "0", h); o = $2 " " h; if (o <= last) { print "FAIL " FILENAME ": row out of order: " $0; bad = 1 } last = o }
                     END { exit bad }' "$repo/src/${t}_gen.inc" || fail=1
done

# Measurements: keys (kind, label, index) whose lines may differ between the architectures.
key() { grep -v '^[WRT] ' "$1" | awk '{ k = $1 " " $2; if ($1 == "X" || $1 == "C") k = k " " $3; print k " | " $0 }' | sort; }
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
    echo "X BAction 1"; echo "X BAction 2"
    echo "L BundleDescription"   # its XI count: pointer spare bits
    # N: pointer-word inhabitants k against 2k; DateComponents' come from an Optional (1, 2 against 2, 4);
    # Data's from its representation's spare bits.
    for t in Set\<String\> Dictionary\<String,Int\> Any anyNP AnyHashable AnyObject Box NSKeyValueObservation \
             Box.Type AnyObject.Type Any.Type URL; do echo "X $t 1"; echo "X $t 2"; done
    for k in 0 1 2; do echo "X DateComponents $k"; echo "X Data $k"; done
    echo "C Optional<DateComponents>#1 f"; echo "C Optional<Data>#0 f"; echo "C Optional<Data>#1 f"
    echo "L Data"; echo "L Optional<Data>"
    echo "C Optional<Optional<Data>>#0 f"; echo "C Optional<Optional<Data>>#1 f"; echo "L Optional<Optional<Data>>"
    echo "L DataBox"; echo "X DataBox 0"; echo "X DataBox 1"
} | sort -u >"$OUT/differ.expected"
# The native spare-bit enums of section E must have been measured on both (their lines are then equal).
for k in "C NWListener.State#4 z" "C BundleDescription#2 z" "C ResizeBehavior#1 z" "C ZoomBehavior#2 z"; do
    grep -q "^$k" "$OUT/arm64" && grep -q "^$k" "$OUT/x86_64" || { echo "FAIL not measured on both: $k"; fail=1; }
done
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
C TwoEmpty#2 f $z $z $z $z 02 back=2
C TwoEmpty#3 f 0000000000000001 $z $z $z 02 back=3
L FiveEmpty size=17 stride=24 flags=0x200007 xi=0xfd
C FiveEmpty#1 f $a5 $a5 01 back=1
C FiveEmpty#2 f $z $z 02 back=2
C FiveEmpty#3 f 0000000000000001 $z 02 back=3
C FiveEmpty#6 f 0000000000000004 $z 02 back=6
X FiveEmpty 0 $a5 $a5 ff
L Low2 size=9 stride=16 flags=0x200007 xi=0x7e
C Low2#1 z $z 80 back=1
L Low3E2 size=9 stride=16 flags=0x200007 xi=0x7c
C Low3E2#1 z $z 40 back=1
C Low3E2#2 z $z 80 back=2
C Low3E2#3 z $z c0 back=3
C Low3E2#4 z 0000000000000001 c0 back=4
L TextNil size=25 stride=32 flags=0x2230007 xi=0xfd
L Kind size=1 stride=1 flags=0x200000 xi=0xfc
L Flags size=2 stride=2 flags=0x200000 xi=0x3ffc
L Small size=6 stride=6 flags=0x1 xi=0x3ffc
L Optional<Kind> size=1 stride=1 flags=0x200000 xi=0xfb
L Action size=16 stride=16 flags=0x10007 xi=0x7fffffff
L Optional<Action> size=16 stride=16 flags=0x210007 xi=0x7ffffffe
L Optional<Optional<Action>> size=16 stride=16 flags=0x210007 xi=0x7ffffffd
L CAction size=8 stride=8 flags=0x7 xi=0x7fffffff
L BAction size=8 stride=8 flags=0x10007 xi=0x7fffffff
X Action 0 $z $a5
X Action 1 0000000000000001 $a5
X Action 2 0000000000000002 $a5
X CAction 0 $z
X CAction 1 0000000000000001
X CAction 2 0000000000000002
X BAction 0 $z
L Set<String> size=8 stride=8 flags=0x10007 xi=0x7fffffff
X Set<String> 0 $z
X Any 0 $a5 $a5 $a5 $z
X AnyHashable 0 $a5 $a5 $a5 $z $a5
X Box.Type 0 $z
X URL 0 $z $a5
L UnsafeRawPointer size=8 stride=8 flags=0x7 xi=0x1
X UnsafeRawPointer 0 $z
L Optional<UnsafeRawPointer> size=8 stride=8 flags=0x200007 xi=0x0
L ObjectIdentifier size=8 stride=8 flags=0x7 xi=0x1
L Selector size=8 stride=8 flags=0x7 xi=0x1
L OpaquePointer size=8 stride=8 flags=0x7 xi=0x1
L IndexPath size=17 stride=24 flags=0x2010007 xi=0xfc
X IndexPath 0 $a5 $a5 ff
X IndexPath 1 $a5 $a5 fe
C Optional<IndexPath>#1 f $a5 $a5 ff back=1
EOF
)
arm=$(cat <<EOF
X Array<Entry> 1 0000000000000001
X Array<Entry> 2 0000000000000002
X Array<Box> 1 0000000000000001
X Array<Any> 1 0000000000000001
X Sparkline 1 0000000000000001 $a5 $a5 $a5 $a5 $a5 $a5 $a5 a5
X BAction 1 0000000000000001
L Content size=16 stride=16 flags=0x210007 xi=0x3ffe
L SwiftUI.Content size=16 stride=16 flags=0x210007 xi=0x3ffe
X Content 0 f000000000000007 f000000000000007
X SwiftUI.Content 0 f000000000000007 f000000000000007
C Seventeen#1 z 0000000000000004 back=1
V TextNil.two(outerNil) 0000000000000001 0000000000000078 e100000000000000 01
X Set<String> 1 0000000000000001
X Any 1 $a5 $a5 $a5 0000000000000001
X AnyHashable 1 $a5 $a5 $a5 0000000000000001 $a5
X Box.Type 1 0000000000000001
X URL 1 0000000000000001 $a5
L Data size=16 stride=16 flags=0x2010007 xi=0xc
X Data 0 $z f000000000000000
C Optional<Data>#1 f $z f000000000000000 back=1
C Optional<Optional<Data>>#1 f $z b000000000000000 back=1
X DataBox 0 $z b000000000000000 $a5
EOF
)
intel=$(cat <<EOF
X Array<Entry> 1 0000000000000002
X Array<Entry> 2 0000000000000004
X Array<Box> 1 0000000000000002
X Array<Any> 1 0000000000000002
X Sparkline 1 0000000000000002 $a5 $a5 $a5 $a5 $a5 $a5 $a5 a5
X BAction 1 0000000000000002
L Content size=16 stride=16 flags=0x210007 xi=0x3ffffe
L SwiftUI.Content size=16 stride=16 flags=0x210007 xi=0x3ffffe
X Content 0 ff00000000000007 ff00000000000007
X SwiftUI.Content 0 ff00000000000007 ff00000000000007
C Seventeen#1 z 0800000000000000 back=1
V TextNil.two(outerNil) 0000000000000002 0000000000000078 e100000000000000 01
X Set<String> 1 0000000000000002
X Any 1 $a5 $a5 $a5 0000000000000002
X AnyHashable 1 $a5 $a5 $a5 0000000000000002 $a5
X Box.Type 1 0000000000000002
X URL 1 0000000000000002 $a5
L Data size=16 stride=16 flags=0x2010007 xi=0xfc
X Data 0 $z ff00000000000000
C Optional<Data>#1 f $z ff00000000000000 back=1
C Optional<Optional<Data>>#1 f $z bf00000000000000 back=1
X DataBox 0 $z bf00000000000000 $a5
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
W Optional<SwiftUI.Content> nil (translated: a table row) walk 2 0:ff00000000000007>ff00000000000007~f000000000000007 8:ff00000000000007>ff00000000000007~f000000000000007
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
W TwoEmpty.one walk 0
W Optional<SecondaryText> nil walk 0
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
W Optional<Action> nil walk 0
W Optional<Action> some walk 0
W Optional<Optional<Action>> outer nil walk 0
W Optional<Optional<Action>> inner nil walk 0
W Optional<CAction> nil walk 0
W Optional<Optional<CAction>> outer nil walk 0
W Row nils walk 0
W Row some walk 0
W Button walk 0
W Handlers nils walk 0
W Handlers some walk 0
W Optional<Optional<Handlers>> outer nil walk 0
W Optional<Optional<Handlers>> inner nil walk 0
W Optional<Optional<Handlers>> some walk 0
W ComparisonResult walk 0
W Optional<ComparisonResult> nil walk 0
W Optional<ComparisonResult> some walk 0
W Oriented walk 0
W Oriented nil walk 0
W TwoRefused survey refused Seventeen
W TwoEmpty.two walk 0
W TwoEmpty.none walk 0
W TwoEmpty.other walk 0
W FiveEmpty#0 walk 0
W FiveEmpty#1 walk 0
W FiveEmpty#2 walk 0
W FiveEmpty#3 walk 0
W FiveEmpty#4 walk 0
W FiveEmpty#5 walk 0
W FiveEmpty#6 walk 0
W FiveEmpty xi-1 (stays refused) refused FiveEmpty
W FiveEmpty empty index 5 (stays refused) refused FiveEmpty
W FiveEmpty tag 3 (stays refused) refused FiveEmpty
R Content.styled row none case 1 (AV, Style)
R Content.styled row {2,0,0x3ffffe,0} refused
R Content.styled row {2,0,0x3ffffe,4} refused
R Content.styled row {2,0,0x3ffffe,1} refused
R Content.styled row {2,0,0x3ffffe,3} case 1 (AV, Style)
R Content.styled row {2,0,0x3fffff,0} case 1 (AV, Style)
R Content.styled row {3,0,0x3ffffe,3} refused
R SpareEmpty.b row none refused
R SpareEmpty.b row {2,1,0x7fd,3} case 1 Box
R SpareEmpty.none row {2,1,0x7fd,3} case 2 empty
R SpareEmpty.none row {2,1,0x7fd,1} refused
R SpareEmpty.none row {2,1,0x7fe,3} refused
R Low2.b row none refused
R Low2.b row {2,0,0x7e,3} case 1 IntBool
R Low3E2.c row {3,2,0x7c,3} case 2 IntBool
R Low3E2.e1 row {3,2,0x7c,3} case 4 empty
R Seventeen.c1 row none refused
R Seventeen.c1 row {17,0,0x7ef,0} refused
T lookup archdiff ComputationalGraph.Output.Errors p
T lookup archdiff ComputationalGraph.Feature s
T lookup archdiff Charts._PrimitivePlottableKind.Storage s
T lookup archdiff Swift.Float16 s
T lookup archdiff unlisted -
T lookup mpenum FontParser.(private).GlyphMap {2,0,0x1fe,0}
T lookup mpenum MediaPlaybackCore.Event {4,0,0x7fffffff,4}
T lookup mpenum Network.NWListener.State {2,3,0x3d,7}
T lookup mpenum first {2,0,0x7fe,3}
T lookup mpenum last {4,0,0x7c,7}
T lookup mpenum unlisted -
T NWListener.State row {2,3,0x3d,7} live xi=0x3d
T BundleDescription row {2,1,0x7fd,3} live xi=0x7fd
T ResizeBehavior row {2,0,0x7e,7} live xi=0x7e
W ResizeBehavior#0 walk 0
W ResizeBehavior#1 walk 0
T ZoomBehavior row {3,0,0x7d,7} live xi=0x7d
W ZoomBehavior#0 walk 0
W ZoomBehavior#1 walk 0
W ZoomBehavior#2 walk 0
W NWListener.State.waiting walk 0
W NWListener.State.failed walk 0
W NWListener.State.setup walk 0
W NWListener.State.ready walk 0
W NWListener.State.cancelled walk 0
W BundleDescription.main walk 0
W BundleDescription.forClass walk 0
W BundleDescription.atURL walk 0
T ComputationalGraph.Output.Errors archdiff p size=17
W ComputationalGraph.Output.Errors walk 0
W ComputationalGraph.Output.Errors in a tuple walk 0
W ComputationalGraph.Output.Errors Optional nil walk 0
W ComputationalGraph.Output.Errors in a native unfrozen struct walk 0
T ComputationalGraph.Feature.DataType archdiff s size=1
W ComputationalGraph.Feature.DataType walk 0
W ComputationalGraph.Feature.DataType in a tuple walk 0
W ComputationalGraph.Feature.DataType Optional nil walk 0
W ComputationalGraph.Feature.DataType in a native unfrozen struct walk 0
T Swift.Float16 archdiff s size=0
W Swift.Float16 refused Float16
W Swift.Float16 in a tuple refused (Int, Float16)
W Swift.Float16 Optional nil refused Optional<Float16>
W Swift.Float16 in a native unfrozen struct walk 0
W N Optional<Set<String>> nil walk 1 0:0>0
W N Optional<Set<String>> some walk 0
W N Optional<Optional<Set<String>>> outer nil walk 1 0:2>2
W N Optional<Optional<Set<String>>> inner nil walk 1 0:0>0
W N Optional<Dictionary<String,Int>> nil walk 1 0:0>0
W N Optional<Dictionary<String,Int>> some walk 0
W N Optional<Optional<Dictionary<String,Int>>> outer nil walk 1 0:2>2
W N Optional<Optional<Dictionary<String,Int>>> inner nil walk 1 0:0>0
W N Optional<Any> nil walk 1 24:0>0
W N Optional<Any> some walk 0
W N Optional<Optional<Any>> outer nil walk 1 24:2>2
W N Optional<Optional<Any>> inner nil walk 1 24:0>0
W N Optional<anyNP> nil walk 1 24:0>0
W N Optional<anyNP> some walk 0
W N Optional<Optional<anyNP>> outer nil walk 1 24:2>2
W N Optional<Optional<anyNP>> inner nil walk 1 24:0>0
W N Optional<AnyHashable> nil walk 1 24:0>0
W N Optional<AnyHashable> some walk 0
W N Optional<Optional<AnyHashable>> outer nil walk 1 24:2>2
W N Optional<Optional<AnyHashable>> inner nil walk 1 24:0>0
W N Optional<AnyObject> nil walk 1 0:0>0
W N Optional<AnyObject> some walk 0
W N Optional<Optional<AnyObject>> outer nil walk 1 0:2>2
W N Optional<Optional<AnyObject>> inner nil walk 1 0:0>0
W N Optional<Box> nil walk 1 0:0>0
W N Optional<Box> some walk 0
W N Optional<Optional<Box>> outer nil walk 1 0:2>2
W N Optional<Optional<Box>> inner nil walk 1 0:0>0
W N Optional<NSKeyValueObservation> nil walk 1 0:0>0
W N Optional<NSKeyValueObservation> some walk 0
W N Optional<Optional<NSKeyValueObservation>> outer nil walk 1 0:2>2
W N Optional<Optional<NSKeyValueObservation>> inner nil walk 1 0:0>0
W N Optional<Box.Type> nil walk 1 0:0>0
W N Optional<Box.Type> some walk 0
W N Optional<Optional<Box.Type>> outer nil walk 1 0:2>2
W N Optional<Optional<Box.Type>> inner nil walk 1 0:0>0
W N Optional<AnyObject.Type> nil walk 1 0:0>0
W N Optional<AnyObject.Type> some walk 0
W N Optional<Optional<AnyObject.Type>> outer nil walk 1 0:2>2
W N Optional<Optional<AnyObject.Type>> inner nil walk 1 0:0>0
W N Optional<Any.Type> nil walk 1 0:0>0
W N Optional<Any.Type> some walk 0
W N Optional<Optional<Any.Type>> outer nil walk 1 0:2>2
W N Optional<Optional<Any.Type>> inner nil walk 1 0:0>0
W N Optional<URL> nil walk 1 0:0>0
W N Optional<URL> some walk 0
W N Optional<Optional<URL>> outer nil walk 1 0:2>2
W N Optional<Optional<URL>> inner nil walk 1 0:0>0
W N Optional<DateComponents> nil walk 1 0:2>2
W N Optional<DateComponents> some walk 2 0:0>0 16:0>0
W N Optional<Optional<DateComponents>> outer nil refused Optional<Optional<DateComponents>>
W N Optional<Optional<DateComponents>> inner nil refused Optional<Optional<DateComponents>>
W N Optional<UnsafeRawPointer> nil walk 1 0:0>0
W N Optional<UnsafeRawPointer> some walk 0
W N Optional<Optional<UnsafeRawPointer>> outer nil walk 0
W N Optional<Optional<UnsafeRawPointer>> inner nil walk 1 0:0>0
W N Optional<UnsafeMutableRawPointer> nil walk 1 0:0>0
W N Optional<UnsafeMutableRawPointer> some walk 0
W N Optional<Optional<UnsafeMutableRawPointer>> outer nil walk 0
W N Optional<Optional<UnsafeMutableRawPointer>> inner nil walk 1 0:0>0
W N Optional<OpaquePointer> nil walk 1 0:0>0
W N Optional<OpaquePointer> some walk 0
W N Optional<Optional<OpaquePointer>> outer nil walk 0
W N Optional<Optional<OpaquePointer>> inner nil walk 1 0:0>0
W N Optional<Selector> nil walk 1 0:0>0
W N Optional<Selector> some walk 0
W N Optional<Optional<Selector>> outer nil walk 0
W N Optional<Optional<Selector>> inner nil walk 1 0:0>0
W N Optional<ObjectIdentifier> nil walk 1 0:0>0
W N Optional<ObjectIdentifier> some walk 0
W N Optional<Optional<ObjectIdentifier>> outer nil walk 0
W N Optional<Optional<ObjectIdentifier>> inner nil walk 1 0:0>0
W N Optional<IndexPath> nil walk 0
W N Optional<IndexPath> some walk 0
W N Optional<Optional<IndexPath>> outer nil walk 0
W N Optional<Optional<IndexPath>> inner nil walk 0
W N Optional<Data> nil walk 1 8:ff00000000000000>ff00000000000000~f000000000000000
W N Optional<Data> some walk 0
W N Optional<Optional<Data>> outer nil walk 1 8:bf00000000000000>bf00000000000000~b000000000000000
W N Optional<Optional<Data>> inner nil walk 1 8:ff00000000000000>ff00000000000000~f000000000000000
W N2 DataBox nil walk 1 8:ff00000000000000>ff00000000000000~f000000000000000
W N2 DataBox some walk 0
W N2 Optional<DataBox> nil walk 1 8:bf00000000000000>bf00000000000000~b000000000000000
W N2 Optional<Data> arm64 nil walk 1 8:f000000000000000>ff00000000000000~f000000000000000
W N2 Optional<Optional<Data>> arm64 outer nil walk 1 8:b000000000000000>bf00000000000000~b000000000000000
W N2 Optional<Optional<Data>> arm64 inner nil walk 1 8:f000000000000000>ff00000000000000~f000000000000000
W N2 Data, 13 Optionals: outer nil (no arm64 encoding) refused Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Optional<Data>>>>>>>>>>>>>
EOF
grep '^[WRT] ' "$OUT/x86_64" >"$OUT/walk" || true
# Native types whose image on this host is another build than the tables': their lines are skipped.
sed -n 's/^T \(.*\) none$/\1/p' "$OUT/walk" >"$OUT/skipped"
if [ -s "$OUT/skipped" ]; then
    for f in walk walk.expected; do
        awk 'NR == FNR { s[++n] = $0; next }
             { for (i = 1; i <= n; ++i) if (index($0, "W " s[i]) == 1 || index($0, "T " s[i] " ") == 1) next; print }' \
            "$OUT/skipped" "$OUT/$f" >"$OUT/$f.kept" && mv "$OUT/$f.kept" "$OUT/$f"
    done
    echo "note: skipped (this host's image is not the tables' build): $(tr '\n' ' ' <"$OUT/skipped")"
fi
sed -n 's/^hao\[[^]]*\]: Swift value survey: \(refused at offset [0-9]* depth [0-9]*\):.* type=/S \1 /p' "$OUT/x86_64.err" >"$OUT/survey"
printf '%s\n' "S refused at offset 0 depth 1 swiftlayout.Seventeen" "S refused at offset 8 depth 1 swiftlayout.SpareEmpty" >"$OUT/survey.expected"
if ! cmp -s "$OUT/survey.expected" "$OUT/survey"; then
    echo "FAIL survey lines:"; diff "$OUT/survey.expected" "$OUT/survey" | sed 's/^/    /'; fail=1
fi
if ! cmp -s "$OUT/walk.expected" "$OUT/walk"; then
    echo "FAIL walk, row and table verdicts:"; diff "$OUT/walk.expected" "$OUT/walk" | sed 's/^/    /'; fail=1
fi
echo "measured $(grep -vc '^[WRT] ' "$OUT/arm64") lines arm64, $(grep -vc '^[WRT] ' "$OUT/x86_64") x86_64;" \
     "$(grep -c '^W ' "$OUT/walk") walks, $(grep -c '^R ' "$OUT/walk") row cases, $(grep -c '^T ' "$OUT/walk") table lines"
[ $fail = 0 ] && echo "swiftlayout: PASS" || echo "swiftlayout: FAIL"
exit $fail
