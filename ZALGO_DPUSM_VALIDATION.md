# Zalgo DPUSM validation plan

This plan is for the disposable runtime environment. It intentionally does
not authorize or execute module insertion, pool creation/destruction, device
writes, or provider unload operations.

## Build and configuration matrix

- Build without `--with-dpusm` and verify ZFS/Zalgo software operation still
  builds and initializes.
- Build with `--with-dpusm=<DPUSM_ROOT>` and verify `zfs.ko`, DPUSM, and
  `zia_software_provider.ko` load in the documented dependency order.
- Confirm the configured DPUSM and Zalgo revisions match
  `ZALGO_DPUSM_DESIGN.md`.

## Pool isolation and policy

Create two disposable pools with separate runtime policies:

```text
pool_a: dpusm_provider=zia_software_provider, dpusm_checksum=on
pool_b: dpusm_provider=off,                  dpusm_checksum=on
```

Verify that Fletcher4 writes, reads, and checksum verification on `pool_a`
can use DPUSM while `pool_b` continues through software, and that changing or
destroying one pool does not change the other pool's provider state.

Also verify:

- `dpusm_checksum=off` disables acceleration but not checksumming;
- setting provider and gate properties in either nvlist order produces the
  same final state;
- `zpool get` reports the active provider only for the selected pool;
- a failed provider selection leaves the previous valid provider intact.

## Replacement and quarantine

- Replace a selected provider on one active pool and verify operations drain
  before the old provider handle reaches zero users.
- Inject or otherwise induce three synchronous DPUSM operation failures.
- Confirm the affected pool falls back to software and its DPUSM checksum gate
  becomes disabled without changing another pool.
- Re-enable `dpusm_checksum` or explicitly select the provider again and verify
  the failure counter is reset and the provider can be retried.
- Confirm checksum mismatches remain normal ZFS data errors, not provider
  quarantine events.

## Export/import and teardown

- Select a provider, export the pool, and verify the provider is absent after
  import until explicitly selected again.
- Destroy one selected pool and confirm its final DPUSM provider `put` occurs
  after vdev/DSL teardown.
- With another pool still selected, confirm destroying the first pool does not
  unregister the shared Zalgo operation tables or release the second pool's
  provider.
- After all pools are exported/destroyed or deselected, verify provider users
  reach zero and the provider module can be unloaded in the documented order.

## Correctness coverage

Exercise linear and ABD-backed requests, small and large blocks, alignment
edges, native and byteswapped Fletcher4, concurrent I/O, partial/failing
operations, checksum verification, repair, scrub, encryption, and ordinary
ZFS redundancy behavior. Compare checksums and pool status against a software
baseline and record any provider or data-integrity errors separately.

## Completed hpe2 validation

Using only `/dev/nvme0n1` through `/dev/nvme4n1`, the following disposable
validation completed on 2026-11-18 with the DPUSM-enabled `zalgo-zia` build:

- Created a striped pool and a `raidz2` pool sequentially, each with
  `dpusm_provider=zia_software_provider`.
- Performed 1 GiB fio writes and reads on both layouts with Fletcher4 and LZ4
  datasets.
- Disabled and re-enabled `dpusm_checksum`; normal writes continued to work
  with the acceleration gate disabled.
- Disabled and re-selected the DPUSM provider; software-path I/O succeeded
  while disabled and DPUSM-backed I/O resumed after re-selection.
- Scrubbed both layouts; both completed with zero repaired bytes, zero errors,
  and `errors: No known data errors`.
- Exported/imported the striped pool; the runtime-only provider reset to `-`,
  then worked again after explicit re-selection.
- Offlined one RAIDZ2 device, successfully read 1 GiB while degraded, onlined
  the device, and observed completion of a zero-error resilver.
- Destroyed both temporary pools and verified no pools or ZFS mounts remained.

This validates normal provider selection, pool-policy changes, runtime reset,
striped and RAIDZ2 I/O, scrub, degraded read, and resilver behavior. It does
not yet validate induced DPUSM operation failures/quarantine, corruption
injection, encryption, concurrent multi-pool isolation, device removal, or
module unload ordering.

## Additional hpe2 failure and isolation validation

An authorized validation run also completed with the software provider's
`checksum_down_max=-1` failure-injection parameter, which makes the next
provider checksum operation return an invalidated-provider result. The run
verified:

- a forced DPUSM checksum failure did not interrupt a 256 MiB fio write;
- the affected pool remained `ONLINE` with no known data errors, showing the
  software/ZFS fallback path remained usable;
- resetting the parameter to `0`, explicitly re-enabling `dpusm_checksum`,
  and writing another 256 MiB file succeeded;
- two simultaneous pools retained independent policies, with one using
  `zia_software_provider` and the other using provider `-`;
- both isolated pools accepted writes and scrubbed with zero errors; and
- all temporary pools were destroyed and the failure-injection parameter was
  restored to `0`.

The internal quarantine state is not exposed through `zpool get`, and the
current shim emits no dedicated quarantine log, so this validates fallback and
recovery behavior but not direct observability of the three-failure threshold.

The existing userspace `make unit` suite also completed successfully,
including the Zalgo registration/hold/selection tests and Fletcher checksum
tests. It does not yet contain DPUSM-specific context, quarantine, or teardown
unit coverage.

## Module teardown and reload validation

After all disposable pools and mounts were removed, the authorized modules
were unloaded in dependency-safe order:

```text
zia_software_provider -> zfs -> spl -> dpusm
```

The provider unregister completed with zero users, ZFS reported a clean module
unload, and DPUSM exited cleanly. The same four modules were then inserted
again, the no-pool ZFS state was checked, and they were unloaded a second time
in reverse dependency order. Both cycles completed without kernel crash
signatures. The external `my_fletcher_provider.ko` was not loaded.

## Compression integration validation

The compression change was built with DPUSM enabled and completed:

```text
MODPOST 3 modules
exit=0
```

The existing userspace `make unit` suite also completed successfully with
`exit=0`, including the existing Zalgo registration tests.

On a disposable striped pool using only `/dev/nvme0n1` through
`/dev/nvme4n1`, the rebuilt modules and software provider completed:

- LZ4 dataset write/read through the DPUSM compression and decompression
  path;
- GZIP-6 dataset write/read through the DPUSM path;
- ZSTD dataset write/read through the CPU fallback because the current
  provider does not advertise ZSTD;
- compression gate-off write through the CPU path;
- decompression gate-off read through the CPU path;
- provider-off read through normal software fallback;
- provider re-enable, scrub, and status verification.

The final pool status was `ONLINE` with zero read, write, or checksum errors,
`scrub repaired 0B`, and `errors: No known data errors`. The pool was
destroyed and `zia_software_provider`, `zfs`, `spl`, and `dpusm` were unloaded
cleanly. No kernel crash signatures were observed and all authorized devices
remained available.

The validation deliberately does not claim DPUSM ZSTD acceleration: the
current software provider's capability mask contains GZIP and LZ4 only.

## No-DPUSM CPU baseline

The Zalgo tree was configured without `--with-dpusm` and built successfully as
two modules (`MODPOST 2 modules`): `spl.ko` and `zfs.ko`. The resulting
`zfs.ko` depended only on `spl.ko`, confirming that the DPUSM shim and
software-provider module were omitted.

On that CPU-only build, a disposable striped pool using all five authorized
NVMe devices completed 256 MiB fio write/read operations and a scrub. The
pool remained `ONLINE` with zero read, write, and checksum errors and
`errors: No known data errors`. The pool was destroyed and the CPU-only
modules were unloaded cleanly.

Afterward, the tree was restored to the DPUSM-enabled configuration and a
fresh DPUSM-enabled kernel build was started; its final result is recorded
separately after completion.
