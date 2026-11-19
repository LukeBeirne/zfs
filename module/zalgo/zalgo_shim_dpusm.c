// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026, TrueNAS.
 */

#include <sys/zalgo.h>
#include <sys/list.h>
#include <dpusm/user_api.h>
#ifdef _KERNEL
#include <linux/module.h>
#endif

static const dpusm_uf_t *dpusm;
static zalgo_registration_t *dpusm_native_registration;
static zalgo_registration_t *dpusm_swap_registration;
static zalgo_registration_t *dpusm_gzip_registration;
static zalgo_registration_t *dpusm_lz4_registration;

#define ZALGO_DPUSM_FAILURE_QUARANTINE 3

typedef enum zalgo_dpusm_failure_class {
	ZALGO_DPUSM_FAILURE_NONE,
	ZALGO_DPUSM_FAILURE_UNAVAILABLE,
	ZALGO_DPUSM_FAILURE_UNSUPPORTED,
	ZALGO_DPUSM_FAILURE_INVALIDATED,
	ZALGO_DPUSM_FAILURE_PROVIDER,
} zalgo_dpusm_failure_class_t;

typedef struct zalgo_dpusm_context {
	spa_t *zdc_spa;
	void *zdc_provider;
	char *zdc_name;
	uint_t zdc_refs;
	boolean_t zdc_draining;
	boolean_t zdc_checksum;
	boolean_t zdc_native;
	boolean_t zdc_swap;
	boolean_t zdc_compress;
	boolean_t zdc_decompress;
	int zdc_compress_caps;
	int zdc_decompress_caps;
	uint_t zdc_failures;
	uint_t zdc_compress_failures;
	uint_t zdc_decompress_failures;
	zalgo_dpusm_failure_class_t zdc_last_failure;
	kcondvar_t zdc_cv;
	list_node_t zdc_link;
} zalgo_dpusm_context_t;

static dpusm_compress_t
zalgo_dpusm_compress_alg(zalgo_compress_subtype_t subtype, int level)
{
	if (subtype == ZG_COMPRESS_LZ4)
		return (DPUSM_COMPRESS_LZ4);
	if (subtype == ZG_COMPRESS_GZIP && level >= 1 && level <= 9)
		return ((dpusm_compress_t)1ULL << (level - 1));
	if (subtype == ZG_COMPRESS_ZSTD && level >= 1 && level <= 21)
		return ((dpusm_compress_t)1ULL << (level + 9));
	return (0);
}

static boolean_t
zalgo_dpusm_compress_supported(zalgo_dpusm_context_t *ctx,
    zalgo_compress_subtype_t subtype, int level, boolean_t decompress)
{
	dpusm_compress_t alg = zalgo_dpusm_compress_alg(subtype, level);
	int caps = decompress ? ctx->zdc_decompress_caps : ctx->zdc_compress_caps;
	return (alg != 0 && (caps & alg) != 0);
}

static zalgo_dpusm_context_t *
zalgo_shim_dpusm_context_hold(spa_t *spa);
static void
zalgo_shim_dpusm_context_rele(zalgo_dpusm_context_t *ctx);

static kmutex_t dpusm_context_lock;
static list_t dpusm_contexts;
static boolean_t dpusm_contexts_initialized;

#ifdef _KERNEL
/* DPUSM is an explicit kernel-module dependency in this build. */
extern const dpusm_uf_t *dpusm_initialize_strong(void)
    __asm__("dpusm_initialize");
extern int dpusm_finalize_strong(void) __asm__("dpusm_finalize");
#endif

static zalgo_dpusm_failure_class_t
zalgo_dpusm_classify_result(int result);

static int
zalgo_dpusm_fletcher4(void *provider, const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum, dpusm_checksum_byteorder_t byteorder,
    zalgo_dpusm_failure_class_t *failure)
{
	void *handle;
	dpusm_mv_t move;
	int error = ENOTSUP;

	if (failure != NULL)
		*failure = ZALGO_DPUSM_FAILURE_NONE;
	if (dpusm == NULL || provider == NULL ||
	    dpusm->alloc == NULL || dpusm->free == NULL ||
	    dpusm->copy.from.generic == NULL || dpusm->checksum == NULL) {
		if (failure != NULL)
			*failure = ZALGO_DPUSM_FAILURE_UNAVAILABLE;
		return (ENOTSUP);
	}

	handle = dpusm->alloc(provider, datalen);
	if (handle == NULL) {
		if (failure != NULL)
			*failure = ZALGO_DPUSM_FAILURE_PROVIDER;
		return (ENOMEM);
	}

	move.handle = handle;
	move.offset = 0;
	error = dpusm->copy.from.generic(&move, data, datalen);
	if (error == DPUSM_OK)
		error = dpusm->checksum(DPUSM_CHECKSUM_FLETCHER_4,
		    byteorder, handle, datalen, checksum, sizeof (*checksum));
	(void) dpusm->free(handle);
	if (error != DPUSM_OK && failure != NULL)
		*failure = zalgo_dpusm_classify_result(error);
	return (error == DPUSM_OK ? 0 : EIO);
}

static zalgo_dpusm_failure_class_t
zalgo_dpusm_classify_result(int result)
{
	switch (result) {
	case DPUSM_PROVIDER_NOT_EXISTS:
	case DPUSM_PROVIDER_UNREGISTERED:
		return (ZALGO_DPUSM_FAILURE_UNAVAILABLE);
	case DPUSM_PROVIDER_INVALIDATED:
		return (ZALGO_DPUSM_FAILURE_INVALIDATED);
	case DPUSM_NOT_IMPLEMENTED:
	case DPUSM_NOT_SUPPORTED:
		return (ZALGO_DPUSM_FAILURE_UNSUPPORTED);
	case DPUSM_ERROR:
	case DPUSM_PROVIDER_MISMATCH:
	case DPUSM_BAD_RESULT:
	default:
		return (ZALGO_DPUSM_FAILURE_PROVIDER);
	}
}

static int
zalgo_dpusm_fletcher4_native(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4(NULL, data, datalen, checksum,
	    DPUSM_BYTEORDER_NATIVE, NULL));
}

static int
zalgo_dpusm_fletcher4_byteswap(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4(NULL, data, datalen, checksum,
	    DPUSM_BYTEORDER_BYTESWAP, NULL));
}

static int
zalgo_dpusm_compress_common(spa_t *spa, zalgo_compress_subtype_t subtype,
    abd_t *src, abd_t *dst, size_t s_len, size_t d_len, int level,
    size_t *c_len,
    zalgo_compress_result_t *result)
{
	zalgo_dpusm_context_t *ctx = zalgo_shim_dpusm_context_hold(spa);
	void *src_handle = NULL, *dst_handle = NULL;
	void *s_buf = NULL, *d_buf = NULL;
	dpusm_mv_t move;
	dpusm_compress_t alg;
	int error = EIO;
	boolean_t attempted = B_FALSE;
	boolean_t d_copyback = B_FALSE;

	if (result != NULL)
		*result = ZALGO_COMPRESS_PROVIDER_FAILURE;
	if (ctx == NULL || !ctx->zdc_compress ||
	    !zalgo_dpusm_compress_supported(ctx, subtype, level, B_FALSE))
		goto out;
	attempted = B_TRUE;
	alg = zalgo_dpusm_compress_alg(subtype, level);
	if (dpusm == NULL || dpusm->alloc == NULL || dpusm->free == NULL ||
	    dpusm->copy.from.generic == NULL || dpusm->copy.to.generic == NULL ||
	    dpusm->compress == NULL)
		goto out;
	s_buf = abd_borrow_buf_copy(src, s_len);
	d_buf = abd_borrow_buf(dst, d_len);
	if (s_buf == NULL || d_buf == NULL)
		goto out;
	src_handle = dpusm->alloc(ctx->zdc_provider, s_len);
	dst_handle = dpusm->alloc(ctx->zdc_provider, d_len);
	if (src_handle == NULL || dst_handle == NULL)
		goto out;
	move.handle = src_handle;
	move.offset = 0;
	error = dpusm->copy.from.generic(&move, s_buf, s_len);
	if (error == DPUSM_OK) {
		move.handle = dst_handle;
		move.offset = 0;
		size_t out_len = d_len;
		error = dpusm->compress(alg, level, src_handle, s_len,
		    dst_handle, &out_len);
		if (error == DPUSM_OK && out_len <= d_len && out_len < s_len) {
			error = dpusm->copy.to.generic(&move, d_buf, out_len);
			if (error == DPUSM_OK) {
				*c_len = out_len;
				d_copyback = B_TRUE;
				if (result != NULL)
					*result = ZALGO_COMPRESS_OK;
			}
		} else if (error == DPUSM_OK && out_len >= s_len) {
			if (result != NULL)
				*result = ZALGO_COMPRESS_INCOMPRESSIBLE;
		} else {
			error = DPUSM_BAD_RESULT;
		}
	}
out:
	if (src_handle != NULL)
		(void) dpusm->free(src_handle);
	if (dst_handle != NULL)
		(void) dpusm->free(dst_handle);
	if (s_buf != NULL)
		abd_return_buf_copy(src, s_buf, s_len);
	if (d_buf != NULL) {
		if (d_copyback) {
			abd_return_buf_copy(dst, d_buf, d_len);
		} else {
			abd_return_buf(dst, d_buf, d_len);
		}
	}
	if (ctx != NULL) {
		if (error != 0 && attempted) {
			mutex_enter(&dpusm_context_lock);
			ctx->zdc_compress_failures++;
			if (ctx->zdc_compress_failures >=
			    ZALGO_DPUSM_FAILURE_QUARANTINE)
				ctx->zdc_compress = B_FALSE;
			mutex_exit(&dpusm_context_lock);
		}
		zalgo_shim_dpusm_context_rele(ctx);
	}
	return (error == 0 ? 0 : EIO);
}

static int
zalgo_dpusm_compress_gzip_spa(spa_t *spa, abd_t *src, abd_t *dst,
    size_t s_len, size_t d_len, int level, size_t *c_len,
    zalgo_compress_result_t *result)
{
	return (zalgo_dpusm_compress_common(spa, ZG_COMPRESS_GZIP, src, dst,
	    s_len, d_len, level, c_len, result));
}

static int
zalgo_dpusm_compress_lz4_spa(spa_t *spa, abd_t *src, abd_t *dst,
    size_t s_len, size_t d_len, int level, size_t *c_len,
    zalgo_compress_result_t *result)
{
	return (zalgo_dpusm_compress_common(spa, ZG_COMPRESS_LZ4, src, dst,
	    s_len, d_len, level, c_len, result));
}

static int
zalgo_dpusm_decompress_common(spa_t *spa, zalgo_compress_subtype_t subtype,
    abd_t *src, abd_t *dst, size_t s_len, size_t d_len, int *level)
{
	zalgo_dpusm_context_t *ctx = zalgo_shim_dpusm_context_hold(spa);
	void *src_handle = NULL, *dst_handle = NULL;
	void *s_buf = NULL, *d_buf = NULL;
	dpusm_mv_t move;
	dpusm_compress_t alg;
	int actual_level;
	size_t out_len = d_len;
	int error = EIO;
	boolean_t attempted = B_FALSE;
	boolean_t d_copyback = B_FALSE;

	if (ctx == NULL || !ctx->zdc_decompress || level == NULL ||
	    !zalgo_dpusm_compress_supported(ctx, subtype, *level, B_TRUE))
		goto out;
	attempted = B_TRUE;
	actual_level = *level;
	alg = zalgo_dpusm_compress_alg(subtype, actual_level);
	if (dpusm == NULL || dpusm->alloc == NULL || dpusm->free == NULL ||
	    dpusm->copy.from.generic == NULL || dpusm->copy.to.generic == NULL ||
	    dpusm->decompress == NULL)
		goto out;
	s_buf = abd_borrow_buf_copy(src, s_len);
	d_buf = abd_borrow_buf(dst, d_len);
	if (s_buf == NULL || d_buf == NULL)
		goto out;
	src_handle = dpusm->alloc(ctx->zdc_provider, s_len);
	dst_handle = dpusm->alloc(ctx->zdc_provider, d_len);
	if (src_handle == NULL || dst_handle == NULL)
		goto out;
	move.handle = src_handle;
	move.offset = 0;
	error = dpusm->copy.from.generic(&move, s_buf, s_len);
	if (error == DPUSM_OK) {
		move.handle = dst_handle;
		error = dpusm->decompress(alg, &actual_level, src_handle, s_len,
		    dst_handle, &out_len);
		if (error == DPUSM_OK && out_len == d_len)
			error = dpusm->copy.to.generic(&move, d_buf, d_len);
		else if (error == DPUSM_OK)
			error = DPUSM_BAD_RESULT;
		if (error == DPUSM_OK)
			d_copyback = B_TRUE;
		if (error == DPUSM_OK)
			*level = actual_level;
	}
out:
	if (src_handle != NULL)
		(void) dpusm->free(src_handle);
	if (dst_handle != NULL)
		(void) dpusm->free(dst_handle);
	if (s_buf != NULL)
		abd_return_buf_copy(src, s_buf, s_len);
	if (d_buf != NULL) {
		if (d_copyback) {
			abd_return_buf_copy(dst, d_buf, d_len);
		} else {
			abd_return_buf(dst, d_buf, d_len);
		}
	}
	if (ctx != NULL) {
		if (error != 0 && attempted) {
			mutex_enter(&dpusm_context_lock);
			ctx->zdc_decompress_failures++;
			if (ctx->zdc_decompress_failures >=
			    ZALGO_DPUSM_FAILURE_QUARANTINE)
				ctx->zdc_decompress = B_FALSE;
			mutex_exit(&dpusm_context_lock);
		}
		zalgo_shim_dpusm_context_rele(ctx);
	}
	return (error == 0 ? 0 : EIO);
}

#define ZALGO_DPUSM_DECOMPRESS_WRAPPER(name, subtype) \
static int name(spa_t *spa, abd_t *src, abd_t *dst, size_t s_len, \
    size_t d_len, int *level) \
{ return (zalgo_dpusm_decompress_common(spa, subtype, src, dst, s_len, \
    d_len, level)); }

ZALGO_DPUSM_DECOMPRESS_WRAPPER(zalgo_dpusm_decompress_gzip_spa,
    ZG_COMPRESS_GZIP)
ZALGO_DPUSM_DECOMPRESS_WRAPPER(zalgo_dpusm_decompress_lz4_spa,
    ZG_COMPRESS_LZ4)

static const zalgo_compress_ops_t zalgo_dpusm_gzip_ops = {
	.zgc_op_compress_spa = zalgo_dpusm_compress_gzip_spa,
	.zgc_op_decompress_spa = zalgo_dpusm_decompress_gzip_spa,
};
static const zalgo_compress_ops_t zalgo_dpusm_lz4_ops = {
	.zgc_op_compress_spa = zalgo_dpusm_compress_lz4_spa,
	.zgc_op_decompress_spa = zalgo_dpusm_decompress_lz4_spa,
};
static int
zalgo_dpusm_fletcher4_once_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum, dpusm_checksum_byteorder_t byteorder)
{
	void *data = abd_borrow_buf_copy(abd, datalen);
	int error;

	if (data == NULL)
		return (ENOMEM);
	error = zalgo_dpusm_fletcher4(NULL, data, datalen, checksum, byteorder,
	    NULL);
	abd_return_buf_copy(abd, data, datalen);
	return (error);
}

static int
zalgo_dpusm_fletcher4_native_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4_once_abd(abd, datalen, checksum,
	    DPUSM_BYTEORDER_NATIVE));
}

static int
zalgo_dpusm_fletcher4_byteswap_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4_once_abd(abd, datalen, checksum,
	    DPUSM_BYTEORDER_BYTESWAP));
}

static int zalgo_dpusm_fletcher4_native_spa(spa_t *, abd_t *, size_t,
    zio_cksum_t *);
static int zalgo_dpusm_fletcher4_byteswap_spa(spa_t *, abd_t *, size_t,
    zio_cksum_t *);

static const zalgo_checksum_ops_t zalgo_dpusm_fletcher4_ops = {
	.zgc_op_once = zalgo_dpusm_fletcher4_native,
	.zgc_op_once_abd = zalgo_dpusm_fletcher4_native_abd,
	.zgc_op_once_spa = zalgo_dpusm_fletcher4_native_spa,
};

static const zalgo_checksum_ops_t zalgo_dpusm_fletcher4_swap_ops = {
	.zgc_op_once = zalgo_dpusm_fletcher4_byteswap,
	.zgc_op_once_abd = zalgo_dpusm_fletcher4_byteswap_abd,
	.zgc_op_once_spa = zalgo_dpusm_fletcher4_byteswap_spa,
};

static int
zalgo_shim_dpusm_init(void)
{
	if (dpusm != NULL)
		return (0);
#ifdef _KERNEL
	dpusm = dpusm_initialize_strong();
#else
	if (dpusm_initialize == NULL)
		return (ENOTSUP);
	dpusm = dpusm_initialize();
#endif
	return (dpusm == NULL ? ENODEV : 0);
}

static zalgo_dpusm_context_t *
zalgo_shim_dpusm_context_hold(spa_t *spa)
{
	zalgo_dpusm_context_t *ctx;

	if (!dpusm_contexts_initialized)
		return (NULL);
	mutex_enter(&dpusm_context_lock);
	for (ctx = list_head(&dpusm_contexts); ctx != NULL;
	    ctx = list_next(&dpusm_contexts, ctx)) {
		if (ctx->zdc_spa == spa && !ctx->zdc_draining) {
			ctx->zdc_refs++;
			break;
		}
	}
	mutex_exit(&dpusm_context_lock);
	return (ctx);
}

static void
zalgo_shim_dpusm_context_rele(zalgo_dpusm_context_t *ctx)
{
	mutex_enter(&dpusm_context_lock);
	VERIFY3U(ctx->zdc_refs, >, 0);
	ctx->zdc_refs--;
	if (ctx->zdc_draining && ctx->zdc_refs == 0)
		cv_broadcast(&ctx->zdc_cv);
	mutex_exit(&dpusm_context_lock);
}

static int
zalgo_dpusm_fletcher4_spa(spa_t *spa, abd_t *abd, size_t datalen,
    zio_cksum_t *checksum, dpusm_checksum_byteorder_t byteorder)
{
	zalgo_dpusm_context_t *ctx = zalgo_shim_dpusm_context_hold(spa);
	void *data;
	zalgo_dpusm_failure_class_t failure;
	int error;

	if (ctx == NULL || !ctx->zdc_checksum ||
	    (byteorder == DPUSM_BYTEORDER_NATIVE && !ctx->zdc_native) ||
	    (byteorder == DPUSM_BYTEORDER_BYTESWAP && !ctx->zdc_swap)) {
		if (ctx != NULL)
			zalgo_shim_dpusm_context_rele(ctx);
		return (ENOTSUP);
	}
	data = abd_borrow_buf_copy(abd, datalen);
	if (data == NULL) {
		zalgo_shim_dpusm_context_rele(ctx);
		return (ENOMEM);
	}
	error = zalgo_dpusm_fletcher4(ctx->zdc_provider, data, datalen,
	    checksum, byteorder, &failure);
	abd_return_buf_copy(abd, data, datalen);
	if (error != 0) {
		mutex_enter(&dpusm_context_lock);
		ctx->zdc_failures++;
		ctx->zdc_last_failure = failure;
		if (ctx->zdc_failures >= ZALGO_DPUSM_FAILURE_QUARANTINE)
			ctx->zdc_checksum = B_FALSE;
		mutex_exit(&dpusm_context_lock);
	}
	zalgo_shim_dpusm_context_rele(ctx);
	return (error);
}

static int
zalgo_dpusm_fletcher4_native_spa(spa_t *spa, abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4_spa(spa, abd, datalen, checksum,
	    DPUSM_BYTEORDER_NATIVE));
}

static int
zalgo_dpusm_fletcher4_byteswap_spa(spa_t *spa, abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4_spa(spa, abd, datalen, checksum,
	    DPUSM_BYTEORDER_BYTESWAP));
}

static boolean_t
zalgo_shim_dpusm_is_disabled(const char *provider_name)
{
	return (provider_name == NULL || provider_name[0] == '\0' ||
	    strcmp(provider_name, "NULL") == 0 ||
	    strcmp(provider_name, "none") == 0 ||
	    strcmp(provider_name, "off") == 0);
}

int
zalgo_shim_dpusm_provider_validate(const char *provider_name)
{
	void *provider;
	dpusm_pc_t *caps = NULL;
	int error;

	if (zalgo_shim_dpusm_is_disabled(provider_name))
		return (0);

	error = zalgo_shim_dpusm_init();
	if (error != 0)
		return (error);
	if (dpusm->get == NULL || dpusm->put == NULL ||
	    dpusm->capabilities == NULL)
		return (ENOTSUP);

	provider = dpusm->get(provider_name);
	if (provider == NULL)
		return (ENOENT);
	error = dpusm->capabilities(provider, &caps) == DPUSM_OK &&
	    caps != NULL &&
	    (caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & (DPUSM_BYTEORDER_NATIVE |
	    DPUSM_BYTEORDER_BYTESWAP)) != 0 ? 0 : ENOTSUP;
	(void) dpusm->put(provider);
	return (error);
}

int
zalgo_shim_dpusm_provider_name(spa_t *spa, char *name, size_t size)
{
	zalgo_dpusm_context_t *ctx;
	const char *provider_name;

	if (name == NULL || size == 0)
		return (EINVAL);
	name[0] = '\0';
	ctx = zalgo_shim_dpusm_context_hold(spa);
	if (ctx == NULL || dpusm->get_name == NULL) {
		if (ctx != NULL)
			zalgo_shim_dpusm_context_rele(ctx);
		return (ENOENT);
	}
	provider_name = dpusm->get_name(ctx->zdc_provider);
	if (provider_name == NULL) {
		zalgo_shim_dpusm_context_rele(ctx);
		return (ENOENT);
	}
	(void) strlcpy(name, provider_name, size);
	zalgo_shim_dpusm_context_rele(ctx);
	return (0);
}

int
zalgo_shim_dpusm_register(void)
{
	mutex_init(&dpusm_context_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&dpusm_contexts, sizeof (zalgo_dpusm_context_t),
	    offsetof(zalgo_dpusm_context_t, zdc_link));
	dpusm_contexts_initialized = B_TRUE;
	return (0);
}

int
zalgo_shim_dpusm_provider_set(spa_t *spa, const char *provider_name)
{
	zalgo_dpusm_context_t *ctx, *old = NULL;
	dpusm_pc_t *caps = NULL;
	int error;

	if (zalgo_shim_dpusm_is_disabled(provider_name)) {
		zalgo_shim_dpusm_context_destroy(spa);
		return (0);
	}
	error = zalgo_shim_dpusm_provider_validate(provider_name);
	if (error != 0)
		return (error);
	if (!dpusm_contexts_initialized)
		return (ENODEV);
	if (dpusm->get == NULL || dpusm->capabilities == NULL ||
	    dpusm->put == NULL)
		return (ENOTSUP);

	ctx = kmem_zalloc(sizeof (*ctx), KM_SLEEP);
	ctx->zdc_spa = spa;
	ctx->zdc_provider = dpusm->get(provider_name);
	ctx->zdc_name = kmem_strdup(provider_name);
	ctx->zdc_checksum = B_TRUE;
	ctx->zdc_compress = B_TRUE;
	ctx->zdc_decompress = B_TRUE;
	ctx->zdc_refs = 0;
	ctx->zdc_draining = B_FALSE;
	cv_init(&ctx->zdc_cv, NULL, CV_DEFAULT, NULL);
	if (ctx->zdc_provider == NULL) {
		error = ENOENT;
		goto fail;
	}
	if (dpusm->capabilities(ctx->zdc_provider, &caps) != DPUSM_OK ||
	    caps == NULL) {
		error = ENOTSUP;
		goto fail;
	}
	ctx->zdc_native = (caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & DPUSM_BYTEORDER_NATIVE) != 0;
	ctx->zdc_swap = (caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & DPUSM_BYTEORDER_BYTESWAP) != 0;
	ctx->zdc_compress_caps = caps->compress;
	ctx->zdc_decompress_caps = caps->decompress;
	if (!ctx->zdc_native && !ctx->zdc_swap) {
		error = ENOTSUP;
		goto fail;
	}

	if (dpusm_native_registration == NULL && dpusm_swap_registration == NULL) {
		if (ctx->zdc_native) {
			error = zalgo_checksum_register_owner(ZG_CHECKSUM_FLETCHER4,
			    "dpusm", "DPUSM Fletcher4", &zalgo_dpusm_fletcher4_ops,
			    ZALGO_PROVIDER_ACCELERATED, NULL,
			    &dpusm_native_registration);
			if (error != 0)
				goto fail_context;
		}
		if (ctx->zdc_swap) {
			error = zalgo_checksum_register_owner(
			    ZG_CHECKSUM_FLETCHER4_SWAP, "dpusm",
			    "DPUSM Fletcher4 byteswap", &zalgo_dpusm_fletcher4_swap_ops,
			    ZALGO_PROVIDER_ACCELERATED, NULL,
			    &dpusm_swap_registration);
			if (error != 0)
				goto fail_context;
		}
		if (ctx->zdc_compress_caps & DPUSM_COMPRESS_GZIP_1) {
			error = zalgo_compress_register_owner(ZG_COMPRESS_GZIP,
			    "dpusm-gzip", "DPUSM gzip compression",
			    &zalgo_dpusm_gzip_ops, ZALGO_PROVIDER_ACCELERATED, NULL,
			    &dpusm_gzip_registration);
			if (error != 0)
				goto fail_context;
		}
		if (ctx->zdc_compress_caps & DPUSM_COMPRESS_LZ4) {
			error = zalgo_compress_register_owner(ZG_COMPRESS_LZ4,
			    "dpusm-lz4", "DPUSM LZ4 compression",
			    &zalgo_dpusm_lz4_ops, ZALGO_PROVIDER_ACCELERATED, NULL,
			    &dpusm_lz4_registration);
			if (error != 0)
				goto fail_context;
			if (dpusm_gzip_registration != NULL)
				(void) zalgo_compress_select(ZG_COMPRESS_GZIP,
				    "dpusm-gzip");
			if (dpusm_lz4_registration != NULL)
				(void) zalgo_compress_select(ZG_COMPRESS_LZ4,
				    "dpusm-lz4");
		}
		if (dpusm_native_registration != NULL)
			(void) zalgo_checksum_select(ZG_CHECKSUM_FLETCHER4, "dpusm");
		if (dpusm_swap_registration != NULL)
			(void) zalgo_checksum_select(ZG_CHECKSUM_FLETCHER4_SWAP, "dpusm");
	}

	/* Validate and register the replacement before detaching the old context. */
	mutex_enter(&dpusm_context_lock);
	for (old = list_head(&dpusm_contexts); old != NULL;
	    old = list_next(&dpusm_contexts, old)) {
		if (old->zdc_spa == spa)
			break;
	}
	if (old != NULL) {
		old->zdc_draining = B_TRUE;
		while (old->zdc_refs != 0)
			cv_wait(&old->zdc_cv, &dpusm_context_lock);
		list_remove(&dpusm_contexts, old);
	}
	list_insert_tail(&dpusm_contexts, ctx);
	mutex_exit(&dpusm_context_lock);
	if (old != NULL) {
		(void) dpusm->put(old->zdc_provider);
		cv_destroy(&old->zdc_cv);
		kmem_free(old->zdc_name, strlen(old->zdc_name) + 1);
		kmem_free(old, sizeof (*old));
	}
	return (0);

fail_context:
	if (dpusm_swap_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_swap_registration);
		dpusm_swap_registration = NULL;
	}
	if (dpusm_native_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_native_registration);
		dpusm_native_registration = NULL;
	}
	if (dpusm_gzip_registration != NULL) {
		(void) zalgo_compress_unregister(dpusm_gzip_registration);
		dpusm_gzip_registration = NULL;
	}
	if (dpusm_lz4_registration != NULL) {
		(void) zalgo_compress_unregister(dpusm_lz4_registration);
		dpusm_lz4_registration = NULL;
	}
	/* No old context has been detached yet, so its provider remains usable. */
	if (old != NULL)
		old = NULL;
fail:
	if (ctx->zdc_provider != NULL)
		(void) dpusm->put(ctx->zdc_provider);
	if (ctx->zdc_name != NULL)
		kmem_free(ctx->zdc_name, strlen(ctx->zdc_name) + 1);
	cv_destroy(&ctx->zdc_cv);
	kmem_free(ctx, sizeof (*ctx));
	return (error);
}

int
zalgo_shim_dpusm_checksum_set(spa_t *spa, boolean_t enabled)
{
	zalgo_dpusm_context_t *ctx;

	mutex_enter(&dpusm_context_lock);
	for (ctx = list_head(&dpusm_contexts); ctx != NULL;
	    ctx = list_next(&dpusm_contexts, ctx)) {
		if (ctx->zdc_spa == spa) {
			ctx->zdc_checksum = enabled;
			if (enabled) {
				ctx->zdc_failures = 0;
				ctx->zdc_last_failure = ZALGO_DPUSM_FAILURE_NONE;
			}
			break;
		}
	}
	mutex_exit(&dpusm_context_lock);
	return (0);
}

int
zalgo_shim_dpusm_compress_set(spa_t *spa, boolean_t enabled)
{
	zalgo_dpusm_context_t *ctx;
	mutex_enter(&dpusm_context_lock);
	for (ctx = list_head(&dpusm_contexts); ctx != NULL;
	    ctx = list_next(&dpusm_contexts, ctx)) {
		if (ctx->zdc_spa == spa) {
			ctx->zdc_compress = enabled;
			if (enabled)
				ctx->zdc_compress_failures = 0;
			break;
		}
	}
	mutex_exit(&dpusm_context_lock);
	return (0);
}

int
zalgo_shim_dpusm_decompress_set(spa_t *spa, boolean_t enabled)
{
	zalgo_dpusm_context_t *ctx;
	mutex_enter(&dpusm_context_lock);
	for (ctx = list_head(&dpusm_contexts); ctx != NULL;
	    ctx = list_next(&dpusm_contexts, ctx)) {
		if (ctx->zdc_spa == spa) {
			ctx->zdc_decompress = enabled;
			if (enabled)
				ctx->zdc_decompress_failures = 0;
			break;
		}
	}
	mutex_exit(&dpusm_context_lock);
	return (0);
}

void
zalgo_shim_dpusm_context_destroy(spa_t *spa)
{
	zalgo_dpusm_context_t *ctx;

	mutex_enter(&dpusm_context_lock);
	for (ctx = list_head(&dpusm_contexts); ctx != NULL;
	    ctx = list_next(&dpusm_contexts, ctx)) {
		if (ctx->zdc_spa == spa)
			break;
	}
	if (ctx == NULL) {
		mutex_exit(&dpusm_context_lock);
		return;
	}
	ctx->zdc_draining = B_TRUE;
	while (ctx->zdc_refs != 0)
		cv_wait(&ctx->zdc_cv, &dpusm_context_lock);
	list_remove(&dpusm_contexts, ctx);
	mutex_exit(&dpusm_context_lock);
	(void) dpusm->put(ctx->zdc_provider);
	cv_destroy(&ctx->zdc_cv);
	kmem_free(ctx->zdc_name, strlen(ctx->zdc_name) + 1);
	kmem_free(ctx, sizeof (*ctx));
	mutex_enter(&dpusm_context_lock);
	boolean_t empty = list_is_empty(&dpusm_contexts);
	mutex_exit(&dpusm_context_lock);
	if (empty) {
		if (dpusm_swap_registration != NULL) {
			(void) zalgo_checksum_unregister(dpusm_swap_registration);
			dpusm_swap_registration = NULL;
		}
		if (dpusm_native_registration != NULL) {
			(void) zalgo_checksum_unregister(dpusm_native_registration);
			dpusm_native_registration = NULL;
		}
		if (dpusm_gzip_registration != NULL) {
			(void) zalgo_compress_unregister(dpusm_gzip_registration);
			dpusm_gzip_registration = NULL;
		}
		if (dpusm_lz4_registration != NULL) {
			(void) zalgo_compress_unregister(dpusm_lz4_registration);
			dpusm_lz4_registration = NULL;
		}
	}
}

void
zalgo_shim_dpusm_fini(void)
{
	while (!list_is_empty(&dpusm_contexts)) {
		zalgo_dpusm_context_t *ctx =
		    (zalgo_dpusm_context_t *)list_head(&dpusm_contexts);
		zalgo_shim_dpusm_context_destroy(ctx->zdc_spa);
	}
	if (dpusm_contexts_initialized) {
		list_destroy(&dpusm_contexts);
		mutex_destroy(&dpusm_context_lock);
		dpusm_contexts_initialized = B_FALSE;
	}
	if (dpusm != NULL) {
#ifdef _KERNEL
		(void) dpusm_finalize_strong();
#else
		if (dpusm_finalize != NULL)
			(void) dpusm_finalize();
#endif
		dpusm = NULL;
	}
}

EXPORT_SYMBOL(zalgo_shim_dpusm_provider_validate);
EXPORT_SYMBOL(zalgo_shim_dpusm_provider_set);
EXPORT_SYMBOL(zalgo_shim_dpusm_checksum_set);
EXPORT_SYMBOL(zalgo_shim_dpusm_compress_set);
EXPORT_SYMBOL(zalgo_shim_dpusm_decompress_set);
EXPORT_SYMBOL(zalgo_shim_dpusm_provider_name);
EXPORT_SYMBOL(zalgo_shim_dpusm_context_destroy);
