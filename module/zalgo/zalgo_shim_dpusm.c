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

#include <sys/abd.h>
#include <sys/byteorder.h>
#include <sys/zalgo.h>

typedef struct my_fletcher_ctx {
	zio_cksum_t	mf_cksum;
	boolean_t	mf_byteswap;
} my_fletcher_ctx_t;

static void
my_fletcher_init_ctx(my_fletcher_ctx_t *ctx, boolean_t byteswap)
{
	ZIO_SET_CHECKSUM(&ctx->mf_cksum, 0, 0, 0, 0);
	ctx->mf_byteswap = byteswap;
}

static void
my_fletcher_update_ctx(my_fletcher_ctx_t *ctx, const void *buf, size_t len)
{
	const uint32_t *ip = buf;
	const uint32_t *ipend = ip + (len / sizeof (uint32_t));
	uint64_t a = ctx->mf_cksum.zc_word[0];
	uint64_t b = ctx->mf_cksum.zc_word[1];
	uint64_t c = ctx->mf_cksum.zc_word[2];
	uint64_t d = ctx->mf_cksum.zc_word[3];

	ASSERT(IS_P2ALIGNED(len, sizeof (uint32_t)));

	for (; ip < ipend; ip++) {
		uint32_t word = ip[0];

		if (ctx->mf_byteswap)
			word = BSWAP_32(word);

		a += word;
		b += a;
		c += b;
		d += c;
	}

	ZIO_SET_CHECKSUM(&ctx->mf_cksum, a, b, c, d);
}

static int
my_fletcher_abd_iter(void *buf, size_t len, void *private)
{
	my_fletcher_update_ctx(private, buf, len);
	return (0);
}

static int
my_fletcher_init(void **ctxp, boolean_t byteswap)
{
	my_fletcher_ctx_t *ctx = kmem_alloc(sizeof (*ctx), KM_SLEEP);

	my_fletcher_init_ctx(ctx, byteswap);
	*ctxp = ctx;

	return (0);
}

static int
my_fletcher_update(void **ctxp, const uint8_t *data, size_t datalen)
{
	my_fletcher_update_ctx(*ctxp, data, datalen);
	return (0);
}

static int
my_fletcher_update_abd(void **ctxp, abd_t *abd, size_t datalen)
{
	(void) abd_iterate_func(abd, 0, datalen, my_fletcher_abd_iter, *ctxp);
	return (0);
}

static int
my_fletcher_final(void **ctxp, zio_cksum_t *checksum)
{
	my_fletcher_ctx_t *ctx = *ctxp;

	*checksum = ctx->mf_cksum;
	kmem_free(ctx, sizeof (*ctx));
	*ctxp = NULL;

	return (0);
}

static int
my_fletcher_once(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum, boolean_t byteswap)
{
	my_fletcher_ctx_t ctx;

	my_fletcher_init_ctx(&ctx, byteswap);
	my_fletcher_update_ctx(&ctx, data, datalen);
	*checksum = ctx.mf_cksum;

	return (0);
}

static int
my_fletcher_once_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum, boolean_t byteswap)
{
	my_fletcher_ctx_t ctx;

	my_fletcher_init_ctx(&ctx, byteswap);
	(void) abd_iterate_func(abd, 0, datalen, my_fletcher_abd_iter, &ctx);
	*checksum = ctx.mf_cksum;

	return (0);
}

static int
my_fletcher_native_init(void **ctxp)
{
	return (my_fletcher_init(ctxp, B_FALSE));
}

static int
my_fletcher_byteswap_init(void **ctxp)
{
	return (my_fletcher_init(ctxp, B_TRUE));
}

static int
my_fletcher_native_once(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (my_fletcher_once(data, datalen, checksum, B_FALSE));
}

static int
my_fletcher_byteswap_once(const uint8_t *data, size_t datalen,
    zio_cksum_t *checksum)
{
	return (my_fletcher_once(data, datalen, checksum, B_TRUE));
}

static int
my_fletcher_native_once_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (my_fletcher_once_abd(abd, datalen, checksum, B_FALSE));
}

static int
my_fletcher_byteswap_once_abd(abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	return (my_fletcher_once_abd(abd, datalen, checksum, B_TRUE));
}

static const zalgo_checksum_ops_t my_fletcher_native_ops = {
	.zgc_op_init = my_fletcher_native_init,
	.zgc_op_update = my_fletcher_update,
	.zgc_op_update_abd = my_fletcher_update_abd,
	.zgc_op_final = my_fletcher_final,
	.zgc_op_once = my_fletcher_native_once,
	.zgc_op_once_abd = my_fletcher_native_once_abd,
};

static const zalgo_checksum_ops_t my_fletcher_byteswap_ops = {
	.zgc_op_init = my_fletcher_byteswap_init,
	.zgc_op_update = my_fletcher_update,
	.zgc_op_update_abd = my_fletcher_update_abd,
	.zgc_op_final = my_fletcher_final,
	.zgc_op_once = my_fletcher_byteswap_once,
	.zgc_op_once_abd = my_fletcher_byteswap_once_abd,
};

int
zalgo_shim_my_fletcher_register(void)
{
	int err;

	err = zalgo_checksum_register(ZG_CHECKSUM_FLETCHER4, "my_fletcher",
	    "my_fletcher native", &my_fletcher_native_ops);
	if (err != 0)
		return (err);

	return (zalgo_checksum_register(ZG_CHECKSUM_FLETCHER4_SWAP,
	    "my_fletcher", "my_fletcher byteswap", &my_fletcher_byteswap_ops));
}
