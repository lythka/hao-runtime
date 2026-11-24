#!/bin/bash
set -euo pipefail
# Run from the fixture collection: Demo/ and BoundaryPackage/ beside this script.
base=$(cd "$(dirname "$0")" && pwd)
mode=${1:-build}
out=${2:-"$base/output"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
case "$mode" in
 build|clean)
  args=(build)
  if [ "$mode" = clean ]; then args=(clean build); fi
  /usr/bin/time -p "$DEVELOPER_DIR/usr/bin/xcodebuild" -project "$base/Demo/HaoStress.xcodeproj" -target HaoStress -sdk iphoneos -configuration Debug -jobs 2 ARCHS=arm64 CODE_SIGNING_ALLOWED=NO SYMROOT="$out/products" OBJROOT="$out/objects" "${args[@]}" 2>&1 | tee "$out/$mode.log"
  /usr/bin/xcrun lipo -verify_arch arm64 "$out/products/Debug-iphoneos/HaoStress.app/HaoStress"
  /usr/bin/file "$out/products/Debug-iphoneos/HaoStress.app/HaoStress"
  ;;
 tests)
  /usr/bin/time -p /usr/bin/xcrun swift test --package-path "$base/BoundaryPackage" --scratch-path "$out/package" 2>&1 | tee "$out/tests.log"
  ;;
 *) echo "Usage: $0 [build|clean|tests] [output-directory]" >&2; exit 2 ;;
esac
