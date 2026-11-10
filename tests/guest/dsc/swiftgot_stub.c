/* link-time stand-in for an arm64e-only shared-cache Swift library (tests/guest/dscswift.sh): carries the install
 * name (-install_name) and the one type descriptor tests/guest/dsc/swiftgot.c anchors on (ANCHOR); never installed */
#ifndef ANCHOR
#error "ANCHOR"
#endif
char anchor[1] __asm__(ANCHOR);
