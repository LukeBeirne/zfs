# Zalgo DPUSM synchronous integration

This note records the contract used by the current synchronous DPUSM
experiment. It is intentionally narrower than a production accelerator ABI.

## Architectural decision for the synchronous experiment

DPUSM is implemented as an accelerated provider of existing Zalgo algorithm
subtypes, with the DPUSM service/device interface kept behind the provider
shim. It is not a new checksum subtype and it is not a separate ZFS-facing
service. Pool-local runtime context selects the provider, while dataset and
block metadata continue to select the algorithm. The software Zalgo provider
and normal ZFS implementation remain independent fallback layers.

This model is chosen because it preserves the on-disk algorithm identity,
allows different pools to select different providers, and keeps DPUSM
capabilities, buffer copying, provider handles, failure handling, and teardown
out of common ZFS algorithm code. It also provides a direct software fallback
for the first synchronous operation without changing the existing ZFS
checksum format or requiring every algorithm to gain a hardware-specific
subtype.

Rejected alternatives for this experiment:

- **A new DPUSM hardware subtype:** rejected because acceleration is an
  implementation choice, not a distinct on-disk algorithm. A new subtype
  would duplicate algorithm selection and complicate software fallback.
- **A separate ZFS-facing DPUSM service:** rejected because it would bypass
  Zalgo's provider selection, holds, capability metadata, and software
  fallback model, while requiring a second dispatch/lifetime interface in
  ZFS.
- **A single global DPUSM provider binding:** rejected because provider
  selection and gates are runtime pool policy; a global binding would make
  concurrent pools interfere with one another.

This is a synchronous integration decision, not a final production
architecture. Compression, RAIDZ, asynchronous requests, detailed error
propagation, and device-removal behavior require their own operation
contracts before they can be included in the decision.

## ZFS boundary versus implementation detail

The ZFS-facing changes required by this experiment are intentionally small:

- carry `spa_t` into the Fletcher4 checksum dispatch so operation-time policy
  can be pool-local;
- store runtime-only provider and operation-gate state in the pool context;
- validate creation-time settings and release the context during export,
  import reset, and destruction; and
- preserve the existing checksum subtype, on-disk format, and normal ZFS
  fallback entry points.

The following remain behind Zalgo/DPUSM and are not ZFS algorithm changes:

- provider registration, accelerated/software classification, selection,
  holds, and fallback-node management;
- DPUSM capability checks, provider handles, temporary buffers, ABD
  linearization, synchronous result mapping, failure quarantine, and context
  draining; and
- optional DPUSM configuration, Kbuild linkage, and software-provider test
  module handling.

This boundary means a future compression, RAIDZ, or asynchronous integration
must first define its caller-visible result and lifetime contract, then add
only the minimum ZFS plumbing needed to carry pool policy and operation
context. It should not copy DPUSM provider logic into common ZFS code.

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

## Compression result contract

The current ZFS compression ABI returns only `size_t`. It uses a returned
length greater than the destination capacity to mean that the input was not
compressible, and `zio_compress_data()` then reports the original input length.
That representation cannot safely carry a DPUSM provider failure: a hardware
failure must not be mistaken for valid incompressible data.

Any future Zalgo compression operation must therefore return an explicit
result alongside the output length:

- `OK`: compressed output is valid and `dst_len` is its length;
- `INCOMPRESSIBLE`: the operation completed successfully but compression did
  not improve the input, so the caller may use the original input length;
- `PROVIDER_FAILURE`: the accelerator failed or became unavailable, so the
  caller must retry the same algorithm through software without interpreting
  `dst_len`;
- `INVALID`: arguments or algorithm selection are invalid and should be
  reported rather than treated as incompressible input.

The DPUSM `compress()` API already returns a status and output length, but the
ZFS/Zalgo adapter must preserve that status until the fallback decision is
made. Compression dispatch is intentionally not wired into the existing
size_t-only `zio_compress_data()` path until this result contract has a
caller-visible compatibility boundary.

The userspace OpenSSL shim is independently configured from the base
libcrypto dependency. Its probe checks the provider MAC API, fetched AES
ciphers, cipher-context copying, AEAD setup, encryption, and tag retrieval.
If that complete API is unavailable, the shim library and its registration are
omitted while the core Zalgo, software providers, and libzpool continue to
build. DPUSM follows the same optional-provider rule through its existing
configure and Kbuild conditionals.

The shim classifies synchronous DPUSM checksum failures internally as
unavailable, unsupported, invalidated, or generic provider failures. A pool
context counts consecutive operation failures; after three failures it
quarantines DPUSM checksum use for that pool by disabling its runtime checksum
gate. The next explicit provider selection or checksum-gate update can restore
use after the caller has decided the provider is healthy. Checksum mismatches
remain ZFS data-integrity results and are not classified as provider failures.

For the currently implemented Fletcher4 operation, this is the concrete
fallback sequence: the pool-aware DPUSM callback returns an acceleration
failure without consuming the source ABD, the Zalgo helper retries the
software provider for the same subtype, and the ZFS caller retains its normal
Fletcher4 implementation as the final fallback. The policy is not yet a
generic contract for compression, RAIDZ, or asynchronous operations.

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
