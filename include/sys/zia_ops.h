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

#ifndef _SYS_ZIA_OPS_H
#define	_SYS_ZIA_OPS_H

#include <sys/zia.h>

/*
 * Accelerator operations used by Z.I.A.
 *
 * Z.I.A. has no build-time dependency on an accelerator framework.
 * Instead, a separately built module attaches an implementation of
 * this table with zia_ops_register() and translates each operation
 * to its framework.  For example, module/zia-dpusm translates these
 * operations to the DPUSM user API.  Until a table is attached,
 * Z.I.A. does nothing and ZFS runs as if offloading were disabled.
 *
 * Unless noted otherwise, functions return ZIA_* codes.  Functions
 * noted as returning errno values return 0 or a positive errno.
 */

#define	ZIA_OPS_VERSION	1

/* capability bit for an enum zio_compress or enum zio_checksum value */
#define	ZIA_CAP_BIT(x)	(1ULL << (x))

/* optional data movement functions (zc_optional) */
#define	ZIA_OPTIONAL_COPY_FROM_PTR		(1ULL << 0)
#define	ZIA_OPTIONAL_COPY_TO_PTR		(1ULL << 1)
#define	ZIA_OPTIONAL_COPY_FROM_SCATTERLIST	(1ULL << 2)
#define	ZIA_OPTIONAL_COPY_TO_SCATTERLIST	(1ULL << 3)

/* I/O offloads (zc_io) */
#define	ZIA_IO_FILE	(1ULL << 0)
#define	ZIA_IO_DISK	(1ULL << 1)

typedef struct zia_caps {
	uint64_t zc_optional;		/* ZIA_OPTIONAL_* */
	uint64_t zc_compress;		/* ZIA_CAP_BIT(enum zio_compress) */
	uint64_t zc_decompress;		/* ZIA_CAP_BIT(enum zio_compress) */
	uint64_t zc_checksum;		/* ZIA_CAP_BIT(enum zio_checksum) */
	uint64_t zc_checksum_byteorder;	/* ZIA_CAP_BIT(zio_byteorder_t) */
	uint64_t zc_io;			/* ZIA_IO_* */
} zia_caps_t;

/* a location in accelerator memory */
typedef struct zia_move {
	void *zm_handle;
	size_t zm_offset;
} zia_mv_t;

/* called by the accelerator when an asynchronous disk I/O completes */
typedef void (*zia_io_done_t)(void *arg, int error);

struct block_device;
struct scatterlist;

typedef struct zia_ops {
	uint32_t zo_version;	/* ZIA_OPS_VERSION */
	uint32_t zo_size;	/* sizeof (zia_ops_t) */

	/* providers */
	void *(*zo_get)(const char *name);
	const char *(*zo_get_name)(void *provider);
	int (*zo_put)(void *provider);
	int (*zo_capabilities)(void *provider, zia_caps_t *caps);

	/* handles to accelerator memory */
	void *(*zo_extract)(void *handle);
	void *(*zo_alloc)(void *provider, size_t size);
	void *(*zo_alloc_ref)(void *src, size_t offset, size_t size);
	int (*zo_free)(void *handle);
	int (*zo_associate_handle)(void *handle, void *ptr);

	/* data movement; the *_ptr and *_scatterlist functions are optional */
	int (*zo_copy_from_generic)(zia_mv_t *mv, const void *buf,
	    size_t size);
	int (*zo_copy_from_scatterlist)(zia_mv_t *mv,
	    struct scatterlist *sgl, unsigned int nents, size_t size);
	int (*zo_copy_to_generic)(zia_mv_t *mv, void *buf, size_t size);
	int (*zo_copy_to_ptr)(zia_mv_t *mv, void *buf, size_t size);
	int (*zo_copy_to_scatterlist)(zia_mv_t *mv,
	    struct scatterlist *sgl, unsigned int nents, size_t size);

	/* zo_all_zeros returns ZIA_BAD_RESULT if the data is not zero */
	int (*zo_zero_fill)(void *handle, size_t offset, size_t size);
	int (*zo_all_zeros)(void *handle, size_t offset, size_t size);

	int (*zo_compress)(enum zio_compress c, int level,
	    void *src, size_t s_len, void *dst, size_t *d_len);
	int (*zo_decompress)(enum zio_compress c, int *level,
	    void *src, size_t s_len, void *dst, size_t *d_len);
	int (*zo_checksum)(enum zio_checksum alg, zio_byteorder_t order,
	    void *data, size_t size, void *cksum, size_t cksum_size);

	/* RAIDZ */
	int (*zo_raid_can_compute)(void *provider, size_t nparity,
	    size_t ndata, size_t *col_sizes, int rec);
	void *(*zo_raid_alloc)(void *provider, size_t nparity, size_t ndata);
	int (*zo_raid_set_column)(void *raid, uint64_t c, void *col,
	    size_t size);
	int (*zo_raid_free)(void *raid);
	int (*zo_raid_gen)(void *raid);
	int (*zo_raid_cmp)(void *lhs_handle, void *rhs_handle, int *diff);
	int (*zo_raid_rec)(void *raid, int *tgts, int ntgts);

	/* file vdevs; open returns NULL on error, write returns errno */
	void *(*zo_file_open)(void *provider, const char *path,
	    int flags, int mode);
	int (*zo_file_write)(void *fp_handle, void *data, size_t size,
	    size_t trailing_zeros, loff_t offset, uint8_t ashift,
	    ssize_t *resid, int *err);
	int (*zo_file_close)(void *fp_handle);

	/* disk vdevs; open returns NULL on error, write/flush return errno */
	void *(*zo_disk_open)(void *provider, const char *path,
	    struct block_device *bdev);
	int (*zo_disk_invalidate)(void *disk_handle);
	int (*zo_disk_write)(void *disk_handle, void *data, size_t data_size,
	    size_t trailing_zeros, uint64_t io_offset, int flags,
	    zia_io_done_t write_done, void *wd_arg);
	int (*zo_disk_flush)(void *disk_handle, zia_io_done_t flush_done,
	    void *fd_arg);
	int (*zo_disk_close)(void *disk_handle);
} zia_ops_t;

#if defined(_KERNEL) && defined(__linux__)
struct module;

/*
 * Attach or detach an operations table.  Only one table can be
 * attached at a time.  While any pool holds a provider, Z.I.A. holds
 * a reference on the owner module, so the table cannot be detached.
 * Both functions return 0 or a positive errno.
 */
int zia_ops_register(const zia_ops_t *ops, struct module *owner);
int zia_ops_unregister(const zia_ops_t *ops);
#endif

#endif /* _SYS_ZIA_OPS_H */
