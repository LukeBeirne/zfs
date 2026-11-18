# Zalgo DPUSM synchronous integration

This note records the contract used by the current synchronous DPUSM
experiment. It is intentionally narrower than a production accelerator ABI.

## Revisions

The experiment was recorded against:

- Zalgo/ZFS `f5de03a19378e1289a462e1cf1147404dd826f22`
  (`zalgo: pool-aware DPUSM dispatch`)
- DPUSM `beb999eb52ddbeaa0cb796b7a27db5fd7dc746ff`
  (`Added ashift value to file_write`)

The Zalgo integration uses the DPUSM user-function table from
`include/dpusm/user_api.h` and capability/result definitions from
`include/dpusm/common.h`.

## Scope and selection

The current implementation exposes DPUSM as an accelerated provider of the
existing Zalgo Fletcher4 and Fletcher4-byteswap checksum subtypes. It does not
introduce a new algorithm subtype. Dataset and block metadata continue to
select the checksum algorithm; the pool context selects whether a DPUSM
provider may implement that algorithm.

The runtime controls are pool properties:

```text
dpusm_provider=<provider name>|off
dpusm_checksum=on|off
```

Both are runtime-only and are not written to pool MOS properties. A provider
must be selected again after import. A pool context contains the DPUSM
provider handle, provider name, Fletcher4 capability bits, checksum gate, and
an operation reference count.

The Zalgo registry contains one DPUSM operation-table registration per
Fletcher4 byte order. Those registrations are shared dispatch entries; the
pool context supplies the provider handle at operation time. This permits
different pools to select different DPUSM providers without replacing the
global Zalgo software fallback or changing the algorithm subtype.

## Synchronous operation contract

For each checksum operation:

1. ZFS passes the pool (`spa_t`), ABD, length, and byte-order selection to the
   pool-aware Zalgo fallback helper.
2. The helper holds the selected Zalgo node. The DPUSM operation callback then
   holds the pool context, which prevents provider replacement or pool teardown
   from freeing the provider while the callback runs.
3. The shim linearizes the requested ABD range into temporary memory using
   `abd_borrow_buf_copy()`.
4. It allocates a temporary DPUSM handle, copies the linear input into that
   handle, and submits the synchronous checksum operation.
5. The temporary handle is freed and the ABD borrow is returned before the
   provider-context reference is released.
6. Any non-success DPUSM result is returned as an acceleration failure. The
   caller retries through the software Zalgo provider and then the normal ZFS
   Fletcher4 implementation.

There is no persistent DPUSM-backed ABD state, asynchronous completion,
request queue ownership, or lazy onload behavior in this scope.

## Capability and result policy

Provider selection requires DPUSM Fletcher4 support and at least one of native
or byteswapped checksum byte order. Unsupported byte orders are rejected at
selection time. A provider operation result other than `DPUSM_OK` currently
maps to an ordinary acceleration failure (`EIO` from the shim), including
provider invalidation, unregistration, unsupported requests, and bad results.
The software/ZFS fallback preserves normal checksum and redundancy behavior.

The implementation deliberately does not yet distinguish retryable provider
failures from checksum mismatches, authentication failures, or malformed
compressed data. That classification remains a separate follow-up.

The shim classifies synchronous DPUSM checksum failures internally as
unavailable, unsupported, invalidated, or generic provider failures. A pool
context counts consecutive operation failures; after three failures it
quarantines DPUSM checksum use for that pool by disabling its runtime checksum
gate. The next explicit provider selection or checksum-gate update can restore
use after the caller has decided the provider is healthy. Checksum mismatches
remain ZFS data-integrity results and are not classified as provider failures.

## Replacement and teardown

Provider replacement validates and acquires the new provider before detaching
the old pool context. The old context is marked draining, new operation holds
are prevented, active operation references are drained, and only then is the
old DPUSM handle returned.

Pool export/destruction performs the same context teardown after vdev and DSL
pool teardown. DPUSM module finalization drains all remaining pool contexts
before finalizing the DPUSM user-function table. Asynchronous teardown and
device-removal protocols are outside this synchronous experiment.

## Build boundaries

DPUSM remains optional and disabled unless configured with:

```sh
./configure --with-dpusm=<DPUSM_ROOT>
```

The configure path validates `<DPUSM_ROOT>/Module.symvers`, adds the DPUSM
include directory only to DPUSM-dependent compilation, and conditionally adds
the in-tree shim and software provider to Kbuild/libtool inputs. Builds
without DPUSM retain the normal Zalgo and ZFS software paths.
