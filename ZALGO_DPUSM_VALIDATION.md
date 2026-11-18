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
