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
	uint_t zdc_failures;
	zalgo_dpusm_failure_class_t zdc_last_failure;
	kcondvar_t zdc_cv;
	list_node_t zdc_link;
} zalgo_dpusm_context_t;

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
EXPORT_SYMBOL(zalgo_shim_dpusm_provider_name);
EXPORT_SYMBOL(zalgo_shim_dpusm_context_destroy);
