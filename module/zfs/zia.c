// SPDX-License-Identifier: BSD-3-Clause
/*
 * © 2021. Triad National Security, LLC. All rights reserved.
 *
 * This program was produced under U.S. Government contract
 * 89233218CNA000001 for Los Alamos National Laboratory (LANL), which
 * is operated by Triad National Security, LLC for the U.S.
 * Department of Energy/National Nuclear Security Administration. All
 * rights in the program are reserved by Triad National Security, LLC,
 * and the U.S. Department of Energy/National Nuclear Security
 * Administration. The Government is granted for itself and others
 * acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
 * license in this material to reproduce, prepare derivative works,
 * distribute copies to the public, perform publicly and display
 * publicly, and to permit others to do so.
 *
 * ----
 *
 * This program is open source under the BSD-3 License.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/abd.h>
#include <sys/abd_impl.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_disk.h>
#include <sys/vdev_file.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/zia.h>
#include <sys/zia_cddl.h>
#include <sys/zia_private.h>

#ifdef ZIA
#if defined(_KERNEL) && defined(__linux__)
#include <linux/module.h>
#endif
#endif

/* ************************************************************* */
/*
 * Accelerator operations registered at runtime by a bridge module
 * (see sys/zia_ops.h).
 *
 * zia_ops, zia_ops_owner and zia_ops_holds are only changed with
 * zia_ops_lock held. zia_ops can only be cleared while
 * zia_ops_holds is 0, i.e. while no pool holds a provider, and every
 * offloader handle belongs to a held provider. So code that has a
 * provider or a handle in hand may use zia_ops without the lock.
 */
static const zia_ops_t *zia_ops = NULL;
#ifdef ZIA
static void *zia_ops_owner = NULL;
static uint64_t zia_ops_holds = 0;
static kmutex_t zia_ops_lock;
#endif
/* ************************************************************* */

#ifdef ZIA
/* every function Z.I.A. calls without checking for NULL first */
static boolean_t
zia_ops_complete(const zia_ops_t *ops)
{
	return (ops->get && ops->get_name && ops->put && ops->extract &&
	    ops->capabilities && ops->alloc && ops->alloc_ref &&
	    ops->free && ops->associate_handle &&
	    ops->copy.from.generic && ops->copy.to.generic &&
	    ops->zero_fill && ops->all_zeros &&
	    ops->compress && ops->decompress && ops->checksum &&
	    ops->raid.can_compute && ops->raid.alloc &&
	    ops->raid.set_column && ops->raid.free && ops->raid.gen &&
	    ops->raid.cmp && ops->raid.rec &&
	    ops->file.open && ops->file.write && ops->file.close &&
	    ops->disk.open && ops->disk.invalidate && ops->disk.write &&
	    ops->disk.flush && ops->disk.close);
}

/* take/release a reference on the registered owner (zia_ops_lock held) */
static boolean_t
zia_ops_owner_hold(void)
{
	ASSERT(MUTEX_HELD(&zia_ops_lock));
#if defined(_KERNEL) && defined(__linux__)
	if (zia_ops_owner != NULL && !try_module_get(zia_ops_owner))
		return (B_FALSE);
#endif
	zia_ops_holds++;
	return (B_TRUE);
}

static void
zia_ops_owner_rele(void)
{
	mutex_enter(&zia_ops_lock);
	ASSERT3U(zia_ops_holds, >, 0);
	zia_ops_holds--;
	void *owner = zia_ops_owner;
	mutex_exit(&zia_ops_lock);

#if defined(_KERNEL) && defined(__linux__)
	if (owner != NULL)
		module_put(owner);
#else
	(void) owner;
#endif
}
#endif

int
zia_ops_register(const zia_ops_t *ops, void *owner)
{
#ifdef ZIA
	if (ops == NULL || ops->zo_version != ZIA_OPS_VERSION ||
	    ops->zo_size != sizeof (zia_ops_t) || !zia_ops_complete(ops))
		return (SET_ERROR(EINVAL));

	mutex_enter(&zia_ops_lock);
	if (zia_ops != NULL) {
		mutex_exit(&zia_ops_lock);
		return (SET_ERROR(EBUSY));
	}
	zia_ops_owner = owner;
	zia_ops = ops;
	mutex_exit(&zia_ops_lock);

#if defined(_KERNEL) && defined(__linux__)
	printk(KERN_INFO "Z.I.A. attached to %s\n",
	    ops->zo_name ? ops->zo_name : "(unnamed)");
#endif
	return (0);
#else
	(void) ops; (void) owner;
	return (SET_ERROR(ENOTSUP));
#endif
}

int
zia_ops_unregister(const zia_ops_t *ops)
{
#ifdef ZIA
	mutex_enter(&zia_ops_lock);
	if (ops == NULL || zia_ops != ops) {
		mutex_exit(&zia_ops_lock);
		return (SET_ERROR(EINVAL));
	}
	if (zia_ops_holds != 0) {
		mutex_exit(&zia_ops_lock);
		return (SET_ERROR(EBUSY));
	}
	zia_ops = NULL;
	zia_ops_owner = NULL;
	mutex_exit(&zia_ops_lock);

#if defined(_KERNEL) && defined(__linux__)
	printk(KERN_INFO "Z.I.A. detached from %s\n",
	    ops->zo_name ? ops->zo_name : "(unnamed)");
#endif
	return (0);
#else
	(void) ops;
	return (SET_ERROR(ENOTSUP));
#endif
}

EXPORT_SYMBOL(zia_ops_register);
EXPORT_SYMBOL(zia_ops_unregister);

boolean_t
zia_is_attached(void)
{
	return (zia_ops != NULL ? B_TRUE : B_FALSE);
}

zia_props_t *
zia_get_props(spa_t *spa)
{
	return (spa?&spa->spa_zia_props:NULL);
}

void
zia_prop_warn(boolean_t val, const char *name)
{
#if defined(_KERNEL) && defined(__linux__)
	if (val == B_TRUE) {
		printk("Z.I.A. %s enabled. Encryption and "
		    "Dedup for this spa will be disabled.\n",
		    name);
	}
#else
	(void) val; (void) name;
#endif
}

int
zia_ops_to_ret(const int ops_ret)
{
#ifdef ZIA
	int zia_ret = ZIA_FALLBACK;
	switch (ops_ret) {
		case ZIA_OPS_OK:
			zia_ret = ZIA_OK;
			break;
		case ZIA_OPS_ERROR:
			zia_ret = ZIA_ERROR;
			break;
		case ZIA_OPS_PROVIDER_MISMATCH:
			zia_ret = ZIA_PROVIDER_MISMATCH;
			break;
		case ZIA_OPS_NOT_IMPLEMENTED:
		case ZIA_OPS_NOT_SUPPORTED:
			zia_ret = ZIA_FALLBACK;
			break;
		case ZIA_OPS_BAD_RESULT:
			zia_ret = ZIA_BAD_RESULT;
			break;
		case ZIA_OPS_PROVIDER_NOT_EXISTS:
		case ZIA_OPS_PROVIDER_INVALIDATED:
		case ZIA_OPS_PROVIDER_UNREGISTERED:
		default:
			zia_ret = ZIA_ACCELERATOR_DOWN;
			break;
	}
	return (zia_ret);
#else
	(void) ops_ret;
	return (ZIA_DISABLED);
#endif
}

#ifdef ZIA
zia_compress_t
compress_to_zia(enum zio_compress c)
{
	zia_compress_t zia_c = 0;

	switch (c) {
		case ZIO_COMPRESS_GZIP_1:
			zia_c = ZIA_COMPRESS_GZIP_1;
			break;
		case ZIO_COMPRESS_GZIP_2:
			zia_c = ZIA_COMPRESS_GZIP_2;
			break;
		case ZIO_COMPRESS_GZIP_3:
			zia_c = ZIA_COMPRESS_GZIP_3;
			break;
		case ZIO_COMPRESS_GZIP_4:
			zia_c = ZIA_COMPRESS_GZIP_4;
			break;
		case ZIO_COMPRESS_GZIP_5:
			zia_c = ZIA_COMPRESS_GZIP_5;
			break;
		case ZIO_COMPRESS_GZIP_6:
			zia_c = ZIA_COMPRESS_GZIP_6;
			break;
		case ZIO_COMPRESS_GZIP_7:
			zia_c = ZIA_COMPRESS_GZIP_7;
			break;
		case ZIO_COMPRESS_GZIP_8:
			zia_c = ZIA_COMPRESS_GZIP_8;
			break;
		case ZIO_COMPRESS_GZIP_9:
			zia_c = ZIA_COMPRESS_GZIP_9;
			break;
		case ZIO_COMPRESS_LZ4:
			zia_c = ZIA_COMPRESS_LZ4;
			break;
		case ZIO_COMPRESS_INHERIT:
		case ZIO_COMPRESS_ON:
		case ZIO_COMPRESS_OFF:
		case ZIO_COMPRESS_LZJB:
		case ZIO_COMPRESS_EMPTY:
		case ZIO_COMPRESS_ZLE:
		case ZIO_COMPRESS_ZSTD:
		case ZIO_COMPRESS_FUNCTIONS:
		default:
			break;
	}

	return (zia_c);
}

static zia_checksum_t
checksum_to_zia(enum zio_checksum c)
{
	zia_checksum_t zia_c = 0;
	switch (c) {
		case ZIO_CHECKSUM_FLETCHER_2:
			zia_c = ZIA_CHECKSUM_FLETCHER_2;
			break;
		case ZIO_CHECKSUM_FLETCHER_4:
			zia_c = ZIA_CHECKSUM_FLETCHER_4;
			break;
		case ZIO_CHECKSUM_SHA256:
			zia_c = ZIA_CHECKSUM_SHA256;
			break;
		case ZIO_CHECKSUM_INHERIT:
		case ZIO_CHECKSUM_ON:
		case ZIO_CHECKSUM_OFF:
		case ZIO_CHECKSUM_LABEL:
		case ZIO_CHECKSUM_GANG_HEADER:
		case ZIO_CHECKSUM_ZILOG:
		case ZIO_CHECKSUM_ZILOG2:
		case ZIO_CHECKSUM_NOPARITY:
		case ZIO_CHECKSUM_SHA512:
		case ZIO_CHECKSUM_SKEIN:
		default:
			break;
	}

	return (zia_c);
}

static zia_byteorder_t
byteorder_to_zia(zio_byteorder_t bo)
{
	zia_byteorder_t zia_bo = ZIA_BYTEORDER_INVALID;
	switch (bo) {
		case ZIO_CHECKSUM_NATIVE:
			zia_bo = ZIA_BYTEORDER_NATIVE;
			break;
		case ZIO_CHECKSUM_BYTESWAP:
			zia_bo = ZIA_BYTEORDER_BYTESWAP;
			break;
		default:
			break;
	}

	return (zia_bo);
}
#endif

#ifdef ZIA
int
zia_get_capabilities(void *provider, zia_caps_t *caps)
{
	/* zia_ops is checked by the caller */
	/* provider and caps are checked by the zia_ops */
	return (zia_ops_to_ret(zia_ops->capabilities(provider, caps)));
}
#endif

/*
 * Called when ZFS is loaded. No accelerator is attached yet; a bridge
 * module can only call zia_ops_register() after ZFS has loaded.
 */
int
zia_init(void)
{
#ifdef ZIA
	mutex_init(&zia_ops_lock, NULL, MUTEX_DEFAULT, NULL);
	return (ZIA_OK);
#else
	return (ZIA_DISABLED);
#endif
}

/*
 * Called when ZFS is unloaded. Bridge modules depend on ZFS symbols,
 * so they (and therefore their registrations) are already gone.
 */
int
zia_fini(void)
{
#ifdef ZIA
	ASSERT3P(zia_ops, ==, NULL);
	ASSERT0(zia_ops_holds);
	mutex_destroy(&zia_ops_lock);
	return (ZIA_OK);
#else
	return (ZIA_DISABLED);
#endif
}

#ifdef ZIA
/* recursively find all leaf vdevs and open them */
static void zia_open_vdevs_impl(vdev_t *vd) {
	vdev_ops_t *ops = vd->vdev_ops;
	if (ops->vdev_op_leaf) {
		const size_t len = strlen(ops->vdev_op_type);
		if (len == 4) {
			if (memcmp(ops->vdev_op_type, "file", 4) == 0) {
				zia_file_open(vd, vd->vdev_path,
				    vdev_file_open_mode(spa_mode(vd->vdev_spa)),
				    0);
			}
#ifdef _KERNEL
			else if (memcmp(ops->vdev_op_type, "disk", 4) == 0) {
				zia_disk_open(vd, vd->vdev_path,
				    get_bdev(vd->vdev_tsd));
			}
#endif
		}
	} else {
		for (uint64_t i = 0; i < vd->vdev_children; i++) {
			vdev_t *child = vd->vdev_child[i];
			zia_open_vdevs_impl(child);
		}
	}
}
#endif

void zia_open_vdevs(vdev_t *vd) {
#ifdef ZIA
	if (!vd)
		return;

	spa_vdev_state_enter(vd->vdev_spa, SCL_NONE);
	zia_open_vdevs_impl(vd);
	(void) spa_vdev_state_exit(vd->vdev_spa, NULL, 0);
#endif
	(void) vd;
}

/*
 * Get a reference to a provider. Returns NULL if no accelerator
 * interface is attached or the provider does not exist. Providers are
 * never loaded on demand; load the provider and bridge modules first.
 */
void *
zia_get_provider(const char *name)
{
#ifdef ZIA
	void *provider = NULL;

	if (name == NULL)
		return (NULL);

	mutex_enter(&zia_ops_lock);
	if (zia_ops == NULL) {
		mutex_exit(&zia_ops_lock);
#if defined(_KERNEL) && defined(__linux__)
		printk(KERN_INFO "Z.I.A. cannot get provider \"%s\": "
		    "no accelerator interface attached\n", name);
#endif
		return (NULL);
	}

	if (!zia_ops_owner_hold()) {
		mutex_exit(&zia_ops_lock);
		return (NULL);
	}

	provider = zia_ops->get(name);
	mutex_exit(&zia_ops_lock);

	if (provider == NULL)
		zia_ops_owner_rele();

#if defined(_KERNEL) && defined(__linux__)
	printk(KERN_INFO "Z.I.A. obtained handle to provider \"%s\" (%p)\n",
	    name, provider);
#endif

	return (provider);
#else
	(void) name;

#if defined(_KERNEL) && defined(__linux__)
	printk(KERN_INFO "Z.I.A. not available. "
	    "Cannot obtain handle to providers.\n");
#endif

	return (NULL);
#endif
}

const char *
zia_get_provider_name(void *provider)
{
#ifdef ZIA
	/* a held provider keeps zia_ops registered */
	if (!zia_ops || !provider) {
		return (NULL);
	}

	return (zia_ops->get_name(provider));
#else
	(void) provider;
	return (NULL);
#endif
}

#ifdef ZIA
/* recursively find all leaf vdevs and close them */
static void zia_close_vdevs(vdev_t *vd) {
	vdev_ops_t *ops = vd->vdev_ops;
	if (ops->vdev_op_leaf) {
		const size_t len = strlen(ops->vdev_op_type);
		if (len == 4) {
			if (memcmp(ops->vdev_op_type, "file", 4) == 0) {
				zia_file_close(vd);
			}
#ifdef _KERNEL
			else if (memcmp(ops->vdev_op_type, "disk", 4) == 0) {
				zia_disk_close(vd);
			}
#endif
		}
	} else {
		for (uint64_t i = 0; i < vd->vdev_children; i++) {
			vdev_t *child = vd->vdev_child[i];
			zia_close_vdevs(child);
		}
	}
}
#endif

int
zia_put_provider(void **provider, vdev_t *vdev)
{
#ifdef ZIA
	if (!provider || !*provider) {
		return (ZIA_FALLBACK);
	}

	/* the reference on this provider keeps zia_ops registered */
	ASSERT3P(zia_ops, !=, NULL);

	/*
	 * if the zpool is not going down, but the provider is going away,
	 * make sure the vdevs don't keep pointing to the invalid provider
	 */
	if (vdev) {
		spa_vdev_state_enter(vdev->vdev_spa, SCL_NONE);
		zia_close_vdevs(vdev);
		(void) spa_vdev_state_exit(vdev->vdev_spa, NULL, 0);
	}

#if defined(_KERNEL) && defined(__linux__)
	/* the name may belong to the provider, so print before put */
	printk(KERN_INFO "Z.I.A. returning provider handle \"%s\" (%p)\n",
	    zia_get_provider_name(*provider), *provider);
#endif

	const int ret = zia_ops->put(*provider);
	*provider = NULL;

	/* zia_ops must not be used after this */
	zia_ops_owner_rele();

	return (zia_ops_to_ret(ret));
#else
	(void) provider; (void) vdev;
	return (ZIA_DISABLED);
#endif
}

int
zia_disable_offloading(zio_t *zio, boolean_t reexecute)
{
	if (!zio) {
		return (ZIA_ERROR);
	}

	/* stop all future zios from offloading */
	spa_t *spa = zio->io_spa;
	zia_props_t *zia_props = zia_get_props(spa);
	mutex_enter(&spa->spa_props_lock);
	zia_props->can_offload = B_FALSE;
	mutex_exit(&spa->spa_props_lock);

	/* stop this zio from offloading again */
	zio->io_can_offload = B_FALSE;

	if (reexecute == B_TRUE) {
		zio->io_flags |= ZIO_FLAG_ZIA_REEXECUTE;
	}

	return (ZIA_OK);
}

boolean_t
zia_is_used(zio_t *zio)
{
	if (!zio) {
		return (B_FALSE);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);

	/* provider + at least 1 operation */
	if (zia_ops && props->provider &&
	    (props->compress ||
	    props->decompress ||
	    props->checksum ||
	    props->raidz.gen[1] ||
	    props->raidz.gen[2] ||
	    props->raidz.gen[3] ||
	    props->raidz.rec[1] ||
	    props->raidz.rec[2] ||
	    props->raidz.rec[3] ||
	    props->file_write ||
	    props->disk_write)) {
		return (B_TRUE);
	}

	return (B_FALSE);
}

boolean_t
zia_is_offloaded(abd_t *abd)
{
#ifdef ZIA
	if (!abd) {
		return (B_FALSE);
	}

	return (ABD_HANDLE(abd)?B_TRUE:B_FALSE);
#endif
	(void) abd;
	return (B_FALSE);
}

int
zia_worst_error(const int lhs, const int rhs)
{
	if (lhs == ZIA_ACCELERATOR_DOWN) {
		return (lhs);
	}

	if (rhs == ZIA_ACCELERATOR_DOWN) {
		return (rhs);
	}

	if (lhs == ZIA_OK) {
		return (rhs);
	}

	if (rhs == ZIA_OK) {
		return (lhs);
	}

	return (ZIA_ERROR);
}

/* create a provider handle/offloader buffer without copying data */
void *
zia_alloc(void *provider, size_t size)
{
#ifdef ZIA
	return ((zia_ops && provider)?zia_ops->alloc(provider, size):NULL);
#else
	(void) provider; (void) size;
	return (NULL);
#endif
}

/* free the offloader handle without onloading the data */
int
zia_free(void **handle)
{
	ASSERT(handle);

#ifdef ZIA
	if (*handle == NULL) {
		return (ZIA_OK);
	}

	/*
	 * Handles normally pin zia_ops through their provider. If the
	 * interface went away anyway, the handle cannot be freed, but it
	 * must not be left behind: a stale handle makes the abd look
	 * offloaded forever.
	 */
	if (zia_ops == NULL) {
		*handle = NULL;
		return (ZIA_ACCELERATOR_DOWN);
	}

	const int ret = zia_ops->free(*handle);
	*handle = NULL;
	return (zia_ops_to_ret(ret));
#else
	return (ZIA_DISABLED);
#endif
}

#ifdef ZIA
static int
zia_offload_generic_cb(void *buf, size_t len, void *priv)
{
	zia_mv_t *mv = (zia_mv_t *)priv;

	const int ret = zia_ops->copy.from.generic(mv, buf, len);
	if (zia_ops_to_ret(ret) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	mv->offset += len;
	return (0);
}
#endif

/* offload abd + offset to handle + 0 */
static int
zia_offload_abd_offset(void *provider, abd_t *abd,
    size_t offset, size_t size, boolean_t *local_offload)
{
#ifdef ZIA
	/* already offloaded */
	if (ABD_HANDLE(abd)) {
		if (local_offload) {
			*local_offload = B_FALSE;
		}

		if (!provider) {
			return (ZIA_OK);
		}

		void *abd_provider = zia_ops->extract(ABD_HANDLE(abd));
		return ((provider == abd_provider)?
		    ZIA_OK:ZIA_PROVIDER_MISMATCH);
	}

	zia_caps_t caps;
	if (zia_get_capabilities(provider, &caps) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	if (local_offload) {
		*local_offload = B_TRUE;
	}

	/* provider is checked by zia_ops */
	void *handle = zia_alloc(provider, size);
	if (!handle) {
		return (ZIA_ERROR);
	}

	zia_mv_t mv = {
		.handle = handle,
		.offset = 0,
	};

	int ret = ZIA_FALLBACK;
	if (abd_is_linear(abd) == B_TRUE) {
		ret = zia_ops->copy.from.generic(&mv,
		    ABD_LINEAR_BUF(abd), size);
		ret = zia_ops_to_ret(ret);
	} else if ((abd_is_linear_page(abd) == B_TRUE) ||
	    (abd_is_gang(abd) == B_TRUE)) {
		ret = abd_iterate_func(abd, offset, size,
		    zia_offload_generic_cb, &mv);

		if (ret == 0) {
			ret = ZIA_OK;
		}
	} else if (abd_is_from_pages(abd) == B_TRUE) {
#if defined(__linux__) && defined(_KERNEL)
		if (zia_ops->copy.from.scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.offset = scatter->abd_offset;
			ret = zia_ops->copy.from.scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
			ret = zia_ops_to_ret(ret);
		} else {
#endif
			ret = abd_iterate_func(abd, offset, size,
			    zia_offload_generic_cb, &mv);

			if (ret == 0) {
				ret = ZIA_OK;
			}
#if defined(__linux__) && defined(_KERNEL)
		}
#endif
	} else {
#if defined(__linux__) && defined(_KERNEL)
		if (zia_ops->copy.from.scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.offset = scatter->abd_offset;
			ret = zia_ops->copy.from.scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
			ret = zia_ops_to_ret(ret);
		} else {
#endif
			ret = abd_iterate_func(abd, offset, size,
			    zia_offload_generic_cb, &mv);

			if (ret == 0) {
				ret = ZIA_OK;
			}
#if defined(__linux__) && defined(_KERNEL)
		}
#endif
	}

	if (ret == ZIA_OK) {
		ABD_HANDLE(abd) = handle;
	} else {
		zia_free(&handle);
	}

	return (ret);
#else
	(void) provider; (void) abd; (void) offset;
	(void) size;
	(void) local_offload;
	return (ZIA_DISABLED);
#endif
}

int
zia_offload_abd(void *provider, abd_t *abd,
    size_t size, boolean_t *local_offload, boolean_t lock)
{
	if (!zia_ops || !provider) {
		return (ZIA_FALLBACK);
	}

	if (!abd) {
		return (ZIA_ERROR);
	}

	if (lock) {
		mutex_enter(&abd->abd_mtx);
	}

	const int rc = zia_offload_abd_offset(provider,
	    abd, 0, size, local_offload);

	if (lock) {
		mutex_exit(&abd->abd_mtx);
	}

	return (rc);
}

#ifdef ZIA
static int
zia_onload_generic_cb(void *buf, size_t len, void *priv)
{
	zia_mv_t *mv = (zia_mv_t *)priv;

	const int ret = zia_ops->copy.to.generic(mv, buf, len);
	if (zia_ops_to_ret(ret) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	mv->offset += len;
	return (0);
}
#endif

/* onload handle + 0 into abd + offset */
static int
zia_onload_abd_offset(abd_t *abd, size_t offset,
    size_t size, boolean_t keep_handle)
{
#ifdef ZIA
	if (!zia_ops) {
		/*
		 * The offloaded copy is unreachable. Drop the stale handle
		 * so that the abd no longer looks offloaded, and report the
		 * accelerator as down so the caller restarts from the
		 * in-memory source instead of retrying the onload.
		 */
		if (abd && ABD_HANDLE(abd)) {
			ABD_HANDLE(abd) = NULL;
			return (ZIA_ACCELERATOR_DOWN);
		}
		return (ZIA_FALLBACK);
	}

	if (!abd) {
		return (ZIA_ERROR);
	}

	void *handle = ABD_HANDLE(abd);
	if (!handle) {
		return (ZIA_ERROR);
	}

	void *provider = zia_ops->extract(handle);
	if (!provider) {
		return (ZIA_ERROR);
	}

	zia_caps_t caps;
	if (zia_get_capabilities(provider, &caps) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	zia_mv_t mv = {
		.handle = handle,
		.offset = 0,
	};

	int ret = ZIA_FALLBACK;
	if (abd_is_linear(abd) == B_TRUE) {
		ret = zia_ops->copy.to.generic(&mv,
		    ABD_LINEAR_BUF(abd), size);
		ret = zia_ops_to_ret(ret);
	} else if ((abd_is_linear_page(abd) == B_TRUE) ||
	    (abd_is_gang(abd) == B_TRUE)) {
		ret = abd_iterate_func(abd, offset, size,
		    zia_onload_generic_cb, &mv);

		if (ret == 0) {
			ret = ZIA_OK;
		}
	} else if (abd_is_from_pages(abd) == B_TRUE) {
#if defined(__linux__) && defined(_KERNEL)
		if (zia_ops->copy.to.scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.offset = scatter->abd_offset;
			ret = zia_ops->copy.to.scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
			ret = zia_ops_to_ret(ret);
		} else {
#endif
			ret = abd_iterate_func(abd, offset, size,
			    zia_onload_generic_cb, &mv);

			if (ret == 0) {
				ret = ZIA_OK;
			}
#if defined(__linux__) && defined(_KERNEL)
		}
#endif
	} else {
#if defined(__linux__) && defined(_KERNEL)
		if (zia_ops->copy.to.scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.offset = scatter->abd_offset;
			ret = zia_ops->copy.to.scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
			ret = zia_ops_to_ret(ret);
		} else {
#endif
			ret = abd_iterate_func(abd, offset, size,
			    zia_onload_generic_cb, &mv);

			if (ret == 0) {
				ret = ZIA_OK;
			}
#if defined(__linux__) && defined(_KERNEL)
		}
#endif
	}

	if (keep_handle != B_TRUE) {
		zia_free_abd(abd, B_FALSE);
	}

	return (ret);
#else
	(void) abd; (void) offset; (void) size; (void) keep_handle;
	return (ZIA_DISABLED);
#endif
}

int
zia_onload_abd(abd_t *abd, size_t size, boolean_t keep_handle)
{
	if (abd_is_gang(abd)) {
		/*
		 * the only gangs that show up are from raidz
		 *
		 * get leading data size, stopping at first zero page
		 * which should always be the second child
		 */
		abd_t *child = list_head(&ABD_GANG(abd).abd_gang_chain);
		const size_t original_size = size;
		size = child->abd_size;

		ASSERT(size <= original_size);
	}

	return (zia_onload_abd_offset(abd, 0, size, keep_handle));
}

int
zia_free_abd(abd_t *abd, boolean_t lock)
{
	if (lock == B_TRUE) {
		mutex_enter(&abd->abd_mtx);
	}

	const int ret = zia_free(&ABD_HANDLE(abd));

	if (lock == B_TRUE) {
		mutex_exit(&abd->abd_mtx);
	}
	return (ret);
}

/*
 * if offloaded locally, just free the handle
 * if not, onload the data and free the handle
 */
int
zia_cleanup_abd(abd_t *abd, size_t size,
    boolean_t local_offload, boolean_t lock)
{
	if (!abd) {
		return (zia_ops ? ZIA_ERROR : ZIA_FALLBACK);
	}

	if (!zia_ops && !ABD_HANDLE(abd)) {
		return (ZIA_FALLBACK);
	}

	int ret = ZIA_OK;

	if (lock == B_TRUE) {
		mutex_enter(&abd->abd_mtx);
	}

	if (!zia_ops) {
		/*
		 * Stale handle with no interface to free it. A local
		 * offload left the in-memory copy valid; otherwise the
		 * data is lost and the caller has to restart.
		 */
		ABD_HANDLE(abd) = NULL;
		ret = local_offload ? ZIA_OK : ZIA_ACCELERATOR_DOWN;
	} else if (local_offload == B_TRUE) {
		/* in-memory copy is still valid */
		/* lock just in case mirrors clean up at the same time */
		ret = zia_free_abd(abd, B_FALSE);
	} else {
		/* have to copy data into memory */
		ret = zia_onload_abd(abd, size, B_FALSE);
	}

	if (lock == B_TRUE) {
		mutex_exit(&abd->abd_mtx);
	}

	return (ret);
}

void
zia_restart_before_vdev(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	if (BP_IS_ENCRYPTED(bp) &&
	    (zio->io_stage != ZIO_STAGE_ENCRYPT)) {
		zio_pop_transform(zio);
	}

	if ((BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF) &&
	    (zio->io_stage != ZIO_STAGE_WRITE_COMPRESS)) {
		zio_pop_transform(zio);
		BP_SET_PSIZE(bp, zio->io_size);
		BP_SET_LSIZE(bp, zio->io_size);
		BP_SET_COMPRESS(bp, ZIO_COMPRESS_OFF);
	}

	zia_disable_offloading(zio, B_TRUE);

	/* only keep trace up to issue async */
	zio->io_pipeline_trace &=
	    ZIO_STAGE_OPEN |
	    ZIO_STAGE_READ_BP_INIT |
	    ZIO_STAGE_WRITE_BP_INIT |
	    ZIO_STAGE_FREE_BP_INIT |
	    ZIO_STAGE_ISSUE_ASYNC;

	/* let zio_execute find the stage after issue async */
	zio->io_stage = ZIO_STAGE_ISSUE_ASYNC;
}

int
zia_zero_fill(abd_t *abd, size_t offset, size_t size)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	if (!abd || !ABD_HANDLE(abd)) {
		return (ZIA_ERROR);
	}

	return (zia_ops_to_ret(zia_ops->zero_fill(ABD_HANDLE(abd),
	    offset, size)));
#else
	(void) abd; (void) offset; (void) size;
	return (ZIA_DISABLED);
#endif
}

int
zia_compress(zia_props_t *props, enum zio_compress c,
    abd_t *src, size_t s_len,
    abd_t **dst, uint64_t *d_len,
    uint8_t level, boolean_t *local_offload)
{
#ifdef ZIA
	if (!zia_ops || !props->provider) {
		return (ZIA_FALLBACK);
	}

	void *cbuf_handle = NULL;
	*dst = abd_alloc_sametype(src, s_len);
	const int rc = zia_compress_impl(zia_ops, props, c, src, s_len,
	    dst, &cbuf_handle, d_len, level, local_offload);
	if (rc != ZIA_OK) {
		abd_free(*dst);
		*dst = NULL;
		return (rc);
	}
	ABD_HANDLE(*dst) = cbuf_handle;
	return (rc);
#else
	(void) props; (void) c; (void) src; (void) s_len;
	(void) dst; (void) d_len; (void) level;
	(void) local_offload;
	return (ZIA_DISABLED);
#endif
}

int
zia_decompress(zia_props_t *props, enum zio_compress c,
    abd_t *src, size_t s_len, abd_t *dst, size_t d_len,
    uint8_t *level)
{
#ifdef ZIA
	if (!props) {
		return (ZIA_ERROR);
	}

	if (!zia_ops || !props->provider) {
		return (ZIA_FALLBACK);
	}

	/* check that decompression can be done before offloading src */
	zia_caps_t caps;
	if ((zia_get_capabilities(props->provider, &caps) != ZIA_OK) ||
	    !(caps.decompress & compress_to_zia(c))) {
		return (ZIA_FALLBACK);
	}

	int ret = zia_offload_abd(props->provider, src,
	    s_len, NULL, B_FALSE);
	if (ret != ZIA_OK) {
		return (ret);
	}

	/*
	 * allocate space for decompressed data
	 *
	 * a lot of these will fail because d_len tends to be small
	 */
	void *handle = zia_alloc(props->provider, d_len);
	ABD_HANDLE(dst) = handle;
	if (!ABD_HANDLE(dst)) {
		/* let abd_free clean up zio->io_abd */
		return (ZIA_ERROR);
	}
	if (abd_is_linear(dst) == B_TRUE) {
		zia_ops->associate_handle(handle, ABD_LINEAR_BUF(dst));
	}

	/*
	 * d_len pulled from accelerator is not used, so
	 * passing in address of local variable is fine
	 */
	int cmp_level = *level;
	ret = zia_ops->decompress(compress_to_zia(c), &cmp_level,
	    ABD_HANDLE(src), s_len, ABD_HANDLE(dst), &d_len);
	*level = cmp_level;

	if (ret != ZIA_OPS_OK) {
		zia_free_abd(dst, B_FALSE);
		/* let abd_free clean up zio->io_abd */
	}

	return (zia_ops_to_ret(ret));
#else
	(void) props; (void) c; (void) src; (void) s_len;
	(void) dst; (void) d_len; (void) level;
	return (ZIA_FALLBACK);
#endif
}

int
zia_checksum_compute(void *provider, zio_cksum_t *dst, enum zio_checksum alg,
    zio_t *zio, uint64_t size, boolean_t *local_offload)
{
#ifdef ZIA
	if (!zia_ops || !provider) {
		return (ZIA_FALLBACK);
	}

	const zia_byteorder_t byteorder =
	    byteorder_to_zia(BP_SHOULD_BYTESWAP(zio->io_bp));

	if (!ABD_HANDLE(zio->io_abd)) {
		zia_caps_t caps;
		if ((zia_get_capabilities(provider, &caps) != ZIA_OK) ||
		    !(caps.checksum & checksum_to_zia(alg)) ||
		    !(caps.checksum_byteorder & byteorder)) {
			return (ZIA_FALLBACK);
		}

		if (zia_offload_abd(provider, zio->io_abd, size,
		    local_offload, B_FALSE) != ZIA_OK) {
			return (ZIA_ERROR);
		}
	} else {
		void *old_provider = zia_ops->extract(ABD_HANDLE(zio->io_abd));
		if (old_provider != provider) {
			return (ZIA_PROVIDER_MISMATCH);
		}

		/* skip checks because zia_ops will do them */
	}

	return (zia_ops_to_ret(zia_ops->checksum(checksum_to_zia(alg),
	    byteorder, ABD_HANDLE(zio->io_abd), size, dst->zc_word,
	    sizeof (dst->zc_word))));
#else
	(void) provider; (void) dst; (void) alg;
	(void) zio; (void) size; (void) local_offload;
	return (ZIA_FALLBACK);
#endif
}

int
zia_checksum_error(enum zio_checksum alg, abd_t *abd,
    uint64_t size, int byteswap, zio_cksum_t *actual_cksum)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	if (!abd || !actual_cksum) {
		return (ZIA_ERROR);
	}

	if (!ABD_HANDLE(abd)) {
		return (ZIA_FALLBACK);
	}

	const zia_byteorder_t byteorder =
	    byteorder_to_zia(byteswap);

	return (zia_ops_to_ret(zia_ops->checksum(checksum_to_zia(alg),
	    byteorder, ABD_HANDLE(abd), size, actual_cksum->zc_word,
	    sizeof (actual_cksum->zc_word))));
#else
	(void) alg; (void) abd; (void) size;
	(void) byteswap; (void) actual_cksum;
	return (ZIA_FALLBACK);
#endif
}

#ifdef ZIA
static boolean_t
zia_can_raidz(raidz_row_t *rr, zia_props_t *props, uint64_t raidn,
    boolean_t rec, uint_t cksum, size_t *col_sizes)
{
	/*
	 * generation is needed for both
	 * generation and reconstruction
	 */
	int good = (
	    /* raidz generation is turned on */
	    (props->raidz.gen[raidn] == 1) &&

		/*
		 * the provider knows whether or not
		 * raidz functions are available
		 */
	    (zia_ops->raid.can_compute(props->provider, raidn,
	    rr->rr_cols - rr->rr_firstdatacol,
	    col_sizes, rec == B_TRUE) == ZIA_OPS_OK));

	if (good && (rec == B_TRUE)) {
		zia_caps_t caps;
		if (zia_get_capabilities(props->provider, &caps) != ZIA_OK) {
			return (B_FALSE);
		}

		good &= (
		    /* raidz reconstruction is turned on */
		    (props->raidz.rec[raidn] == 1) &&

		    /* need checksum */
		    (props->checksum == 1) &&

		    /* raidz reconstruction support was checked earlier */

		    /* make sure the checksum is supported by the provider */
		    (caps.checksum & checksum_to_zia(cksum)));
	}
	return (good?B_TRUE:B_FALSE);
}
#endif

/* onload abd and delete raidz_row_t stuff */
static int
zia_raidz_cleanup(zio_t *zio, raidz_row_t *rr,
    boolean_t local_offload, boolean_t onload_parity)
{
	/*
	 * bring data back to zio->io_abd, which should
	 * place data into parent automatically
	 */

	const int ret = zia_worst_error(
	    zia_raidz_free(rr, onload_parity),
	    zia_cleanup_abd(zio->io_abd, zio->io_size, local_offload, B_TRUE));

	return (ret);
}

int
zia_raidz_alloc(zio_t *zio, raidz_row_t *rr, boolean_t rec,
    uint_t cksum, boolean_t *local_offload)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	if (!zio || !rr) {
		return (ZIA_ERROR);
	}

	/* do not offload in the middle of resilvering */
	if (zio->io_flags & ZIO_FLAG_RESILVER) {
		if (!ABD_HANDLE(zio->io_abd)) {
			return (ZIA_FALLBACK);
		}
	}

	/*
	 * existence of row handle implies existence
	 * of data and column handles
	 */
	if (rr->rr_zia_handle) {
		return (ZIA_OK);
	}

	if (zio->io_can_offload != B_TRUE) {
		return (ZIA_ACCELERATOR_DOWN);
	}

	const uint64_t raidn = rr->rr_firstdatacol;
	if ((1 > raidn) || (raidn > 3)) {
		return (ZIA_ERROR);
	}

	/* need at least raidn + 2 columns */
	if (raidn + 2 > rr->rr_cols) {
		return (ZIA_ERROR);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);
	if (!props->provider) {
		return (ZIA_FALLBACK);
	}

	/* get column sizes */
	const size_t column_sizes_size = sizeof (size_t) * rr->rr_cols;
	size_t *column_sizes = kmem_alloc(column_sizes_size, KM_SLEEP);
	for (uint64_t c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];

		column_sizes[c] = rc->rc_size;
	}

	if (zia_can_raidz(rr, props, raidn, rec,
	    cksum, column_sizes) != B_TRUE) {
		kmem_free(column_sizes, column_sizes_size);
		return (ZIA_FALLBACK);
	}
	kmem_free(column_sizes, column_sizes_size);

	void *provider = props->provider;
	if (!provider) {
		return (ZIA_FALLBACK);
	}

	/*
	 * offload the source data if it hasn't already been offloaded
	 *
	 * need to lock here since offloading normally doesn't lock, but
	 * abds hitting raidz might have been mirrored
	 */
	const int ret = zia_offload_abd(provider, zio->io_abd,
	    zio->io_size, local_offload, B_TRUE);
	if (ret != ZIA_OK) {
		return (ret);
	}

	/* mirrored abds generate their own references to the columns */

	/* set up raid context */
	rr->rr_zia_handle = zia_ops->raid.alloc(provider,
	    raidn, rr->rr_cols - raidn);

	if (!rr->rr_zia_handle) {
		return (ZIA_ERROR);
	}

	/* fill in raid context */

	/* create parity column handles */
	for (uint64_t c = 0; c < raidn; c++) {
		raidz_col_t *rc = &rr->rr_col[c];

		ASSERT(!ABD_HANDLE(rc->rc_abd));

		void *handle = NULL;

		/* allocate rc->rc_abd->abd_size, mark as rc->rc_size */
		if (rec == B_TRUE) {
			/*
			 * reconstructing - parity columns are not
			 * in zio->io_abd - offload rc->rc_abd
			 */
			zia_offload_abd(provider, rc->rc_abd,
			    rc->rc_abd->abd_size, NULL, B_FALSE);
			handle = ABD_HANDLE(rc->rc_abd);
		} else {
			/* generating - create new columns */
			handle =
			    zia_ops->alloc(provider, rc->rc_abd->abd_size);
		}

		if (!handle) {
			goto error;
		}

		if (abd_is_linear(rc->rc_abd) == B_TRUE) {
			zia_ops->associate_handle(handle,
			    ABD_LINEAR_BUF(rc->rc_abd));
		}

		if (zia_ops->raid.set_column(rr->rr_zia_handle,
		    c, handle, rc->rc_size) != ZIA_OPS_OK) {
			goto error;
		}

		ABD_HANDLE(rc->rc_abd) = handle;
	}

	/*
	 * recalculate data column offsets and
	 * create references for each column
	 */
	uint64_t offset = 0;
	for (uint64_t c = raidn; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];

		/*
		 * if the column is a gang abd, the handle
		 * will point to the first child
		 */
		void *handle = zia_ops->alloc_ref(ABD_HANDLE(zio->io_abd),
		    offset, rc->rc_size);

		if (!handle) {
			goto error;
		}

		if (zia_ops->raid.set_column(rr->rr_zia_handle,
		    c, handle, rc->rc_size) != ZIA_OPS_OK) {
			goto error;
		}

		ABD_HANDLE(rc->rc_abd) = handle;
		offset += rc->rc_size;
	}

	for (uint64_t c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		ASSERT(zia_is_offloaded(rc->rc_abd) == B_TRUE);
	}
	ASSERT(rr->rr_zia_handle);
	ASSERT(zia_is_offloaded(zio->io_abd) == B_TRUE);

	return (ZIA_OK);

error:
	zia_raidz_cleanup(zio, rr, local_offload?*local_offload:B_FALSE,
	    B_FALSE);

	for (uint64_t c = 0; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		ASSERT(zia_is_offloaded(rc->rc_abd) == B_FALSE);
	}
	ASSERT(rr->rr_zia_handle == NULL);
	ASSERT(zia_is_offloaded(zio->io_abd) == B_FALSE);

	return (ZIA_ERROR);
#else
	(void) zio; (void) rr; (void) rec;
	(void) cksum; (void) local_offload;
	return (ZIA_FALLBACK);
#endif

}

/*
 * only frees the raidz data
 * onload the data separately if it is needed
 */
int
zia_raidz_free(raidz_row_t *rr, boolean_t onload_parity)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	if (!rr) {
		return (ZIA_ERROR);
	}

	if (!rr->rr_zia_handle) {
		return (ZIA_FALLBACK);
	}

	int ret = ZIA_OK;
	uint64_t c = 0;

	if (onload_parity == B_TRUE) {
		for (; c < rr->rr_firstdatacol; c++) {
			raidz_col_t *rc = &rr->rr_col[c];
			ret = zia_worst_error(ret,
			    zia_onload_abd(rc->rc_abd,
			    rc->rc_size, B_FALSE));
		}
	}

	for (; c < rr->rr_cols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		ret = zia_worst_error(ret,
		    zia_free_abd(rc->rc_abd, B_FALSE));
	}

	ret = zia_worst_error(ret,
	    zia_ops_to_ret(zia_ops->raid.free(
	    rr->rr_zia_handle)));
	rr->rr_zia_handle = NULL;

	return (ret);
#else
	(void) rr; (void) onload_parity;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_gen(raidz_row_t *rr)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	/* can only pass if raidz_alloc succeeded */
	if (!rr->rr_zia_handle) {
		return (ZIA_ERROR);
	}

	return (zia_ops_to_ret(zia_ops->raid.gen(rr->rr_zia_handle)));
#else
	(void) rr;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_gen_cleanup(zio_t *zio, raidz_row_t *rr,
    boolean_t local_offload)
{
	/*
	 * RAIDZ generation only calls cleanup
	 * on failure, so parity does not need
	 * to be brought back.
	 */
	return (zia_raidz_cleanup(zio, rr,
	    local_offload, B_FALSE));
}

/*
 * allocate new parity columns for this row
 * and assign them to the raidz struct
 *
 * orig takes ownership of the original handles
 */
int
zia_raidz_new_parity(zio_t *zio, raidz_row_t *rr, uint64_t c)
{
#ifdef ZIA
	if (!zio || !rr || (c >= rr->rr_firstdatacol)) {
		return (ZIA_ERROR);
	}

	if (!ABD_HANDLE(zio->io_abd) || !rr->rr_zia_handle) {
		return (ZIA_FALLBACK);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);
	void *provider = props->provider;
	if (!provider) {
		return (ZIA_FALLBACK);
	}

	raidz_col_t *rc = &rr->rr_col[c];
	if (ABD_HANDLE(rc->rc_abd)) {
		return (ZIA_ERROR);
	}

	void *new_parity_handle = zia_alloc(provider,
	    rc->rc_abd->abd_size);
	if (!new_parity_handle) {
		return (ZIA_ERROR);
	}

	if (abd_is_linear(rc->rc_abd) == B_TRUE) {
		zia_ops->associate_handle(new_parity_handle,
		    ABD_LINEAR_BUF(rc->rc_abd));
	}

	const int ret = zia_ops->raid.set_column(rr->rr_zia_handle,
	    c, new_parity_handle, rc->rc_size);
	if (ret == ZIA_OPS_OK) {
		ABD_HANDLE(rc->rc_abd) = new_parity_handle;
	} else {
		zia_free(&new_parity_handle);
	}

	return (zia_ops_to_ret(ret));
#else
	(void) zio; (void) rr; (void) c;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_cmp(abd_t *lhs, abd_t *rhs, int *diff)
{
#ifdef ZIA
	if (!lhs || !rhs || !diff) {
		return (ZIA_ERROR);
	}

	if (lhs == rhs) {
		*diff = 0;
		return (ZIA_OK);
	}

	void *lhs_handle = ABD_HANDLE(lhs);
	void *rhs_handle = ABD_HANDLE(rhs);
	if (!lhs_handle || !rhs_handle) {
		return (ZIA_ERROR);
	}

	return (zia_ops_to_ret(zia_ops->raid.cmp(lhs_handle, rhs_handle,
	    diff)));
#else
	(void) lhs; (void) rhs; (void) diff;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_rec(raidz_row_t *rr, int *t, int nt)
{
#ifdef ZIA
	if (!zia_ops) {
		return (ZIA_FALLBACK);
	}

	/* can only pass if raidz_alloc succeeded */
	if (!rr->rr_zia_handle) {
		return (ZIA_FALLBACK);
	}

	return (zia_ops_to_ret(zia_raidz_rec_impl(zia_ops, rr, t, nt)));
#else
	(void) rr; (void) t; (void) nt;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_rec_cleanup(zio_t *zio, raidz_row_t *rr,
    boolean_t local_offload, boolean_t onload_parity)
{
	return (zia_raidz_cleanup(zio, rr,
	    local_offload, onload_parity));
}

int
zia_file_open(vdev_t *vdev, const char *path,
    int flags, int mode)
{
	if (!vdev || !vdev->vdev_spa) {
		return (ZIA_ERROR);
	}

	zia_props_t *zia_props = zia_get_props(vdev->vdev_spa);
	if (zia_props->file_write != 1) {
		return (ZIA_FALLBACK);
	}

#ifdef ZIA
	void *provider = zia_props->provider;
	if (!zia_ops || !provider) {
		return (ZIA_FALLBACK);
	}

	if (!VDEV_HANDLE(vdev)) {
		VDEV_HANDLE(vdev) = zia_ops->file.open(provider,
		    path, flags, mode);
	}

	return (VDEV_HANDLE(vdev)?ZIA_OK:ZIA_ERROR);
#else
	(void) path; (void) flags; (void) mode;
	return (ZIA_FALLBACK);
#endif
}

int
zia_file_write(vdev_t *vdev, abd_t *abd, ssize_t size,
    loff_t offset, uint8_t ashift, ssize_t *resid, int *err)
{
#ifdef ZIA
	if (!vdev || !abd) {
		return (ZIA_ERROR);
	}

	if (!zia_ops || !VDEV_HANDLE(vdev) || !ABD_HANDLE(abd)) {
		return (ZIA_FALLBACK);
	}

	/*
	 * this was intended to handle gang abds, but breaking
	 * at first zero child abd was not correct
	 */
	size_t data_size = size;
	size_t trailing_zeros = 0;

	return (zia_ops->file.write(VDEV_HANDLE(vdev), ABD_HANDLE(abd),
	    data_size, trailing_zeros, offset, ashift, resid, err));
#else
	(void) vdev; (void) abd; (void) size;
	(void) offset; (void) ashift; (void) resid; (void) err;
	return (ZIA_FALLBACK);
#endif
}

int
zia_file_close(vdev_t *vdev)
{
#ifdef ZIA
	if (!vdev) {
		return (ZIA_ERROR);
	}

	void *handle = VDEV_HANDLE(vdev);
	VDEV_HANDLE(vdev) = NULL;

	if (!zia_ops || !handle) {
		return (ZIA_FALLBACK);
	}

	zia_ops->file.close(handle);

	return (ZIA_OK);
#else
	(void) vdev;
	return (ZIA_FALLBACK);
#endif
}

#ifdef __linux__
#ifdef _KERNEL
int
zia_disk_open(vdev_t *vdev, const char *path,
    struct block_device *bdev)
{
#ifdef ZIA
	if (!vdev || !vdev->vdev_spa) {
		return (ZIA_ERROR);
	}

	zia_props_t *props = zia_get_props(vdev->vdev_spa);
	void *provider = props->provider;

	if (!zia_ops || !provider || props->disk_write != 1) {
		return (ZIA_FALLBACK);
	}

	if (!VDEV_HANDLE(vdev)) {
		VDEV_HANDLE(vdev) = zia_ops->disk.open(provider,
		    path, bdev);
	}

	return (VDEV_HANDLE(vdev)?ZIA_OK:ZIA_ERROR);
#else
	(void) vdev; (void) path; (void) bdev;
	return (ZIA_FALLBACK);
#endif
}

int
zia_disk_invalidate(vdev_t *vdev)
{
#ifdef ZIA
	if (!vdev) {
		return (ZIA_ERROR);
	}

	if (!zia_ops || !VDEV_HANDLE(vdev)) {
		return (ZIA_FALLBACK);
	}

	return (zia_ops_to_ret(zia_ops->disk.invalidate(VDEV_HANDLE(vdev))));
#else
	(void) vdev;
	return (ZIA_FALLBACK);
#endif
}

int
zia_disk_write(vdev_t *vdev, zio_t *zio, int flags, boolean_t *local_offload)
{
#ifdef ZIA
	if (!vdev || !zio || !zio->io_abd || !local_offload) {
		return (EIO);
	}

	if (!zia_ops || !VDEV_HANDLE(vdev)) {
		return (EIO);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);

	zia_caps_t caps;
	if (zia_get_capabilities(props->provider, &caps) != ZIA_OK) {
		return (EIO);
	}

	/* cannot offload disk_write */
	if ((caps.io & ZIA_IO_DISK) == 0) {
		return (EIO);
	}

	/*
	 * disk_write offloading was not enabled
	 * or
	 * zio should not be offloaded
	 */
	if ((props->disk_write == 0) ||
	    (zio->io_can_offload == B_FALSE)) {
		return (EIO);
	}

	if (zia_offload_abd(props->provider, zio->io_abd,
	    zio->io_size, local_offload, B_TRUE) != ZIA_OK) {
		return (EIO);
	}

	/* returns E errors */
	return (zia_ops->disk.write(VDEV_HANDLE(vdev), ABD_HANDLE(zio->io_abd),
	    zio->io_size, 0, zio->io_offset, flags,
	    zia_disk_write_completion, zio));
#else
	(void) vdev; (void) zio; (void) flags; (void) local_offload;
	return (EIO);
#endif
}

int
zia_disk_flush(vdev_t *vdev, zio_t *zio)
{
#ifdef ZIA
	if (!vdev || !zio) {
		return (EIO);
	}

	if (!zia_ops || !VDEV_HANDLE(vdev)) {
		return (EIO);
	}

	return (zia_ops->disk.flush(VDEV_HANDLE(vdev),
	    zia_disk_flush_completion, zio));
#else
	(void) vdev; (void) zio;
	return (EIO);
#endif
}

int
zia_disk_close(vdev_t *vdev)
{
#ifdef ZIA
	if (!vdev) {
		return (ZIA_ERROR);
	}

	void *handle = VDEV_HANDLE(vdev);
	VDEV_HANDLE(vdev) = NULL;

	if (!zia_ops || !handle) {
		return (ZIA_FALLBACK);
	}

	/* trust that ZFS handles closing disks once */
	zia_ops->disk.close(handle);

	return (ZIA_OK);
#else
	(void) vdev;
	return (ZIA_FALLBACK);
#endif
}
#endif
#endif
