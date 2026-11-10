#!/bin/bash
# os_log records from guest code (src/oslog.c): what `log show` prints for a guest program's own messages,
# against the same file's arm64 slice run natively.
#
#   oslog       tests/guest/oslog.c + osloghelper.c (a guest dylib: no x86_64 slice): every log type,
#               public/private/sensitive/plain strings, numbers, objects, precision, value annotations,
#               mask.hash, %m, OS_LOG_DEFAULT, a second subsystem, signposts, _os_log_internal,
#               _os_log_send_and_compose_impl
#   oslogswift  tests/guest/oslogswift.swift: Logger with privacy, os_log() and os_signpost() (libswiftos
#               sends a pack naming the guest's dso)
#
# Each program is embedded (tools/hao-embed --host-test) and run as `arch -arm64` (native) and
# `arch -x86_64` under JIT, --interp (HAO_FLAGS=i) and AOT (HAO_CACHE). Its records are read back with
# `log show --style compact --info --debug --signpost` by process ID, and compared line by line with the
# timestamp, PID and thread ID removed. One difference is by design and is normalised (KNOWN): a guest
# signpost's name goes in front of its message and the record's own name is empty, which log show prints
# as ": NAME: message" where natively it prints "NAME: message". Also normalised: a mask.hash value (salted
# per process: two native runs differ too) to its shape, 24 base64 characters, and libsystem_trace's own
# "Activity for state dumps" (native libraries' records, not the guest's). Every other difference fails, and so does a
# `<compose failure` line on either side. Before src/oslog.c every guest record was `<compose failure [UUID]>`.
# Mode oldstub (JIT) embeds with a front end that has no hao_oslog_format, as a stub linked before it: the
# records then come from libhao_identity's own format, which must also be one logd can resolve.
# MODES= picks modes (jit interp aot oldstub), PROGS= programs. Exit 1 on a failure, 2 on a setup error.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-oslog)}
HAO=${HAO:-$repo/build/hao}
B=${B:-$(dirname "$HAO")}
T=${T:-60}
MODES=${MODES:-jit interp aot oldstub}
PROGS=${PROGS:-oslog oslogswift}
rm -rf "$OUT"; mkdir -p "$OUT/plain" "$OUT/emb" "$OUT/emb-old" "$OUT/aot" "$OUT/oldstub/front"
cd "$OUT" || exit 2
fails=0

clang -arch arm64 -arch arm64e -O1 -dynamiclib -install_name "$OUT/libosloghelper.dylib" \
    -o libosloghelper.dylib "$here/osloghelper.c" && codesign -s - -f libosloghelper.dylib 2>/dev/null || exit 2
clang -arch arm64 -arch arm64e -O1 -Wno-format -Wno-deprecated-declarations -framework CoreFoundation \
    -o plain/oslog "$here/oslog.c" libosloghelper.dylib && codesign -s - -f plain/oslog 2>/dev/null || exit 2
for a in arm64 arm64e; do
    xcrun swiftc -target $a-apple-macos27.0 -O -o "oslogswift.$a" "$here/oslogswift.swift" 2>swift.log || { cat swift.log; exit 2; }
done
lipo -create -output plain/oslogswift oslogswift.arm64 oslogswift.arm64e && codesign -s - -f plain/oslogswift 2>/dev/null || exit 2
# a build directory whose front end lacks hao_oslog_format (tools/hao-embed relinks from its front/stub.o)
printf 'int hao_self_main(int, char **, char **, char **);\nint main(int c, char **v, char **e, char **a) { return hao_self_main(c, v, e, a); }\n' >oldstub.c
cp "$B/libhao.dylib" "$B/libhao_identity.dylib" oldstub/ || exit 2
min=$(otool -l "$B/front/stub.o" | awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" { print $2; exit }')
clang -arch x86_64 -mmacosx-version-min="${min:-27.0}" -O2 -c oldstub.c -o oldstub/front/stub.o &&
    clang -arch x86_64 -mmacosx-version-min="${min:-27.0}" -o oldstub/hao-stub oldstub/front/stub.o -Loldstub -lhao -lhao_identity \
        -Wl,-rpath,/usr/lib/hao -Wl,-no_adhoc_codesign || exit 2
if nm oldstub/hao-stub | grep -q hao_oslog_format; then echo "FAIL setup: oldstub has hao_oslog_format"; exit 2; fi
for p in $PROGS; do
    python3 "$repo/tools/hao-embed" --stub "$B/hao-stub" --host-test -o "emb/$p" "plain/$p" >/dev/null || exit 2
    python3 "$repo/tools/hao-embed" --stub "$OUT/oldstub/hao-stub" --host-test -o "emb-old/$p" "plain/$p" >/dev/null || exit 2
done
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" plain/oslog libosloghelper.dylib plain/oslogswift >aot.gen 2>&1 ||
    { echo "FAIL AOT generation: $(head -c 300 aot.gen)"; exit 2; }
for p in $PROGS; do
    case " $MODES " in *" aot "*)
        if ! HAO_CACHE="$OUT/aot" HAO_VERBOSE=1 timeout -s KILL "$T" arch -x86_64 "emb/$p" aotcheck 2>&1 >/dev/null |
            grep -q "AOT cache $OUT/aot/.* loaded"; then echo "FAIL $p aot: the AOT cache was not loaded"; fails=$((fails + 1)); fi ;;
    esac
done

# run PROG MODE: the program's records, normalised, in rec.PROG.MODE
run() {
    local p=$1 m=$2 tag="hao-oslog-$1-$2-$$" pid
    case $m in
    native) timeout -s KILL "$T" arch -arm64 "emb/$p" "$tag" >"out.$p.$m" 2>&1 ;;
    jit) timeout -s KILL "$T" arch -x86_64 "emb/$p" "$tag" >"out.$p.$m" 2>&1 ;;
    interp) HAO_FLAGS=i timeout -s KILL "$T" arch -x86_64 "emb/$p" "$tag" >"out.$p.$m" 2>&1 ;;
    aot) HAO_CACHE="$OUT/aot" timeout -s KILL "$T" arch -x86_64 "emb/$p" "$tag" >"out.$p.$m" 2>&1 ;;
    oldstub) timeout -s KILL "$T" arch -x86_64 "emb-old/$p" "$tag" >"out.$p.$m" 2>&1 ;;
    esac
    pid=$(head -1 "out.$p.$m")
    case $pid in ''|*[!0-9]*) echo "FAIL $p $m: no PID ($(head -c 200 "out.$p.$m"))"; fails=$((fails + 1)); return 1 ;; esac
    # the store takes a moment; wait for the last record
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        log show --last 5m --info --debug --signpost --style compact --predicate "processID == $pid" >"raw.$p.$m" 2>&1
        grep -q "$tag swift end\|$tag end" "raw.$p.$m" && break
    done
    # Timestamp Ty Process[PID:TID] rest -> Ty Process rest; signpost IDs by order of appearance
    sed -n '2,$p' "raw.$p.$m" | sed -E 's/^[0-9-]+ [0-9:.]+ ([A-Za-z]+) +([^[]+)\[[0-9]+:[0-9a-f]*\] /\1 \2 /' |
        sed -e "s/$tag/TAG/g" -E -e "s/<mask\.hash: '[A-Za-z0-9+\/=]{24}'>/<mask.hash: HASH>/" |
        grep -v '^A .* (libsystem_trace\.dylib) Activity for state dumps$' | awk '{
            while (match($0, /spid 0x[0-9a-f]+/)) {
                id = substr($0, RSTART + 5, RLENGTH - 5)
                if (!(id in seen)) seen[id] = "S" (++k)
                $0 = substr($0, 1, RSTART - 1) "spid=" seen[id] substr($0, RSTART + RLENGTH)
            }
            print }' >"rec.$p.$m"
}

for p in $PROGS; do
    run "$p" native || continue
    if grep -q "compose failure" "rec.$p.native"; then echo "FAIL $p native: compose failure"; fails=$((fails + 1)); fi
    if [ "$(wc -l <"rec.$p.native")" -lt 8 ]; then
        echo "FAIL $p native: only $(wc -l <"rec.$p.native") records"; cat "raw.$p.native" | head; fails=$((fails + 1)); continue
    fi
    for m in $MODES; do
        run "$p" "$m" || continue
        # KNOWN: a guest signpost's name is carried in its message (src/oslog.c)
        # oldstub: the sender is libhao_identity, which compact style names where there is no subsystem
        sed -E -e 's/^(Sp .*, (begin|event|  end)\] ): /\1/' \
            -e "$( [ "$m" = oldstub ] && echo 's/^([A-Za-z]+ [^ ]+) \(libhao_identity\.dylib\) /\1 /' || echo 's/^$//')" \
            "rec.$p.$m" >"cmp.$p.$m"
        if cmp -s "rec.$p.native" "cmp.$p.$m" && ! grep -q "compose failure" "rec.$p.$m"; then
            echo "PASS $p $m ($(wc -l <"rec.$p.native" | tr -d ' ') records)"
        else
            echo "FAIL $p $m"; diff "rec.$p.native" "cmp.$p.$m" | head -40; fails=$((fails + 1))
        fi
        if ! cmp -s <(sed 1d "out.$p.native") <(sed 1d "out.$p.$m"); then
            echo "FAIL $p $m: stdout differs"; diff <(sed 1d "out.$p.native") <(sed 1d "out.$p.$m") | head; fails=$((fails + 1))
        fi
    done
done
[ "$fails" -eq 0 ] || exit 1
