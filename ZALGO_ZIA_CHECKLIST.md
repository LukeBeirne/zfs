# Z.I.A. Functionality in Zalgo Checklist

This is an initial, non-comprehensive checklist for investigating whether
Z.I.A. functionality should be represented as Zalgo providers and hardware-
specific subtypes. The working assumption is that Zalgo could become the
bridge between ZFS and DPUSM, but the design is not settled.

## Repository and baseline

- [ ] Record the exact Zalgo and DPUSM commits used for each experiment.
- [ ] Build and run the existing Zalgo path without DPUSM as a baseline.
- [ ] Build and run the existing Z.I.A./DPUSM path independently for a
      behavior and performance comparison.
- [ ] Identify which current Z.I.A. changes are required by ZFS, and which
      are implementation details that could move behind Zalgo.
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

## DPUSM shim investigation

- [ ] Prototype `module/zalgo/zalgo_shim_dpusm.c` as an optional in-tree
      shim, with no DPUSM dependency when DPUSM is not configured.
- [ ] Identify the smallest DPUSM API surface the shim needs.
- [ ] Map Zalgo contexts and operation lifetimes to DPUSM sessions, queues,
      requests, completions, and buffers.
- [ ] Determine whether DPUSM can consume ABD-backed data directly or needs
      a linearization/copy path.
- [ ] Define initialization and teardown ordering for ZFS, Zalgo, DPUSM, and
      hardware resources.
- [ ] Decide how the shim reports unsupported requests, queue exhaustion,
      device loss, timeouts, and hardware errors.
- [ ] Ensure the shim can be compiled, linked, and loaded on systems without
      DPUSM.

## Error handling and fallback

- [ ] Make error propagation end-to-end for operations whose existing ZFS
      interfaces currently return `void`, especially checksum operations.
- [ ] Decide whether a failed hardware provider is retried by another Zalgo
      provider, and define which failures are retryable.
- [ ] Restart retries from the original input with a fresh context rather
      than continuing a partially completed operation.
- [ ] Add provider health, quarantine, or re-selection behavior if needed.
- [ ] Distinguish provider failure from ordinary checksum mismatch,
      authentication failure, unsupported request, and device unavailability.
- [ ] Preserve normal ZFS redundancy, repair, and error-reporting behavior.

## Asynchronous execution

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
- [ ] Compare correctness against the existing CPU/Z.I.A. implementations.
- [ ] Benchmark CPU, existing Z.I.A., and Zalgo-DPUSM paths under matching
      workloads and report setup overhead and variability.
- [ ] Validate normal ZFS pool I/O, checksum verification, repair, encryption,
      and recovery behavior on disposable test storage.

## Design decision

- [ ] Decide whether DPUSM is best implemented as a Zalgo provider, a new
      Zalgo hardware subtype, a separate service behind a Zalgo provider, or
      a combination of these approaches.
- [ ] Record the rejected alternatives and the reasons for choosing the
      eventual integration model before preparing a production pull request.
