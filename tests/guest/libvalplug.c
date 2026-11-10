/* Plug-ins for tests/guest/libval.sh: -DGUEST builds libgplug (arm64 only),
 * otherwise libnplug (universal); -DINJECT builds the DYLD_INSERT_LIBRARIES probe.
 * -DGVALUE=n gives libgplug another value in the same layout (the file a
 * library is rewritten with). */
#include <stdio.h>
#ifdef INJECT
__attribute__((constructor)) static void injected(void) { fprintf(stderr, "INJECTED\n"); }
#elif defined(GUEST)
#ifndef GVALUE
#define GVALUE 7
#endif
int gplug_value(void) { return GVALUE; }
#else
int plug_value(void) { return 42; }
#endif
