/* Plug-ins for tests/guest/libval.sh: -DGUEST builds libgplug (arm64 only),
 * otherwise libnplug (universal); -DINJECT builds the DYLD_INSERT_LIBRARIES probe. */
#include <stdio.h>
#ifdef INJECT
__attribute__((constructor)) static void injected(void) { fprintf(stderr, "INJECTED\n"); }
#elif defined(GUEST)
int gplug_value(void) { return 7; }
#else
int plug_value(void) { return 42; }
#endif
