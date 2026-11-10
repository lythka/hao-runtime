#pragma once
#include "Prefix.h"
#include "Recipe.h"
#include "Enum.h"
/* Fixture caller guarantees Copyable and native-compatible live fields.
 * Native enum discriminator, possibly foreign bounded nil payloads. */
int copy_enum_empty(void *destination, const void *source, const void *type, int preserve_encoding);
int take_enum_empty(void *destination, void *owned_source, const void *type, int preserve_encoding);
int destroy_enum_empty(void *owned_value, const void *type);
