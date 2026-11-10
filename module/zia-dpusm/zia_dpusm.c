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
 * Z.I.A. to DPUSM glue
 *
 * This module attaches the Data Processing Unit Services Module (DPUSM)
 * to Z.I.A.  It implements the Z.I.A. operations table by translating
 * each operation to the DPUSM user API.  Once it is loaded, any provider
 * registered with DPUSM can be selected with
 *
 *     zpool set zia_provider=<provider module name> <pool>
 *
 * Neither ZFS nor DPUSM needs the other at build time.  Only this
 * module is built against both, and it is loaded after them:
 *
 *     zfs -> dpusm -> zia_dpusm -> providers (in any order after dpusm)
 *
 * Build it with
 *
 *     make zia-dpusm DPUSM=/path/to/dpusm
 *
 * This module cannot be unloaded while a pool uses a provider.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <sys/zia_ops.h>

#include <dpusm/user_api.h>

static const dpusm_uf_t *dpusm = NULL;

static int
dpusm_to_zia(const int dpusm_ret)
{
	switch (dpusm_ret) {
	case DPUSM_OK:
		return (ZIA_OK);
	case DPUSM_ERROR:
		return (ZIA_ERROR);
	case DPUSM_PROVIDER_MISMATCH:
		return (ZIA_PROVIDER_MISMATCH);
	case DPUSM_NOT_IMPLEMENTED:
	case DPUSM_NOT_SUPPORTED:
		return (ZIA_FALLBACK);
	case DPUSM_BAD_RESULT:
		return (ZIA_BAD_RESULT);
	case DPUSM_PROVIDER_NOT_EXISTS:
	case DPUSM_PROVIDER_INVALIDATED:
	case DPUSM_PROVIDER_UNREGISTERED:
	default:
		return (ZIA_ACCELERATOR_DOWN);
	}
}

static dpusm_compress_t
compress_to_dpusm(enum zio_compress c)
{
	switch (c) {
	case ZIO_COMPRESS_GZIP_1:
		return (DPUSM_COMPRESS_GZIP_1);
	case ZIO_COMPRESS_GZIP_2:
		return (DPUSM_COMPRESS_GZIP_2);
	case ZIO_COMPRESS_GZIP_3:
		return (DPUSM_COMPRESS_GZIP_3);
	case ZIO_COMPRESS_GZIP_4:
		return (DPUSM_COMPRESS_GZIP_4);
	case ZIO_COMPRESS_GZIP_5:
		return (DPUSM_COMPRESS_GZIP_5);
	case ZIO_COMPRESS_GZIP_6:
		return (DPUSM_COMPRESS_GZIP_6);
	case ZIO_COMPRESS_GZIP_7:
		return (DPUSM_COMPRESS_GZIP_7);
	case ZIO_COMPRESS_GZIP_8:
		return (DPUSM_COMPRESS_GZIP_8);
	case ZIO_COMPRESS_GZIP_9:
		return (DPUSM_COMPRESS_GZIP_9);
	case ZIO_COMPRESS_LZ4:
		return (DPUSM_COMPRESS_LZ4);
	default:
		return (0);
	}
}

static uint64_t
compress_caps(uint64_t dpusm_mask)
{
	uint64_t caps = 0;
	for (enum zio_compress c = 0; c < ZIO_COMPRESS_FUNCTIONS; c++) {
		const dpusm_compress_t d = compress_to_dpusm(c);
		if (d != 0 && (dpusm_mask & d))
			caps |= ZIA_CAP_BIT(c);
	}
	return (caps);
}

static dpusm_checksum_t
checksum_to_dpusm(enum zio_checksum c)
{
	switch (c) {
	case ZIO_CHECKSUM_FLETCHER_2:
		return (DPUSM_CHECKSUM_FLETCHER_2);
	case ZIO_CHECKSUM_FLETCHER_4:
		return (DPUSM_CHECKSUM_FLETCHER_4);
	case ZIO_CHECKSUM_SHA256:
		return (DPUSM_CHECKSUM_SHA256);
	default:
		return (0);
	}
}

static dpusm_checksum_byteorder_t
byteorder_to_dpusm(zio_byteorder_t bo)
{
	switch (bo) {
	case ZIO_CHECKSUM_NATIVE:
		return (DPUSM_BYTEORDER_NATIVE);
	case ZIO_CHECKSUM_BYTESWAP:
		return (DPUSM_BYTEORDER_BYTESWAP);
	default:
		return (DPUSM_BYTEORDER_MAX);
	}
}

/*
 * zia_mv_t and dpusm_mv_t describe the same thing; translate instead of
 * casting so that their layouts are free to differ.
 */
#define	ZIA_TO_DPUSM_MV(zmv)	{			\
	.handle = (zmv)->zm_handle,			\
	.offset = (zmv)->zm_offset,			\
}

static void *
zd_get(const char *name)
{
	return (dpusm->get(name));
}

static const char *
zd_get_name(void *provider)
{
	return (dpusm->get_name(provider));
}

static int
zd_put(void *provider)
{
	return (dpusm_to_zia(dpusm->put(provider)));
}

static int
zd_capabilities(void *provider, zia_caps_t *caps)
{
	dpusm_pc_t *pc = NULL;
	const int ret = dpusm->capabilities(provider, &pc);
	if (ret != DPUSM_OK)
		return (dpusm_to_zia(ret));
	if (pc == NULL)
		return (ZIA_ERROR);

	if (pc->optional & DPUSM_OPTIONAL_COPY_FROM_PTR)
		caps->zc_optional |= ZIA_OPTIONAL_COPY_FROM_PTR;
	if (pc->optional & DPUSM_OPTIONAL_COPY_TO_PTR)
		caps->zc_optional |= ZIA_OPTIONAL_COPY_TO_PTR;
	if (pc->optional & DPUSM_OPTIONAL_COPY_FROM_SCATTERLIST)
		caps->zc_optional |= ZIA_OPTIONAL_COPY_FROM_SCATTERLIST;
	if (pc->optional & DPUSM_OPTIONAL_COPY_TO_SCATTERLIST)
		caps->zc_optional |= ZIA_OPTIONAL_COPY_TO_SCATTERLIST;

	caps->zc_compress = compress_caps((unsigned int)pc->compress);
	caps->zc_decompress = compress_caps((unsigned int)pc->decompress);

	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		const dpusm_checksum_t d = checksum_to_dpusm(c);
		if (d != 0 && (pc->checksum & d))
			caps->zc_checksum |= ZIA_CAP_BIT(c);
	}

	if (pc->checksum_byteorder & DPUSM_BYTEORDER_NATIVE)
		caps->zc_checksum_byteorder |=
		    ZIA_CAP_BIT(ZIO_CHECKSUM_NATIVE);
	if (pc->checksum_byteorder & DPUSM_BYTEORDER_BYTESWAP)
		caps->zc_checksum_byteorder |=
		    ZIA_CAP_BIT(ZIO_CHECKSUM_BYTESWAP);

	if (pc->io & DPUSM_IO_FILE)
		caps->zc_io |= ZIA_IO_FILE;
	if (pc->io & DPUSM_IO_DISK)
		caps->zc_io |= ZIA_IO_DISK;

	return (ZIA_OK);
}

static void *
zd_extract(void *handle)
{
	return (dpusm->extract(handle));
}

static void *
zd_alloc(void *provider, size_t size)
{
	return (dpusm->alloc(provider, size));
}

static void *
zd_alloc_ref(void *src, size_t offset, size_t size)
{
	return (dpusm->alloc_ref(src, offset, size));
}

static int
zd_free(void *handle)
{
	return (dpusm_to_zia(dpusm->free(handle)));
}

static int
zd_associate_handle(void *handle, void *ptr)
{
	return (dpusm_to_zia(dpusm->associate_handle(handle, ptr)));
}

static int
zd_copy_from_generic(zia_mv_t *zmv, const void *buf, size_t size)
{
	dpusm_mv_t mv = ZIA_TO_DPUSM_MV(zmv);
	return (dpusm_to_zia(dpusm->copy.from.generic(&mv, buf, size)));
}

static int
zd_copy_from_scatterlist(zia_mv_t *zmv, struct scatterlist *sgl,
    unsigned int nents, size_t size)
{
	dpusm_mv_t mv = ZIA_TO_DPUSM_MV(zmv);
	return (dpusm_to_zia(dpusm->copy.from.scatterlist(&mv,
	    sgl, nents, size)));
}

static int
zd_copy_to_generic(zia_mv_t *zmv, void *buf, size_t size)
{
	dpusm_mv_t mv = ZIA_TO_DPUSM_MV(zmv);
	return (dpusm_to_zia(dpusm->copy.to.generic(&mv, buf, size)));
}

static int
zd_copy_to_ptr(zia_mv_t *zmv, void *buf, size_t size)
{
	dpusm_mv_t mv = ZIA_TO_DPUSM_MV(zmv);
	return (dpusm_to_zia(dpusm->copy.to.ptr(&mv, buf, size)));
}

static int
zd_copy_to_scatterlist(zia_mv_t *zmv, struct scatterlist *sgl,
    unsigned int nents, size_t size)
{
	dpusm_mv_t mv = ZIA_TO_DPUSM_MV(zmv);
	return (dpusm_to_zia(dpusm->copy.to.scatterlist(&mv,
	    sgl, nents, size)));
}

static int
zd_zero_fill(void *handle, size_t offset, size_t size)
{
	return (dpusm_to_zia(dpusm->zero_fill(handle, offset, size)));
}

static int
zd_all_zeros(void *handle, size_t offset, size_t size)
{
	return (dpusm_to_zia(dpusm->all_zeros(handle, offset, size)));
}

static int
zd_compress(enum zio_compress c, int level, void *src, size_t s_len,
    void *dst, size_t *d_len)
{
	const dpusm_compress_t alg = compress_to_dpusm(c);
	if (alg == 0)
		return (ZIA_FALLBACK);
	return (dpusm_to_zia(dpusm->compress(alg, level,
	    src, s_len, dst, d_len)));
}

static int
zd_decompress(enum zio_compress c, int *level, void *src, size_t s_len,
    void *dst, size_t *d_len)
{
	const dpusm_compress_t alg = compress_to_dpusm(c);
	if (alg == 0)
		return (ZIA_FALLBACK);
	return (dpusm_to_zia(dpusm->decompress(alg, level,
	    src, s_len, dst, d_len)));
}

static int
zd_checksum(enum zio_checksum c, zio_byteorder_t order, void *data,
    size_t size, void *cksum, size_t cksum_size)
{
	const dpusm_checksum_t alg = checksum_to_dpusm(c);
	if (alg == 0)
		return (ZIA_FALLBACK);
	return (dpusm_to_zia(dpusm->checksum(alg, byteorder_to_dpusm(order),
	    data, size, cksum, cksum_size)));
}

static int
zd_raid_can_compute(void *provider, size_t nparity, size_t ndata,
    size_t *col_sizes, int rec)
{
	return (dpusm_to_zia(dpusm->raid.can_compute(provider, nparity,
	    ndata, col_sizes, rec)));
}

static void *
zd_raid_alloc(void *provider, size_t nparity, size_t ndata)
{
	return (dpusm->raid.alloc(provider, nparity, ndata));
}

static int
zd_raid_set_column(void *raid, uint64_t c, void *col, size_t size)
{
	return (dpusm_to_zia(dpusm->raid.set_column(raid, c, col, size)));
}

static int
zd_raid_free(void *raid)
{
	return (dpusm_to_zia(dpusm->raid.free(raid)));
}

static int
zd_raid_gen(void *raid)
{
	return (dpusm_to_zia(dpusm->raid.gen(raid)));
}

static int
zd_raid_cmp(void *lhs_handle, void *rhs_handle, int *diff)
{
	return (dpusm_to_zia(dpusm->raid.cmp(lhs_handle, rhs_handle, diff)));
}

static int
zd_raid_rec(void *raid, int *tgts, int ntgts)
{
	return (dpusm_to_zia(dpusm->raid.rec(raid, tgts, ntgts)));
}

static void *
zd_file_open(void *provider, const char *path, int flags, int mode)
{
	return (dpusm->file.open(provider, path, flags, mode));
}

/* returns errno values */
static int
zd_file_write(void *fp_handle, void *data, size_t size,
    size_t trailing_zeros, loff_t offset, uint8_t ashift,
    ssize_t *resid, int *err)
{
	return (dpusm->file.write(fp_handle, data, size, trailing_zeros,
	    offset, ashift, resid, err));
}

static int
zd_file_close(void *fp_handle)
{
	return (dpusm_to_zia(dpusm->file.close(fp_handle)));
}

static void *
zd_disk_open(void *provider, const char *path, struct block_device *bdev)
{
	return (dpusm->disk.open(provider, path, bdev));
}

static int
zd_disk_invalidate(void *disk_handle)
{
	return (dpusm_to_zia(dpusm->disk.invalidate(disk_handle)));
}

/* returns errno values */
static int
zd_disk_write(void *disk_handle, void *data, size_t data_size,
    size_t trailing_zeros, uint64_t io_offset, int flags,
    zia_io_done_t write_done, void *wd_arg)
{
	return (dpusm->disk.write(disk_handle, data, data_size,
	    trailing_zeros, io_offset, flags, write_done, wd_arg));
}

/* returns errno values */
static int
zd_disk_flush(void *disk_handle, zia_io_done_t flush_done, void *fd_arg)
{
	return (dpusm->disk.flush(disk_handle, flush_done, fd_arg));
}

static int
zd_disk_close(void *disk_handle)
{
	return (dpusm_to_zia(dpusm->disk.close(disk_handle)));
}

static zia_ops_t zia_dpusm_ops = {
	.zo_version			= ZIA_OPS_VERSION,
	.zo_size			= sizeof (zia_ops_t),
	.zo_get				= zd_get,
	.zo_get_name			= zd_get_name,
	.zo_put				= zd_put,
	.zo_capabilities		= zd_capabilities,
	.zo_extract			= zd_extract,
	.zo_alloc			= zd_alloc,
	.zo_alloc_ref			= zd_alloc_ref,
	.zo_free			= zd_free,
	.zo_associate_handle		= zd_associate_handle,
	.zo_copy_from_generic		= zd_copy_from_generic,
	.zo_copy_from_scatterlist	= zd_copy_from_scatterlist,
	.zo_copy_to_generic		= zd_copy_to_generic,
	.zo_copy_to_ptr			= zd_copy_to_ptr,
	.zo_copy_to_scatterlist		= zd_copy_to_scatterlist,
	.zo_zero_fill			= zd_zero_fill,
	.zo_all_zeros			= zd_all_zeros,
	.zo_compress			= zd_compress,
	.zo_decompress			= zd_decompress,
	.zo_checksum			= zd_checksum,
	.zo_raid_can_compute		= zd_raid_can_compute,
	.zo_raid_alloc			= zd_raid_alloc,
	.zo_raid_set_column		= zd_raid_set_column,
	.zo_raid_free			= zd_raid_free,
	.zo_raid_gen			= zd_raid_gen,
	.zo_raid_cmp			= zd_raid_cmp,
	.zo_raid_rec			= zd_raid_rec,
	.zo_file_open			= zd_file_open,
	.zo_file_write			= zd_file_write,
	.zo_file_close			= zd_file_close,
	.zo_disk_open			= zd_disk_open,
	.zo_disk_invalidate		= zd_disk_invalidate,
	.zo_disk_write			= zd_disk_write,
	.zo_disk_flush			= zd_disk_flush,
	.zo_disk_close			= zd_disk_close,
};

static int __init
zia_dpusm_init(void)
{
	/* dpusm_initialize() is declared weak; it is NULL without DPUSM */
	if (dpusm_initialize == NULL)
		return (-ENODEV);

	dpusm = dpusm_initialize();
	if (dpusm == NULL)
		return (-ENODEV);

	const int err = zia_ops_register(&zia_dpusm_ops, THIS_MODULE);
	if (err != 0) {
		if (dpusm_finalize != NULL)
			(void) dpusm_finalize();
		dpusm = NULL;
		return (-err);
	}

	return (0);
}

static void __exit
zia_dpusm_exit(void)
{
	/*
	 * Pools that use a provider hold a reference on this module, so
	 * unregistering cannot fail once the module is being removed.
	 */
	VERIFY0(zia_ops_unregister(&zia_dpusm_ops));
	if (dpusm_finalize != NULL)
		(void) dpusm_finalize();
	dpusm = NULL;
}

module_init(zia_dpusm_init);
module_exit(zia_dpusm_exit);

MODULE_DESCRIPTION("Z.I.A. to DPUSM glue");
MODULE_LICENSE("CDDL");
