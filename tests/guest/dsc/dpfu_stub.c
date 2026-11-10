/* Link-time stand-in for /usr/lib/libdpfu.dylib (arm64e shared cache only). */
const char *DPFUGetDeviceModeString(int mode) { (void)mode; return 0; }
unsigned long DPFUSessionGetTypeID(void) { return 0; }
