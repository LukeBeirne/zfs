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

#include <sys/zia_ops.h>

#if defined(_KERNEL) && defined(__linux__)
#include <linux/module.h>
#include <linux/rwsem.h>
#endif

/*
 * Accelerator operations attached by a separately built module
 * (for example, zia-dpusm.ko).  Until a table is attached, every
 * Z.I.A. function falls back to ZFS.
 *
 * zia_ops can only change while no pool holds a provider: each
 * provider handle returned by zia_get_provider() holds a reference
 * on the owner module and is counted in zia_ops_users, and
 * zia_ops_unregister() fails with EBUSY while that count is non-zero.
 * Handles to accelerator memory are only created through a provider,
 * so a table in use cannot be detached.  zia_ops_lock serializes
 * attach, detach, get and put.
 */
static const zia_ops_t *zia_ops = NULL;

#if defined(_KERNEL) && defined(__linux__)
static struct module *zia_ops_owner = NULL;
static uint64_t zia_ops_users = 0;
static DECLARE_RWSEM(zia_ops_lock);

int
zia_ops_register(const zia_ops_t *ops, struct module *owner)
{
	if (ops == NULL || owner == NULL)
		return (EINVAL);

	if (ops->zo_version != ZIA_OPS_VERSION ||
	    ops->zo_size != sizeof (zia_ops_t)) {
		printk(KERN_ERR "Z.I.A.: rejecting operations table from %s: "
		    "version %u size %u, expected version %u size %zu\n",
		    module_name(owner), ops->zo_version, ops->zo_size,
		    ZIA_OPS_VERSION, sizeof (zia_ops_t));
		return (ENOTSUP);
	}

	down_write(&zia_ops_lock);
	if (zia_ops != NULL) {
		up_write(&zia_ops_lock);
		return (EBUSY);
	}
	zia_ops_owner = owner;
	WRITE_ONCE(zia_ops, ops);
	up_write(&zia_ops_lock);

	printk(KERN_INFO "Z.I.A.: attached to %s\n", module_name(owner));
	return (0);
}
EXPORT_SYMBOL(zia_ops_register);

int
zia_ops_unregister(const zia_ops_t *ops)
{
	down_write(&zia_ops_lock);
	if (ops == NULL || zia_ops != ops) {
		up_write(&zia_ops_lock);
		return (EINVAL);
	}
	if (zia_ops_users != 0) {
		up_write(&zia_ops_lock);
		return (EBUSY);
	}
	printk(KERN_INFO "Z.I.A.: detached from %s\n",
	    module_name(zia_ops_owner));
	WRITE_ONCE(zia_ops, NULL);
	zia_ops_owner = NULL;
	up_write(&zia_ops_lock);
	return (0);
}
EXPORT_SYMBOL(zia_ops_unregister);
#endif

static inline const zia_ops_t *
zia_get_ops(void)
{
#if defined(_KERNEL) && defined(__linux__)
	return (READ_ONCE(zia_ops));
#else
	return (zia_ops);
#endif
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

boolean_t
zia_is_attached(void)
{
	return (zia_get_ops() != NULL);
}

int
zia_get_capabilities(void *provider, zia_caps_t *caps)
{
	const zia_ops_t *ops = zia_get_ops();
	if (!ops || !provider || !caps)
		return (ZIA_ERROR);

	memset(caps, 0, sizeof (*caps));
	return (ops->zo_capabilities(provider, caps));
}

int
zia_init(void)
{
#ifdef ZIA
	return (ZIA_OK);
#else
	return (ZIA_DISABLED);
#endif
}

int
zia_fini(void)
{
	/*
	 * zia_ops is owned by the attaching module, which depends on zfs
	 * and so must be unloaded (and must have detached) before zfs.
	 */
	return (ZIA_OK);
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
 * Get a provider handle from the attached operations table.  The
 * handle holds a reference on the table's owner module, which is
 * dropped by zia_put_provider().
 */
void *
zia_get_provider(const char *name)
{
#if defined(ZIA) && defined(_KERNEL) && defined(__linux__)
	void *provider = NULL;

	down_write(&zia_ops_lock);
	const zia_ops_t *ops = zia_ops;
	if (ops == NULL) {
		up_write(&zia_ops_lock);
		printk(KERN_WARNING "Z.I.A.: cannot get provider \"%s\": "
		    "no accelerator interface is attached\n", name);
		return (NULL);
	}

	if (!try_module_get(zia_ops_owner)) {
		up_write(&zia_ops_lock);
		return (NULL);
	}

	provider = ops->zo_get(name);
	if (provider != NULL)
		zia_ops_users++;
	else
		module_put(zia_ops_owner);
	up_write(&zia_ops_lock);

	printk(KERN_INFO "Z.I.A. obtained handle to provider \"%s\" (%p)\n",
	    name, provider);

	return (provider);
#else
	(void) name;
	return (NULL);
#endif
}

const char *
zia_get_provider_name(void *provider)
{
	const zia_ops_t *ops = zia_get_ops();

	if (!ops || !provider) {
		return (NULL);
	}

	return (ops->zo_get_name(provider));
}

#if defined(ZIA) && defined(_KERNEL) && defined(__linux__)
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
#if defined(ZIA) && defined(_KERNEL) && defined(__linux__)
	const zia_ops_t *ops = zia_get_ops();

	if (!provider || !*provider) {
		return (ZIA_FALLBACK);
	}

	/* a provider handle keeps the operations table attached */
	VERIFY3P(ops, !=, NULL);

	/*
	 * if the zpool is not going down, but the provider is going away,
	 * make sure the vdevs don't keep pointing to the invalid provider
	 */
	if (vdev) {
		spa_vdev_state_enter(vdev->vdev_spa, SCL_NONE);
		zia_close_vdevs(vdev);
		(void) spa_vdev_state_exit(vdev->vdev_spa, NULL, 0);
	}

	const char *name = zia_get_provider_name(*provider);
	const int ret = ops->zo_put(*provider);

	printk(KERN_INFO "Z.I.A. returned provider handle \"%s\" "
	    "(%p) and got return value %d\n",
	    name, *provider, ret);

	*provider = NULL;

	down_write(&zia_ops_lock);
	ASSERT3U(zia_ops_users, >, 0);
	zia_ops_users--;
	struct module *owner = zia_ops_owner;
	up_write(&zia_ops_lock);
	module_put(owner);

	return (ret);
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
	const zia_ops_t *ops = zia_get_ops();

	if (!zio) {
		return (B_FALSE);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);

	/* provider + at least 1 operation */
	if (ops && props->provider &&
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
	const zia_ops_t *ops = zia_get_ops();

	return ((ops && provider)?ops->zo_alloc(provider, size):NULL);
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
	const zia_ops_t *ops = zia_get_ops();

	/*
	 * Without an attached table the handle cannot be freed.  Drop it
	 * and report the accelerator as down so callers stop using it.
	 */
	if (!ops) {
		const int ret = *handle ? ZIA_ACCELERATOR_DOWN : ZIA_OK;
		*handle = NULL;
		return (ret);
	}

	const int ret = ops->zo_free(*handle);
	*handle = NULL;
	return (ret);
#else
	return (ZIA_DISABLED);
#endif
}

/* move data from the offloader into a linear abd and unregister the mapping */
int
zia_onload(void **handle, void *buf, size_t size)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	if (!handle || !*handle || !buf) {
		return (ZIA_ERROR);
	}

	void *provider = ops->zo_extract(*handle);
	if (!provider) {
		return (ZIA_ERROR);
	}

	zia_caps_t caps;
	if (zia_get_capabilities(provider, &caps) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	zia_mv_t mv = { .zm_handle = *handle, .zm_offset = 0 };
	int ret = ZIA_ERROR;

	if (caps.zc_optional & ZIA_OPTIONAL_COPY_TO_PTR) {
		ret = ops->zo_copy_to_ptr(&mv, buf, size);
	} else {
		ret = ops->zo_copy_to_generic(&mv, buf, size);
	}

	/*
	 * if success, no more need for handle
	 * if failure, can't do anything with
	 * handle in any case, so destroy it
	 */
	zia_free(handle);

	return (ret);
#else
	(void) handle; (void) buf; (void) size;
	return (ZIA_DISABLED);
#endif
}

#ifdef ZIA
static int
zia_offload_generic_cb(void *buf, size_t len, void *priv)
{
	const zia_ops_t *ops = zia_get_ops();

	zia_mv_t *mv = (zia_mv_t *)priv;

	const int ret = ops->zo_copy_from_generic(mv, buf, len);
	if (ret != ZIA_OK) {
		return (ZIA_ERROR);
	}

	mv->zm_offset += len;
	return (0);
}
#endif

/* offload abd + offset to handle + 0 */
static int
zia_offload_abd_offset(void *provider, abd_t *abd,
    size_t offset, size_t size, boolean_t *local_offload)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	/* already offloaded */
	if (ABD_HANDLE(abd)) {
		if (local_offload) {
			*local_offload = B_FALSE;
		}

		if (!provider) {
			return (ZIA_OK);
		}

		void *abd_provider = ops->zo_extract(ABD_HANDLE(abd));
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

	/* provider is checked by the accelerator interface */
	void *handle = zia_alloc(provider, size);
	if (!handle) {
		return (ZIA_ERROR);
	}

	zia_mv_t mv = {
		.zm_handle = handle,
		.zm_offset = 0,
	};

	int ret = ZIA_FALLBACK;
	if (abd_is_linear(abd) == B_TRUE) {
		ret = ops->zo_copy_from_generic(&mv,
		    ABD_LINEAR_BUF(abd), size);
	} else if ((abd_is_linear_page(abd) == B_TRUE) ||
	    (abd_is_gang(abd) == B_TRUE)) {
		ret = abd_iterate_func(abd, offset, size,
		    zia_offload_generic_cb, &mv);

		if (ret == 0) {
			ret = ZIA_OK;
		}
	} else if (abd_is_from_pages(abd) == B_TRUE) {
#if defined(__linux__) && defined(_KERNEL)
		if (ops->zo_copy_from_scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.zm_offset = scatter->abd_offset;
			ret = ops->zo_copy_from_scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
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
		if (ops->zo_copy_from_scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.zm_offset = scatter->abd_offset;
			ret = ops->zo_copy_from_scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops || !provider) {
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
	const zia_ops_t *ops = zia_get_ops();

	zia_mv_t *mv = (zia_mv_t *)priv;

	const int ret = ops->zo_copy_to_generic(mv, buf, len);
	if (ret != ZIA_OK) {
		return (ZIA_ERROR);
	}

	mv->zm_offset += len;
	return (0);
}
#endif

/* onload handle + 0 into abd + offset */
static int
zia_onload_abd_offset(abd_t *abd, size_t offset,
    size_t size, boolean_t keep_handle)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		/* a handle without a table can never be onloaded */
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

	void *provider = ops->zo_extract(handle);
	if (!provider) {
		return (ZIA_ERROR);
	}

	zia_caps_t caps;
	if (zia_get_capabilities(provider, &caps) != ZIA_OK) {
		return (ZIA_ERROR);
	}

	zia_mv_t mv = {
		.zm_handle = handle,
		.zm_offset = 0,
	};

	int ret = ZIA_FALLBACK;
	if (abd_is_linear(abd) == B_TRUE) {
		ret = ops->zo_copy_to_generic(&mv,
		    ABD_LINEAR_BUF(abd), size);
	} else if ((abd_is_linear_page(abd) == B_TRUE) ||
	    (abd_is_gang(abd) == B_TRUE)) {
		ret = abd_iterate_func(abd, offset, size,
		    zia_onload_generic_cb, &mv);

		if (ret == 0) {
			ret = ZIA_OK;
		}
	} else if (abd_is_from_pages(abd) == B_TRUE) {
#if defined(__linux__) && defined(_KERNEL)
		if (ops->zo_copy_to_scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.zm_offset = scatter->abd_offset;
			ret = ops->zo_copy_to_scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
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
		if (ops->zo_copy_to_scatterlist) {
			struct abd_scatter *scatter = &ABD_SCATTER(abd);
			mv.zm_offset = scatter->abd_offset;
			ret = ops->zo_copy_to_scatterlist(&mv,
			    scatter->abd_sgl, scatter->abd_nents, size);
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	if (!abd) {
		return (ZIA_ERROR);
	}

	int ret = ZIA_OK;

	if (lock == B_TRUE) {
		mutex_enter(&abd->abd_mtx);
	}

	if (local_offload == B_TRUE) {
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	if (!abd || !ABD_HANDLE(abd)) {
		return (ZIA_ERROR);
	}

	return (ops->zo_zero_fill(ABD_HANDLE(abd), offset, size));
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops || !props->provider) {
		return (ZIA_FALLBACK);
	}

	void *cbuf_handle = NULL;
	*dst = abd_alloc_sametype(src, s_len);
	const int rc = zia_compress_impl(ops, props, c, src, s_len,
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
	const zia_ops_t *ops = zia_get_ops();

	if (!props) {
		return (ZIA_ERROR);
	}

	if (!ops || !props->provider) {
		return (ZIA_FALLBACK);
	}

	/* check that decompression can be done before offloading src */
	zia_caps_t caps;
	if ((zia_get_capabilities(props->provider, &caps) != ZIA_OK) ||
	    !(caps.zc_decompress & ZIA_CAP_BIT(c))) {
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
		ops->zo_associate_handle(handle, ABD_LINEAR_BUF(dst));
	}

	/*
	 * d_len pulled from accelerator is not used, so
	 * passing in address of local variable is fine
	 */
	int cmp_level = *level;
	ret = ops->zo_decompress(c, &cmp_level,
	    ABD_HANDLE(src), s_len, ABD_HANDLE(dst), &d_len);
	*level = cmp_level;

	if (ret != ZIA_OK) {
		zia_free_abd(dst, B_FALSE);
		/* let abd_free clean up zio->io_abd */
	}

	return (ret);
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops || !provider) {
		return (ZIA_FALLBACK);
	}

	const zio_byteorder_t byteorder = BP_SHOULD_BYTESWAP(zio->io_bp) ?
	    ZIO_CHECKSUM_BYTESWAP : ZIO_CHECKSUM_NATIVE;

	if (!ABD_HANDLE(zio->io_abd)) {
		zia_caps_t caps;
		if ((zia_get_capabilities(provider, &caps) != ZIA_OK) ||
		    !(caps.zc_checksum & ZIA_CAP_BIT(alg)) ||
		    !(caps.zc_checksum_byteorder & ZIA_CAP_BIT(byteorder))) {
			return (ZIA_FALLBACK);
		}

		if (zia_offload_abd(provider, zio->io_abd, size,
		    local_offload, B_FALSE) != ZIA_OK) {
			return (ZIA_ERROR);
		}
	} else {
		void *old_provider = ops->zo_extract(ABD_HANDLE(zio->io_abd));
		if (old_provider != provider) {
			return (ZIA_PROVIDER_MISMATCH);
		}

		/* skip checks because the accelerator interface does them */
	}

	return (ops->zo_checksum(alg,
	    byteorder, ABD_HANDLE(zio->io_abd), size, dst->zc_word,
	    sizeof (dst->zc_word)));
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	if (!abd || !actual_cksum) {
		return (ZIA_ERROR);
	}

	if (!ABD_HANDLE(abd)) {
		return (ZIA_FALLBACK);
	}

	const zio_byteorder_t byteorder = byteswap ?
	    ZIO_CHECKSUM_BYTESWAP : ZIO_CHECKSUM_NATIVE;

	return (ops->zo_checksum(alg,
	    byteorder, ABD_HANDLE(abd), size, actual_cksum->zc_word,
	    sizeof (actual_cksum->zc_word)));
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
	const zia_ops_t *ops = zia_get_ops();

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
	    (ops->zo_raid_can_compute(props->provider, raidn,
	    rr->rr_cols - rr->rr_firstdatacol,
	    col_sizes, rec == B_TRUE) == ZIA_OK));

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
		    (caps.zc_checksum & ZIA_CAP_BIT(cksum)));
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
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
	rr->rr_zia_handle = ops->zo_raid_alloc(provider,
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
			    ops->zo_alloc(provider, rc->rc_abd->abd_size);
		}

		if (!handle) {
			goto error;
		}

		if (abd_is_linear(rc->rc_abd) == B_TRUE) {
			ops->zo_associate_handle(handle,
			    ABD_LINEAR_BUF(rc->rc_abd));
		}

		if (ops->zo_raid_set_column(rr->rr_zia_handle,
		    c, handle, rc->rc_size) != ZIA_OK) {
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
		void *handle = ops->zo_alloc_ref(ABD_HANDLE(zio->io_abd),
		    offset, rc->rc_size);

		if (!handle) {
			goto error;
		}

		if (ops->zo_raid_set_column(rr->rr_zia_handle,
		    c, handle, rc->rc_size) != ZIA_OK) {
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
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
	    (ops->zo_raid_free(
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
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	/* can only pass if raidz_alloc succeeded */
	if (!rr->rr_zia_handle) {
		return (ZIA_ERROR);
	}

	return (ops->zo_raid_gen(rr->rr_zia_handle));
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
	const zia_ops_t *ops = zia_get_ops();

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
		ops->zo_associate_handle(new_parity_handle,
		    ABD_LINEAR_BUF(rc->rc_abd));
	}

	const int ret = ops->zo_raid_set_column(rr->rr_zia_handle,
	    c, new_parity_handle, rc->rc_size);
	if (ret == ZIA_OK) {
		ABD_HANDLE(rc->rc_abd) = new_parity_handle;
	} else {
		zia_free(&new_parity_handle);
	}

	return (ret);
#else
	(void) zio; (void) rr; (void) c;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_cmp(abd_t *lhs, abd_t *rhs, int *diff)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

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

	return (ops->zo_raid_cmp(lhs_handle, rhs_handle, diff));
#else
	(void) lhs; (void) rhs; (void) diff;
	return (ZIA_FALLBACK);
#endif
}

int
zia_raidz_rec(raidz_row_t *rr, int *t, int nt)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	if (!ops) {
		return (ZIA_FALLBACK);
	}

	/* can only pass if raidz_alloc succeeded */
	if (!rr->rr_zia_handle) {
		return (ZIA_FALLBACK);
	}

	return (zia_raidz_rec_impl(ops, rr, t, nt));
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
	const zia_ops_t *ops = zia_get_ops();
	void *provider = zia_props->provider;
	if (!ops || !provider) {
		return (ZIA_FALLBACK);
	}

	if (!VDEV_HANDLE(vdev)) {
		VDEV_HANDLE(vdev) = ops->zo_file_open(provider,
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
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev || !abd) {
		return (ZIA_ERROR);
	}

	if (!ops || !VDEV_HANDLE(vdev) || !ABD_HANDLE(abd)) {
		return (ZIA_FALLBACK);
	}

	/*
	 * this was intended to handle gang abds, but breaking
	 * at first zero child abd was not correct
	 */
	size_t data_size = size;
	size_t trailing_zeros = 0;

	return (ops->zo_file_write(VDEV_HANDLE(vdev), ABD_HANDLE(abd),
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
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev) {
		return (ZIA_ERROR);
	}

	if (!ops || !VDEV_HANDLE(vdev)) {
		return (ZIA_FALLBACK);
	}

	ops->zo_file_close(VDEV_HANDLE(vdev));
	VDEV_HANDLE(vdev) = NULL;

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
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev || !vdev->vdev_spa) {
		return (ZIA_ERROR);
	}

	zia_props_t *props = zia_get_props(vdev->vdev_spa);
	void *provider = props->provider;

	if (!ops || !provider || props->disk_write != 1) {
		return (ZIA_FALLBACK);
	}

	if (!VDEV_HANDLE(vdev)) {
		VDEV_HANDLE(vdev) = ops->zo_disk_open(provider,
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
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev) {
		return (ZIA_ERROR);
	}

	if (!ops || !VDEV_HANDLE(vdev)) {
		return (ZIA_FALLBACK);
	}

	return (ops->zo_disk_invalidate(VDEV_HANDLE(vdev)));
#else
	(void) vdev;
	return (ZIA_FALLBACK);
#endif
}

int
zia_disk_write(vdev_t *vdev, zio_t *zio, int flags, boolean_t *local_offload)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev || !zio || !zio->io_abd || !local_offload) {
		return (EIO);
	}

	if (!ops || !VDEV_HANDLE(vdev)) {
		return (EIO);
	}

	zia_props_t *props = zia_get_props(zio->io_spa);

	zia_caps_t caps;
	if (zia_get_capabilities(props->provider, &caps) != ZIA_OK) {
		return (EIO);
	}

	/* cannot offload disk_write */
	if ((caps.zc_io & ZIA_IO_DISK) == 0) {
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
	return (ops->zo_disk_write(VDEV_HANDLE(vdev), ABD_HANDLE(zio->io_abd),
	    zio->io_size, 0, zio->io_offset, flags,
	    zia_disk_write_completion, zio));
#else
	(void) vdev; (void) zio; (void) io_size;
	(void) io_offset; (void) flags;
	return (ZIA_FALLBACK);
#endif
}

int
zia_disk_flush(vdev_t *vdev, zio_t *zio)
{
#ifdef ZIA
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev || !zio) {
		return (EIO);
	}

	if (!ops || !VDEV_HANDLE(vdev)) {
		return (EIO);
	}

	return (ops->zo_disk_flush(VDEV_HANDLE(vdev),
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
	const zia_ops_t *ops = zia_get_ops();

	if (!vdev) {
		return (ZIA_ERROR);
	}

	void *handle = VDEV_HANDLE(vdev);
	VDEV_HANDLE(vdev) = NULL;

	if (!ops || !handle) {
		return (ZIA_FALLBACK);
	}

	/* trust that ZFS handles closing disks once */
	ops->zo_disk_close(handle);

	return (ZIA_OK);
#else
	(void) vdev;
	return (ZIA_FALLBACK);
#endif
}
#endif
#endif
