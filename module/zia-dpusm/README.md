# Using Z.I.A. with the DPUSM

ZFS builds the ZFS Interface for Accelerators (Z.I.A.) without the
[Data Processing Unit Services Module (DPUSM)](https://github.com/hpc/dpusm).
The DPUSM is attached at runtime by the `zia-dpusm` bridge module in this
directory. Any DPUSM provider can then be used by a pool.

```
cd zfs
make zia-modules DPUSM=/path/to/dpusm   # builds zia-dpusm.ko and zia-software-provider.ko
modprobe zfs                            # zfs and dpusm can be loaded in either order
sudo insmod /path/to/dpusm/dpusm.ko
sudo insmod <provider>.ko               # any DPUSM provider
sudo insmod module/zia-dpusm.ko         # attaches Z.I.A. to the DPUSM
zpool set zia_provider=<provider module name> <pool>
```

`zpool get zia_available <pool>` shows `yes` once `zia-dpusm` is loaded.
Providers are never loaded on demand, and `zia_provider` is not stored in the
pool, so set it again after importing the pool.

To unload, run `zpool set zia_provider=off` on every pool using a provider,
then unload `zia-dpusm`, the providers, and the DPUSM.
