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
#include <dpusm/user_api.h>
#ifdef _KERNEL
#include <linux/module.h>
#endif

static const dpusm_uf_t *dpusm;
static void *dpusm_provider;
static zalgo_registration_t *dpusm_native_registration;
static zalgo_registration_t *dpusm_swap_registration;
#ifdef _KERNEL
/* DPUSM is an explicit kernel-module dependency in this build. */
extern const dpusm_uf_t *dpusm_initialize_strong(void)
    __asm__("dpusm_initialize");
extern int dpusm_finalize_strong(void) __asm__("dpusm_finalize");
#endif

static int
zalgo_dpusm_fletcher4(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum, dpusm_checksum_byteorder_t byteorder)
{
	void *handle;
	dpusm_mv_t move;
	int error = ENOTSUP;

	if (dpusm == NULL || dpusm_provider == NULL ||
	    dpusm->alloc == NULL || dpusm->free == NULL ||
	    dpusm->copy.from.generic == NULL || dpusm->checksum == NULL)
		return (ENOTSUP);

	handle = dpusm->alloc(dpusm_provider, datalen);
	if (handle == NULL)
		return (ENOMEM);

	move.handle = handle;
	move.offset = 0;
	error = dpusm->copy.from.generic(&move, data, datalen);
	if (error == DPUSM_OK)
		error = dpusm->checksum(DPUSM_CHECKSUM_FLETCHER_4,
		    byteorder, handle, datalen, checksum, sizeof (*checksum));
	(void) dpusm->free(handle);
	return (error == DPUSM_OK ? 0 : EIO);
}

static int
zalgo_dpusm_fletcher4_native(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4(data, datalen, checksum,
	    DPUSM_BYTEORDER_NATIVE));
}

static int
zalgo_dpusm_fletcher4_byteswap(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (zalgo_dpusm_fletcher4(data, datalen, checksum,
	    DPUSM_BYTEORDER_BYTESWAP));
}

static int
zalgo_dpusm_fletcher4_once_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum, dpusm_checksum_byteorder_t byteorder)
{
	void *data = abd_borrow_buf_copy(abd, datalen);
	int error;

	if (data == NULL)
		return (ENOMEM);
	error = zalgo_dpusm_fletcher4(data, datalen, checksum, byteorder);
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

static const zalgo_checksum_ops_t zalgo_dpusm_fletcher4_ops = {
	.zgc_op_once = zalgo_dpusm_fletcher4_native,
	.zgc_op_once_abd = zalgo_dpusm_fletcher4_native_abd,
};

static const zalgo_checksum_ops_t zalgo_dpusm_fletcher4_swap_ops = {
	.zgc_op_once = zalgo_dpusm_fletcher4_byteswap,
	.zgc_op_once_abd = zalgo_dpusm_fletcher4_byteswap_abd,
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

const char *
zalgo_shim_dpusm_provider_name(void)
{
	if (dpusm == NULL || dpusm_provider == NULL || dpusm->get_name == NULL)
		return (NULL);
	return (dpusm->get_name(dpusm_provider));
}

int
zalgo_shim_dpusm_register(void)
{
	return (0);
}

int
zalgo_shim_dpusm_bind(const char *provider_name)
{
	return (zalgo_shim_dpusm_bind_owner(provider_name, NULL));
}

int
zalgo_shim_dpusm_bind_owner(const char *provider_name, void *owner)
{
	dpusm_pc_t *caps = NULL;
	int error = ENOTSUP;

	if (zalgo_shim_dpusm_is_disabled(provider_name) || dpusm_provider != NULL)
		return (EINVAL);
	if (zalgo_shim_dpusm_init() != 0)
		return (dpusm == NULL ? ENODEV : ENOTSUP);
	if (dpusm->get == NULL || dpusm->capabilities == NULL ||
	    dpusm->put == NULL)
		return (ENOTSUP);

	dpusm_provider = dpusm->get(provider_name);
	if (dpusm_provider == NULL)
		return (ENOENT);
	if (dpusm->capabilities(dpusm_provider, &caps) != DPUSM_OK ||
	    caps == NULL)
		goto fail;

	if ((caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & DPUSM_BYTEORDER_NATIVE) != 0) {
		error = zalgo_checksum_register_owner(ZG_CHECKSUM_FLETCHER4,
		    "dpusm", "DPUSM Fletcher4", &zalgo_dpusm_fletcher4_ops,
		    ZALGO_PROVIDER_ACCELERATED, owner,
		    &dpusm_native_registration);
		if (error != 0)
			goto fail;
		error = zalgo_checksum_select(ZG_CHECKSUM_FLETCHER4, "dpusm");
		if (error != 0)
			goto fail;
	}
	if ((caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & DPUSM_BYTEORDER_BYTESWAP) != 0) {
		error = zalgo_checksum_register_owner(ZG_CHECKSUM_FLETCHER4_SWAP,
		    "dpusm", "DPUSM Fletcher4 byteswap",
		    &zalgo_dpusm_fletcher4_swap_ops,
		    ZALGO_PROVIDER_ACCELERATED, owner,
		    &dpusm_swap_registration);
		if (error != 0)
			goto fail;
		error = zalgo_checksum_select(ZG_CHECKSUM_FLETCHER4_SWAP, "dpusm");
		if (error != 0)
			goto fail;
	}
	if (dpusm_native_registration == NULL && dpusm_swap_registration == NULL) {
		error = ENOTSUP;
		goto fail;
	}

	return (0);

fail:
	if (dpusm_swap_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_swap_registration);
		dpusm_swap_registration = NULL;
	}
	if (dpusm_native_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_native_registration);
		dpusm_native_registration = NULL;
	}
	(void) dpusm->put(dpusm_provider);
	dpusm_provider = NULL;
	return (error == 0 ? ENOTSUP : error);
}

int
zalgo_shim_dpusm_provider_set(const char *provider_name)
{
	const char *active_name;
	char *old_name = NULL;
	int error;

	if (zalgo_shim_dpusm_is_disabled(provider_name)) {
		zalgo_shim_dpusm_unbind();
		return (0);
	}

	active_name = zalgo_shim_dpusm_provider_name();
	if (active_name != NULL && strcmp(active_name, provider_name) == 0)
		return (0);

	error = zalgo_shim_dpusm_provider_validate(provider_name);
	if (error != 0)
		return (error);

	/* The prototype has one global Zalgo DPUSM binding. */
	if (active_name != NULL)
		old_name = kmem_strdup(active_name);
	if (active_name != NULL)
		zalgo_shim_dpusm_unbind();
	error = zalgo_shim_dpusm_bind(provider_name);
	if (error != 0 && old_name != NULL)
		(void) zalgo_shim_dpusm_bind(old_name);
	if (old_name != NULL)
		kmem_free(old_name, strlen(old_name) + 1);
	return (error);
}

void
zalgo_shim_dpusm_unbind(void)
{
	if (dpusm_swap_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_swap_registration);
		dpusm_swap_registration = NULL;
	}
	if (dpusm_native_registration != NULL) {
		(void) zalgo_checksum_unregister(dpusm_native_registration);
		dpusm_native_registration = NULL;
	}
	if (dpusm != NULL && dpusm_provider != NULL) {
		(void) dpusm->put(dpusm_provider);
		dpusm_provider = NULL;
	}
}

void
zalgo_shim_dpusm_fini(void)
{
	zalgo_shim_dpusm_unbind();
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

EXPORT_SYMBOL(zalgo_shim_dpusm_bind);
EXPORT_SYMBOL(zalgo_shim_dpusm_bind_owner);
EXPORT_SYMBOL(zalgo_shim_dpusm_provider_validate);
EXPORT_SYMBOL(zalgo_shim_dpusm_provider_set);
EXPORT_SYMBOL(zalgo_shim_dpusm_unbind);
