// _ContiguousArrayStorage<Any> metadata laid out as the runtime instantiates it, in __DATA_CONST (sealed after fixups)
    .section __DATA_CONST,__const
    .p2align 3
_sealed_meta_full:
    .quad "_$ss23_ContiguousArrayStorageCfD"
    .quad "_$sBoWV"
_sealed_meta:
    .quad _sealed_meta_isa
    .quad "_$ss28__ContiguousArrayStorageBaseCN"
    .quad __objc_empty_cache
    .quad 0
    .quad _sealed_meta_isa + 2
    .long 2, 0
    .long 32
    .short 7, 0
    .long 176, 24
    .quad "_$ss23_ContiguousArrayStorageCMn"
    .quad 0
    .quad "_$ss28__ContiguousArrayStorageBaseCABycfC"
    .quad 0                     // not exported; bridging only
    .quad 16
    .quad "_$ss23_ContiguousArrayStorageC16_doNotCallMeBaseAByxGyt_tcfC"
    .quad 0                     // not exported; bridging only
    .quad 0                     // not exported; bridging only
    .quad "_$ss23_ContiguousArrayStorageC16canStoreElements13ofDynamicTypeSbypXp_tF"
    .quad "_$ss23_ContiguousArrayStorageC17staticElementTypeypXpvg"
    .quad "_$sypN" + 8          // its address point
_sealed_meta_isa:
    .quad 0, 0, 0, 0, 0
    .text
    .p2align 2
    .globl _sealed_storage_metadata
_sealed_storage_metadata:
    adrp x0, _sealed_meta@PAGE
    add x0, x0, _sealed_meta@PAGEOFF
    ret
    .globl _sealed_storage_alloc
_sealed_storage_alloc:
    b _swift_allocObject
