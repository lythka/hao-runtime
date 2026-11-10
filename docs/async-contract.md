# Async contract interface

ABI 1 in `include/hao/async_contract.h` separates the policy for publishing a
Swift async result across the guest/native boundary from the runtime's
transport for it. The runtime contains the transport; the policy comes from an
optional provider library, supplied separately from this repository. The
runtime contains no API- or application-specific rules.

## Loading a provider

A package selects a provider by installing `libhao_async_contract.dylib`,
which exports `hao_async_contract_provider_v1`, beside the loaded
`libhao.dylib`. Without a provider, execution continues without adaptation. A
provider that is present but fails trust, loading or ABI validation aborts the
process; there is no silent fallback.

Package loading uses the AOT cache trust rules: file and ancestor ownership,
permissions, ACLs and no symlinks. Guarded and set-ID processes require
root-controlled paths and a root-owned provider; open processes also accept the
current user's trusted paths. The provider and its dependencies are executable
package code, so their provenance is a build and release responsibility.

`HAO_ASYNC_CONTRACT_LIBRARY` selects a provider explicitly in an open process,
for development. Guarded and set-ID processes ignore it. Neither the working
directory nor an application name selects a package.

## Provider contract

The provider receives the import symbol, argument registers, Swift context and
self, and the guest receiver's image UUID and VM address. It either declines,
or returns an eight-byte result location, the exact source and receiver words,
and the throwing convention. API-specific metadata checks, OS build binding,
receiver evidence and contract versioning belong in the provider.

A provider must establish the result type and size, producer semantics,
receiver representation, exclusive publication interval and context protocol.
An image UUID and address identify a compiled receiver; identity alone is not
semantic evidence.

## Transport

The runtime saves the invocation's async context link, substitutes a temporary
parent frame, and restores the original header before resuming the receiver.
Successful publications apply the admitted word substitution; error returns
never read result storage. Active records are consumed and freed before the
receiver runs. Throwing and non-throwing continuations use separate signatures.
No per-invocation thunk is added to a permanent cache. The transport relies on
Swift's single-resume protocol and does not recover from a corrupt or late
duplicate continuation.

## Diagnostics

With `HAO_BRIDGE_CHECK=1`, admitted invocations emit `async contract begin`
and `async contract end` JSON records. IDs are per process; the process ID in
the log prefix is part of the key. Begin records identify the guest UUID and
resume point and the selected words. End records are `converted`, `preserved`
or `error`; successful completions record the value read and the value
published. An admission without an end is pending, not a conversion. The
diagnostic sink may mirror the records to stderr and its file; read one stream.

[Releases](releases.md#async-provider) describes how a provider is packaged.
