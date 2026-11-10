#pragma once
#include "Prefix.h"
/* Experiment only: trusted initialized compiler metadata, matching case/field
 * schemas, Copyable values and native-compatible live references/String are
 * caller contracts. This proves selected enum storage/ownership classes; it
 * does not establish those contracts from arbitrary bytes or metadata. */
int common_leaf_value(const void *type, const void *value);
/* -1: no byte-XI Optional proof; 1: outer none; 0: payload must be walked.
 * Recognizing none never reads its inactive payload or certifies a live one. */
int common_byte_optional_case(const void *type, const void *value);
/* Exact ObjCRetainableBox metadata, not weak/foreign/existential storage. */
int common_objc_reference_type(const void *type);
int common_objc_reference_value(const void *type, const void *value);
