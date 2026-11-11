#!/bin/bash
# Build HaoExec.kext (kernel-independent). Bind it to a kernel collection with
#   kext/stamp_identity.py BootKernelExtensions.kc build/HaoExec.kext
# (the image build does this after building the collection).
#   kext/build.sh [BUILD_DIR [MACOS_MIN]]   -> BUILD_DIR/HaoExec.kext (default build/)
# MACOS_MIN is the kext's deployment target, default 13.0: the kext every
# payload ships, for both the 27.0 and the Tahoe userspace, since it loads
# into the Darwin 27 kernel either way.
set -euo pipefail
MIN=${2:-13.0}
[[ "$MIN" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "kext/build.sh: bad MACOS_MIN $MIN" >&2; exit 2; }
H=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${1:-$H/build}"
OUT=$(cd "${1:-$H/build}" && pwd)/HaoExec.kext
cd "$H/kext"
SDK=$(xcrun --show-sdk-path)
mkdir -p "$OUT/Contents/MacOS"
clang -arch x86_64 -mmacosx-version-min="$MIN" -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
  -mkernel -nostdlib -fno-builtin -fno-common -fno-stack-protector -DKERNEL -D__KERNEL__ -DKERNEL_PRIVATE=0 \
  -I"$SDK/System/Library/Frameworks/Kernel.framework/Headers" \
  -Xlinker -kext -Xlinker -export_dynamic \
  -o "$OUT/Contents/MacOS/HaoExec" HaoExec/HaoExec.c -lkmod -lcc_kext
cp HaoExec/Info.plist "$OUT/Contents/Info.plist"
# Ad-hoc signature for the unstamped bundle. stamp_identity.py rewrites the binary for the target kernel
# afterwards, so the image that loads HaoExec decides how it is loaded; a failure here is still an error.
codesign -s - -f "$OUT" || { echo "kext/build.sh: codesign failed for $OUT" >&2; exit 1; }
echo "built $OUT (unstamped: run stamp_identity.py with the target BootKC)"
