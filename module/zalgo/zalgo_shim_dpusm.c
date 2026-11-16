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

static const dpusm_uf_t *dpusm;
static void *dpusm_provider;

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
	int error = zalgo_dpusm_fletcher4(data, datalen, checksum, byteorder);
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

int
zalgo_shim_dpusm_register(void)
{
	return (0);
}

int
zalgo_shim_dpusm_bind(const char *provider_name)
{
	dpusm_pc_t *caps = NULL;
	int error = ENOTSUP;

	if (provider_name == NULL || dpusm_provider != NULL)
		return (EINVAL);
	if (dpusm_initialize == NULL)
		return (ENOTSUP);

	if (dpusm == NULL)
		dpusm = dpusm_initialize();
	if (dpusm == NULL)
		return (ENODEV);
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
		error = zalgo_checksum_register_flags(ZG_CHECKSUM_FLETCHER4,
		    "dpusm", "DPUSM Fletcher4", &zalgo_dpusm_fletcher4_ops,
		    ZALGO_PROVIDER_ACCELERATED);
		if (error != 0)
			goto fail;
	}
	if ((caps->checksum & DPUSM_CHECKSUM_FLETCHER_4) != 0 &&
	    (caps->checksum_byteorder & DPUSM_BYTEORDER_BYTESWAP) != 0) {
		error = zalgo_checksum_register_flags(ZG_CHECKSUM_FLETCHER4_SWAP,
		    "dpusm", "DPUSM Fletcher4 byteswap",
		    &zalgo_dpusm_fletcher4_swap_ops,
		    ZALGO_PROVIDER_ACCELERATED);
		if (error != 0)
			goto fail;
	}

	return (0);

fail:
	(void) dpusm->put(dpusm_provider);
	dpusm_provider = NULL;
	return (error == 0 ? ENOTSUP : error);
}

void
zalgo_shim_dpusm_fini(void)
{
	if (dpusm != NULL && dpusm_provider != NULL) {
		(void) dpusm->put(dpusm_provider);
		dpusm_provider = NULL;
	}
	if (dpusm != NULL && dpusm_finalize != NULL) {
		(void) dpusm_finalize();
		dpusm = NULL;
	}
}
