#!/bin/bash
# Build the 26.5 OSS Libc test for arm64e (guest), x86_64 (reference) and arm64 (host native).
set -e
cd "$(dirname "$0")"
: "${OSS265:?Set OSS265 to the supplied 26.5 open-source input directory}"
L=$OSS265/Libc
INC="-I$L/fbsdcompat -I$L/darwin -I$L/locale -I$L/locale/FreeBSD -I$L/stdlib/FreeBSD -I$L/gen/FreeBSD"
SRC="stdlib/FreeBSD/qsort.c stdlib/FreeBSD/qsort_r.c stdlib/FreeBSD/heapsort.c stdlib/FreeBSD/heapsort_r.c stdlib/FreeBSD/merge.c stdlib/FreeBSD/radixsort.c stdlib/FreeBSD/bsearch.c string/FreeBSD/memmem.c string/FreeBSD/strnstr.c string/FreeBSD/strsep.c string/FreeBSD/strmode.c stdlib/FreeBSD/tsearch.c stdlib/FreeBSD/tfind.c stdlib/FreeBSD/tdelete.c stdlib/FreeBSD/twalk.c stdlib/FreeBSD/lsearch.c gen/FreeBSD/vis.c stdlib/FreeBSD/strtonum.c"
OUT=../../build/oss265; mkdir -p $OUT
for arch in arm64e x86_64 arm64; do
  objs=""
  for f in $SRC; do o=$OUT/$arch-$(basename $f .c).o; clang -arch $arch -O2 -w -include rename.h $INC -c $L/$f -o $o; objs="$objs $o"; done
  clang -arch $arch -O2 -w driver.c $objs -o $OUT/oss265.$arch
  codesign -s - -f $OUT/oss265.$arch 2>/dev/null
done
echo "built $OUT/oss265.{arm64e,x86_64,arm64}"
