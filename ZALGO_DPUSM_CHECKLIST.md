# Zalgo-DPUSM Integration Checklist

This is an initial, non-comprehensive checklist for integrating hardware-
accelerated DPUSM functionality as Zalgo providers and hardware-specific
subtypes. The working assumption is that Zalgo can become the bridge between
ZFS and DPUSM, but the design is not settled.

## Repository and baseline

- [ ] Record the exact Zalgo and DPUSM commits used for each experiment.
- [ ] Build and run the existing Zalgo path without DPUSM as a baseline.
- [ ] Build and run the existing DPUSM path independently for a
      behavior and performance comparison.
- [ ] Identify which existing accelerator-integration changes are required by
      ZFS, and which are implementation details that can move behind Zalgo.
- [ ] Keep the first integration experiments behind build/configuration
      checks so ZFS still builds when DPUSM is unavailable.

## Zalgo model and subtype design

- [ ] Decide whether DPUSM should be a provider of an existing subtype or
      whether it requires new hardware-acceleration subtypes.
- [ ] Define the operation and context contracts for each proposed subtype.
- [ ] Specify which operations are synchronous, asynchronous, or capable of
      both modes.
- [ ] Define how ABDs, linear buffers, scatter/gather data, alignment, and
      size restrictions are represented.
- [ ] Define capability reporting for hardware availability, supported
      algorithms, maximum request sizes, and concurrency limits.
- [ ] Decide whether provider selection is global, per operation, per pool,
      per dataset, or per request.
- [ ] Document how subtype selection and provider benchmarking interact with
      DPUSM setup and device state.

### Algorithm implementation and test-provider scope

- [x] Do not require a new, dedicated DPUSM provider implementation for each
      Zalgo/Z.I.A. algorithm.  For initial validation, use the
      `zia_software_provider` implementation, or adapt an existing hardware
      accelerator provider that already works with the original Z.I.A.
      implementation.
- [x] Keep any provider adapter, capability mapping, and lifetime handling
      needed to expose those providers through Zalgo separate from writing a
      new DPUSM algorithm implementation.
- [x] Treat asynchronous Z.I.A. as out of scope for this integration.  It is
      developed in a separate worktree and should not be folded into the
      synchronous Zalgo/DPUSM checklist or implementation plan.
- [x] Do not add benchmarking infrastructure for Zalgo operation types that
      do not already have a meaningful benchmark.  Where a kstat score is
      required for registry or reporting consistency, use a score of zero
      rather than inventing a benchmark for the operation.

### Agreed dispatch and fallback model

- [x] Keep algorithm selection and implementation selection separate.  The
      dataset property or existing block metadata selects the algorithm
      subtype (for example, `checksum=sha256` selects checksum/SHA-256 and
      `compression=lz4` selects compression/LZ4); pool context only selects
      which eligible provider implements that subtype.
- [x] Give each algorithm-specific registry entry an independently selected
      `zr_current` provider and `zr_fallback` software provider.  A DPUSM
      failure must
      not overwrite or replace the current provider merely to recover from
      that operation.
- [x] Use the fallback hierarchy:
      `DPUSM Zalgo provider -> software Zalgo provider -> normal ZFS
      implementation`.  If no software Zalgo provider exists, the ZFS
      integration must invoke the existing implementation for that algorithm.
- [x] Treat all non-success DPUSM operation results as ordinary acceleration
      failures for the initial implementation.  Clean up the DPUSM operation
      and retry the same algorithm through the software Zalgo provider where
      possible; more specific error classification can be added later.
- [x] For the initial DPUSM path, copy data to provider memory before the
      operation and copy results back afterward on every call.  Do not require
      persistent offloaded ABD state or lazy onload behavior yet.
- [x] The DPUSM operation wrapper owns the temporary provider buffers used for
      copying and may free them after a failed hardware operation.  The Zalgo
      fallback path must receive a valid source and clean destination state.
- [x] Treat provider invalidation as an operation failure that returns to the
      same fallback path.  DPUSM's provider validity checks are expected to
      report the invalidation through the operation result.
- [x] Add provider classification as registration metadata rather than
      inferring it from provider names or requiring new algorithm subtypes.
      Prefer an explicit accelerated/software provider flag, with the
      existing simple registration API retaining a low-friction default for
      providers that do not need special classification.  Use that metadata
      to identify the best software fallback independently of the overall
      best/current provider.
- [x] Do not treat the absence of a software Zalgo provider as an integration
      failure.  It is a required final fallback to the established ZFS
      implementation, not a reason to change the selected algorithm.
- [ ] Implement pool-aware operation-time dispatch so the pool's DPUSM gate
      and configured provider are consulted without changing the
      dataset/block-selected subtype.  This is the implementation issue
      behind the pool-context dispatch design, not a change to the algorithm
      selection model.
- [ ] Hold the selected Zalgo node and DPUSM provider handle through the
      complete operation and any software retry, including invalidation and
      provider-replacement races.
- [ ] Define a compression operation result that distinguishes provider
      failure from successful incompressible input.  A compressed-size return
      value alone must not make a hardware failure look like a valid
      uncompressed result.

### Configuration scope and policy

- [x] Keep ZFS algorithm selection at the dataset level: dataset properties
      such as `checksum` and `compression` select the algorithm and on-disk
      format required by the operation.
- [x] Keep DPUSM provider selection and operation gates at the pool level,
      following the existing Z.I.A. model.  The initial properties are
      `dpusm_provider`, `dpusm_compress`, `dpusm_decompress`,
      `dpusm_checksum`, and per-width RAIDZ generation/reconstruction gates.
- [x] Define `dpusm_<operation>=off` as disabling DPUSM acceleration only;
      it must not disable the corresponding ZFS algorithm or integrity
      operation.
- [x] Define the fallback hierarchy as: dataset/block algorithm selection,
      pool DPUSM gate and provider capability, then the normal Zalgo/software
      provider.  A missing or invalid DPUSM provider must not prevent normal
      ZFS operation.
- [ ] Connect pool policy to operation-time Zalgo holds so separate pools can
      use different DPUSM policies without changing Zalgo's global default.
- [ ] Add runtime provider binding, capability checks, and safe replacement
      when `dpusm_provider` changes.
- [ ] Add user documentation and tests for dataset algorithm settings
      interacting with pool-level DPUSM settings.

## DPUSM shim investigation

- [x] Prototype `module/zalgo/zalgo_shim_dpusm.c` as an optional in-tree
      shim, with no DPUSM dependency when DPUSM is not configured.
- [x] Identify the smallest DPUSM API surface the shim needs.
- [ ] Map Zalgo contexts and operation lifetimes to DPUSM sessions, queues,
      requests, completions, and buffers.
- [x] Determine whether DPUSM can consume ABD-backed data directly or needs
      a linearization/copy path.
- [x] Define initialization and teardown ordering for ZFS, Zalgo, DPUSM, and
      hardware resources.
- [ ] Decide how the shim reports unsupported requests, queue exhaustion,
      device loss, timeouts, and hardware errors.
- [x] Ensure the shim can be compiled, linked, and loaded on systems without
      DPUSM.

## Error handling and fallback

- [ ] Make error propagation end-to-end for operations whose existing ZFS
      interfaces currently return `void`, especially checksum operations.
- [x] Decide whether a failed hardware provider is retried by another Zalgo
      provider, and define which failures are retryable.
- [x] Restart retries from the original input with a fresh context rather
      than continuing a partially completed operation.
- [ ] Add an explicit DPUSM-to-software fallback path for every accelerated
      operation.  The synchronous Fletcher4 first pass now retries the best
      available software Zalgo provider, but each future accelerated operation
      must receive the same treatment.
      A DPUSM provider failure must retry the best available
      software Zalgo provider for the same algorithm before returning an
      error, where the surrounding ZFS operation permits retry.
- [ ] Model the Z.I.A. `ZIA_FALLBACK` behavior for DPUSM failures without
      conflating it with an algorithm failure: unsupported, unavailable,
      invalidated, timed-out, or hardware-error results should select the
      software implementation, while malformed compressed data,
      checksum mismatches, and authentication failures must remain data
      errors.
- [x] Ensure a DPUSM failure leaves a valid retry input and a clean output
      destination.  Hardware may partially consume or modify buffers, so a
      retry must restore or preserve the original data and reset output
      ownership/length state.
- [x] Retain or identify the best software provider for each algorithm
      independently of the currently selected DPUSM provider.  DPUSM
      selection must not replace the software fallback needed for recovery.
- [ ] Add provider health, quarantine, or re-selection behavior if needed.
- [ ] Distinguish provider failure from ordinary checksum mismatch,
      authentication failure, unsupported request, and device unavailability.
- [ ] Preserve normal ZFS redundancy, repair, and error-reporting behavior.

## Asynchronous execution

This section is intentionally out of scope for the current integration.  The
asynchronous Z.I.A. work is maintained in a separate worktree; revisit these
items only when that work is explicitly brought into this branch.

- [ ] Determine whether asynchronous DPUSM work fits the current Zalgo
      operation tables or requires new async operation interfaces.
- [ ] Define submission, completion, cancellation, timeout, and retry rules.
- [ ] Define ownership and lifetime of ABDs, contexts, requests, and provider
      holds across asynchronous completion.
- [ ] Prevent provider unload or teardown while callbacks or requests remain
      active.
- [ ] Decide how ZFS task queues and ZIO pipeline stages interact with DPUSM
      completion events.

## Build, configuration, and portability

- [ ] Add a configuration mechanism for detecting and selecting DPUSM.
- [ ] Keep common Zalgo code portable across Linux, FreeBSD, and userspace
      builds where applicable.
- [ ] Keep DPUSM-specific headers and kernel interfaces behind the appropriate
      platform/configuration boundaries.
- [ ] Update Kbuild, configure checks, generated build inputs, and packaging
      only after the integration shape is understood.
- [ ] Document required DPUSM versions, symbols, and ABI compatibility.

### Optional OpenSSL provider dependencies

- [ ] Resolve the current CI linker failure in
      `module/zalgo/zalgo_shim_openssl.c`: the shim calls
      `EVP_CIPHER_CTX_dup()`, but some CI environments expose the declaration
      through their headers while linking an older `libcrypto` that does not
      provide the symbol.
- [ ] Prefer replacing each `EVP_CIPHER_CTX_dup()` call with a compatibility
      helper that allocates a destination using `EVP_CIPHER_CTX_new()`, copies
      the source using `EVP_CIPHER_CTX_copy()`, and frees the destination if
      the copy fails. This avoids requiring the newer `EVP_CIPHER_CTX_dup()`
      API while preserving the prepared-context-per-operation design.
- [ ] Verify the `EVP_CIPHER_CTX_copy()` approach with the fetched AES-GCM and
      AES-CCM cipher contexts used by the shim.
- [ ] Add configure-time compile/link capability checks for the complete
      OpenSSL API used by the Zalgo shim rather than checking only the
      existing basic `libcrypto` functionality.
- [ ] Make the OpenSSL Zalgo shim independently optional: if its headers,
      symbols, or library are unavailable, omit only
      `libzalgo_shim_openssl.la` and continue building Zalgo, `libzpool`, and
      the normal software providers.
- [ ] Keep optional-provider registration conditional on whether the provider
      was compiled, so an unavailable OpenSSL dependency does not become a
      whole-ZFS build or initialization failure.
- [ ] Apply the same model to the future DPUSM shim: unavailable DPUSM should
      omit that provider while leaving Zalgo core, CPU providers, and CI
      builds functional.

## Lifetime, registration, and unload safety

- [ ] Add safe provider unregister support before treating an external or
      optional DPUSM provider as unloadable.
- [ ] Protect `zr_current`, `zr_best`, provider nodes, and active holds while
      changing provider availability.
- [ ] Integrate provider/module references with DPUSM device and queue
      lifetimes.
- [ ] Verify teardown after pool destruction, export/import, module reload,
      device removal, and DPUSM shutdown.

## Validation and measurement

- [ ] Add unit coverage for provider registration, selection, capabilities,
      errors, fallback, and teardown.
- [ ] Test with DPUSM absent, present but idle, busy, unavailable, and failing.
- [ ] Test linear and ABD inputs, small and large requests, alignment edges,
      concurrent requests, and partial failures.
- [ ] Compare correctness against the existing CPU and DPUSM implementations.
- [ ] Compare CPU and Zalgo-DPUSM correctness and behavior under matching
      workloads.  Add performance measurements only for operation types that
      already have Zalgo benchmarks; report a zero kstat score for operation
      types without a benchmark when a score is required.
- [ ] Validate normal ZFS pool I/O, checksum verification, repair, encryption,
      and recovery behavior on disposable test storage.

## Design decision

- [ ] Decide whether DPUSM is best implemented as a Zalgo provider, a new
      Zalgo hardware subtype, a separate service behind a Zalgo provider, or
      a combination of these approaches.
- [ ] Record the rejected alternatives and the reasons for choosing the
      eventual integration model before preparing a production pull request.
