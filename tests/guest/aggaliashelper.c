/* libaggaliasimpl.dylib for tests/guest/aggalias.sh: the defining library. Every function has an
 * ABI difference between the slices that the plain register bridge gets wrong, and its interface
 * unit (aggalias.sh) types it under these names only. libaggaliaspub.dylib re-exports each one
 * under another name (ld -alias aai_X aap_X: an export-trie re-export with an import name), as
 * Metadata.framework re-exports MetadataUtilities' __MDPlistX as _MDPlistX (mds,
 * _MDPlistDataGetBytePtr).
 *   aai_data   aggregate by value + out pointer, pointer result (_MDPlistDataGetBytePtr's shape)
 *   aai_type   aggregate by value, narrow (uint8_t) result (_MDPlistGetPlistObjectType)
 *   aai_index  aggregate by value + uint32_t, aggregate result (sret: x8 vs rdi)
 *   aai_area   HFA by value (d0-d3 vs 32 bytes on the stack) + a double (CGS -> SLS rect calls)
 *   aai_flag   bool result whose x86_64 code leaves 0x5a above al (narrow TYPED 'B')
 *   aai_dl     aai_data's shape, reached only through dlsym of its alias name
 */
#include "aggaliashelper.h"

static const void *data(aa_obj o, size_t *len)
{
    const uint8_t *p = (const uint8_t *)o.bytes + o.off;
    if (len) *len = *(const uint32_t *)p;
    return p + 4;
}
const void *aai_data(aa_obj o, size_t *len) { return data(o, len); }
const void *aai_dl(aa_obj o, size_t *len) { return data(o, len); }
uint8_t aai_type(aa_obj o) { return (uint8_t)(o.type ^ (uint8_t)o.aux); }
aa_obj aai_index(aa_obj o, uint32_t i)
{
    aa_obj r = {(const uint8_t *)o.bytes + 4 * i, o.aux + i, o.off + 8 * i, (uint8_t)(o.type + i)};
    return r;
}
double aai_area(aa_rect r, double scale) { return (r.size.w - r.origin.x) * (r.size.h - r.origin.y) * scale; }
#if defined(__x86_64__)
__asm__(".text\n.globl _aai_flag\n_aai_flag:\n  movabsq $0x5a5a5a5a5a5a5a5a, %rax\n  testl %edi, %edi\n  setg %al\n  ret\n");
#else
bool aai_flag(int v) { return v > 0; }
#endif
