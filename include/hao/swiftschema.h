#ifndef HAO_SWIFTSCHEMA_H
#define HAO_SWIFTSCHEMA_H
#include <stdint.h>
#include <stdio.h>
/* Failure-only schema inventory. Never reads a value or changes admission. */
void hao_swift_schema_dump(FILE *, const void *root, const void *enclosing,
                           const void *unresolved, uint64_t guest_lr, uint64_t witness);
#endif
