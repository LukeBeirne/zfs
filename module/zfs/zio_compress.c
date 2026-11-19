// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright (c) 2013 by Saso Kiselkov. All rights reserved.
 * Copyright (c) 2013, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2019, 2024, Klara, Inc.
 * Copyright (c) 2019, Allan Jude
 * Copyright (c) 2021, 2024 by George Melikov. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/zfeature.h>
#include <sys/zio.h>
#include <sys/zio_compress.h>
#include <sys/zstd/zstd.h>
#include <sys/zalgo.h>

/*
 * Compression vectors.
 */
zio_compress_info_t zio_compress_table[ZIO_COMPRESS_FUNCTIONS] = {
	{"inherit",	0,	NULL,	NULL, NULL},
	{"on",		0,	NULL,	NULL, NULL},
	{"uncompressed", 0,	NULL,	NULL, NULL},
	{"lzjb",	0,
	    zfs_lzjb_compress,	zfs_lzjb_decompress, NULL},
	{"empty",	0,	NULL,	NULL, NULL},
	{"gzip-1",	1,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-2",	2,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-3",	3,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-4",	4,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-5",	5,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-6",	6,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-7",	7,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-8",	8,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"gzip-9",	9,
	    zfs_gzip_compress,	zfs_gzip_decompress, NULL},
	{"zle",		64,
	    zfs_zle_compress,	zfs_zle_decompress, NULL},
	{"lz4",		0,
	    zfs_lz4_compress,	zfs_lz4_decompress, NULL},
	{"zstd",	ZIO_ZSTD_LEVEL_DEFAULT,
	    zfs_zstd_compress,	zfs_zstd_decompress, zfs_zstd_decompress_level},
};

uint8_t
zio_complevel_select(spa_t *spa, enum zio_compress compress, uint8_t child,
    uint8_t parent)
{
	(void) spa;
	uint8_t result;

	if (!ZIO_COMPRESS_HASLEVEL(compress))
		return (0);

	result = child;
	if (result == ZIO_COMPLEVEL_INHERIT)
		result = parent;

	return (result);
}

static zalgo_compress_subtype_t
zio_compress_to_zalgo(enum zio_compress c)
{
	if (c >= ZIO_COMPRESS_GZIP_1 && c <= ZIO_COMPRESS_GZIP_9)
		return (ZG_COMPRESS_GZIP);
	if (c == ZIO_COMPRESS_LZ4)
		return (ZG_COMPRESS_LZ4);
	return (ZG_COMPRESS_ZSTD);
}

enum zio_compress
zio_compress_select(spa_t *spa, enum zio_compress child,
    enum zio_compress parent)
{
	enum zio_compress result;

	ASSERT(child < ZIO_COMPRESS_FUNCTIONS);
	ASSERT(parent < ZIO_COMPRESS_FUNCTIONS);
	ASSERT(parent != ZIO_COMPRESS_INHERIT);

	result = child;
	if (result == ZIO_COMPRESS_INHERIT)
		result = parent;

	if (result == ZIO_COMPRESS_ON) {
		if (spa_feature_is_active(spa, SPA_FEATURE_LZ4_COMPRESS))
			result = ZIO_COMPRESS_LZ4_ON_VALUE;
		else
			result = ZIO_COMPRESS_LEGACY_ON_VALUE;
	}

	return (result);
}

size_t
zio_compress_data(enum zio_compress c, abd_t *src, abd_t **dst, size_t s_len,
    size_t d_len, uint8_t level)
{
	size_t c_len;
	uint8_t complevel;
	zio_compress_info_t *ci = &zio_compress_table[c];

	ASSERT3P(ci->ci_compress, !=, NULL);
	ASSERT3U(s_len, >, 0);

	complevel = ci->ci_level;

	if (c == ZIO_COMPRESS_ZSTD) {
		/* If we don't know the level, we can't compress it */
		if (level == ZIO_COMPLEVEL_INHERIT)
			return (s_len);

		if (level == ZIO_COMPLEVEL_DEFAULT)
			complevel = ZIO_ZSTD_LEVEL_DEFAULT;
		else
			complevel = level;

		ASSERT3U(complevel, !=, ZIO_COMPLEVEL_INHERIT);
	}

	if (*dst == NULL)
		*dst = abd_alloc_sametype(src, s_len);

	c_len = ci->ci_compress(src, *dst, s_len, d_len, complevel);

	if (c_len > d_len)
		return (s_len);

	return (c_len);
}

size_t
zio_compress_data_spa(spa_t *spa, enum zio_compress c, abd_t *src,
    abd_t **dst, size_t s_len, size_t d_len, uint8_t level)
{
	size_t c_len = s_len;
	int complevel = zio_compress_table[c].ci_level;
	zalgo_compress_result_t result;

	if (c == ZIO_COMPRESS_ZSTD) {
		if (level == ZIO_COMPLEVEL_INHERIT)
			return (s_len);
		complevel = level == ZIO_COMPLEVEL_DEFAULT ?
		    ZIO_ZSTD_LEVEL_DEFAULT : level;
	}
	if (*dst == NULL)
		*dst = abd_alloc_sametype(src, s_len);
	if (zalgo_compress_data_fallback_spa(spa, zio_compress_to_zalgo(c), src,
	    *dst, s_len, d_len, complevel, &c_len, &result) == 0) {
		if (result == ZALGO_COMPRESS_OK && c_len <= d_len)
			return (c_len);
		if (result == ZALGO_COMPRESS_INCOMPRESSIBLE)
			return (s_len);
	}
	return (zio_compress_data(c, src, dst, s_len, d_len, level));
}

int
zio_decompress_data(enum zio_compress c, abd_t *src, abd_t *dst,
    size_t s_len, size_t d_len, uint8_t *level)
{
	zio_compress_info_t *ci = &zio_compress_table[c];
	if ((uint_t)c >= ZIO_COMPRESS_FUNCTIONS || ci->ci_decompress == NULL)
		return (SET_ERROR(EINVAL));

	int err;
	if (ci->ci_decompress_level != NULL && level != NULL)
		err = ci->ci_decompress_level(src, dst, s_len, d_len, level);
	else
		err = ci->ci_decompress(src, dst, s_len, d_len, ci->ci_level);

	return (err);
}

int
zio_decompress_data_spa(spa_t *spa, enum zio_compress c, abd_t *src,
    abd_t *dst, size_t s_len, size_t d_len, uint8_t *level)
{
	int dpusm_level = zio_compress_table[c].ci_level;
	if (c == ZIO_COMPRESS_ZSTD && level != NULL)
		dpusm_level = *level;
	if (zalgo_decompress_data_fallback_spa(spa, zio_compress_to_zalgo(c),
	    src, dst, s_len, d_len, &dpusm_level) == 0)
		return (0);
	return (zio_decompress_data(c, src, dst, s_len, d_len, level));
}

int
zio_compress_to_feature(enum zio_compress comp)
{
	switch (comp) {
	case ZIO_COMPRESS_ZSTD:
		return (SPA_FEATURE_ZSTD_COMPRESS);
	default:
		break;
	}
	return (SPA_FEATURE_NONE);
}
