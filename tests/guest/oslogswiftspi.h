/*
 * libswiftos SPI that the os overlay's os_log() and os_signpost() call, for guest code that calls it directly
 * (tests/guest/oslog.c from a C variadic, oslogswift.swift from an inlined withVaList). Both take an arm64
 * va_list in the guest; the format and va_list of _swift_os_signpost_with_format are its 7th and 8th
 * arguments.
 */
#include <os/log.h>
#include <os/signpost.h>
#include <stdarg.h>

void _swift_os_log(const void *dso, const void *ra, os_log_t log, os_log_type_t type, const char *fmt, va_list args);
void _swift_os_signpost_with_format(const void *dso, const void *ra, os_log_t log, os_signpost_type_t type,
                                    const char *name, os_signpost_id_t spid, const char *fmt, va_list args);
