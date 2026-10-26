// SPDX-License-Identifier: BSD-3-Clause
/*
 * © 2026. Triad National Security, LLC. All rights reserved.
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

/*
 * zia-dpusm: attaches Z.I.A. to the Data Processing Unit Services
 * Module (DPUSM, https://github.com/hpc/dpusm).
 *
 * ZFS does not know about the DPUSM and the DPUSM does not know about
 * ZFS. This module is the only place where the two meet: on load it
 * gets the DPUSM user API and registers it with Z.I.A. as a
 * zia_ops_t; on unload it unregisters it. Any DPUSM provider then
 * becomes usable with
 *
 *     zpool set zia_provider=<provider module name> <pool>
 *
 * Load order: zfs and dpusm (either order), then this module. DPUSM
 * providers can be loaded before or after this module. This module
 * cannot be unloaded while any pool holds a provider, and while it is
 * loaded neither zfs nor dpusm can be unloaded.
 *
 * Build with "make zia-modules DPUSM=<DPUSM build directory>". See
 * README.md in this directory for the full load and unload steps.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <sys/zia_ops.h>
#include <dpusm/user_api.h>

/*
 * Z.I.A. and the DPUSM use the same values for return codes and
 * capability bits, so they are passed through unchanged. Fail the
 * build if either side changes.
 */
#define	ZIA_DPUSM_SAME(z, d)	\
	_Static_assert((uint64_t)(z) == (uint64_t)(d), #z " != " #d)

ZIA_DPUSM_SAME(ZIA_OPS_OK, DPUSM_OK);
ZIA_DPUSM_SAME(ZIA_OPS_ERROR, DPUSM_ERROR);
ZIA_DPUSM_SAME(ZIA_OPS_PROVIDER_NOT_EXISTS, DPUSM_PROVIDER_NOT_EXISTS);
ZIA_DPUSM_SAME(ZIA_OPS_PROVIDER_UNREGISTERED, DPUSM_PROVIDER_UNREGISTERED);
ZIA_DPUSM_SAME(ZIA_OPS_PROVIDER_INVALIDATED, DPUSM_PROVIDER_INVALIDATED);
ZIA_DPUSM_SAME(ZIA_OPS_PROVIDER_MISMATCH, DPUSM_PROVIDER_MISMATCH);
ZIA_DPUSM_SAME(ZIA_OPS_NOT_IMPLEMENTED, DPUSM_NOT_IMPLEMENTED);
ZIA_DPUSM_SAME(ZIA_OPS_NOT_SUPPORTED, DPUSM_NOT_SUPPORTED);
ZIA_DPUSM_SAME(ZIA_OPS_BAD_RESULT, DPUSM_BAD_RESULT);

ZIA_DPUSM_SAME(ZIA_CAP_COPY_FROM_PTR, DPUSM_OPTIONAL_COPY_FROM_PTR);
ZIA_DPUSM_SAME(ZIA_CAP_COPY_TO_PTR, DPUSM_OPTIONAL_COPY_TO_PTR);
ZIA_DPUSM_SAME(ZIA_CAP_COPY_FROM_SCATTERLIST,
    DPUSM_OPTIONAL_COPY_FROM_SCATTERLIST);
ZIA_DPUSM_SAME(ZIA_CAP_COPY_TO_SCATTERLIST,
    DPUSM_OPTIONAL_COPY_TO_SCATTERLIST);
ZIA_DPUSM_SAME(ZIA_CAP_ASSOCIATE_HANDLE, DPUSM_OPTIONAL_ASSOCIATE_HANDLE);
ZIA_DPUSM_SAME(ZIA_CAP_MEM_STATS, DPUSM_OPTIONAL_MEM_STATS);
ZIA_DPUSM_SAME(ZIA_CAP_ZERO_FILL, DPUSM_OPTIONAL_ZERO_FILL);
ZIA_DPUSM_SAME(ZIA_CAP_ALL_ZEROS, DPUSM_OPTIONAL_ALL_ZEROS);

ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_1, DPUSM_COMPRESS_GZIP_1);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_2, DPUSM_COMPRESS_GZIP_2);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_3, DPUSM_COMPRESS_GZIP_3);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_4, DPUSM_COMPRESS_GZIP_4);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_5, DPUSM_COMPRESS_GZIP_5);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_6, DPUSM_COMPRESS_GZIP_6);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_7, DPUSM_COMPRESS_GZIP_7);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_8, DPUSM_COMPRESS_GZIP_8);
ZIA_DPUSM_SAME(ZIA_COMPRESS_GZIP_9, DPUSM_COMPRESS_GZIP_9);
ZIA_DPUSM_SAME(ZIA_COMPRESS_LZ4, DPUSM_COMPRESS_LZ4);

ZIA_DPUSM_SAME(ZIA_CHECKSUM_FLETCHER_2, DPUSM_CHECKSUM_FLETCHER_2);
ZIA_DPUSM_SAME(ZIA_CHECKSUM_FLETCHER_4, DPUSM_CHECKSUM_FLETCHER_4);
ZIA_DPUSM_SAME(ZIA_CHECKSUM_SHA256, DPUSM_CHECKSUM_SHA256);

ZIA_DPUSM_SAME(ZIA_BYTEORDER_NATIVE, DPUSM_BYTEORDER_NATIVE);
ZIA_DPUSM_SAME(ZIA_BYTEORDER_BYTESWAP, DPUSM_BYTEORDER_BYTESWAP);

ZIA_DPUSM_SAME(ZIA_IO_FILE, DPUSM_IO_FILE);
ZIA_DPUSM_SAME(ZIA_IO_DISK, DPUSM_IO_DISK);

/* the DPUSM user API, valid while this module is loaded */
static const dpusm_uf_t *dpusm = NULL;

/*
 * DPUSM providers are looked up by kernel module name, which always
 * uses '_' where the module file name may use '-'. Accept either.
 */
static void *
zia_dpusm_get(const char *name)
{
	char buf[MODULE_NAME_LEN];
	size_t i;

	if (name == NULL)
		return (NULL);

	for (i = 0; i < sizeof (buf) - 1 && name[i] != '\0'; i++)
		buf[i] = (name[i] == '-') ? '_' : name[i];

	/* too long to be a module name */
	if (name[i] != '\0')
		return (NULL);

	buf[i] = '\0';
	return (dpusm->get(buf));
}

static int
zia_dpusm_capabilities(void *provider, zia_caps_t *caps)
{
	dpusm_pc_t *pc = NULL;

	if (caps == NULL)
		return (DPUSM_ERROR);

	const int ret = dpusm->capabilities(provider, &pc);
	if (ret != DPUSM_OK)
		return (ret);

	if (pc == NULL)
		return (DPUSM_ERROR);

	/* DPUSM stores the masks as int; do not sign-extend bit 31 */
	caps->optional = (uint32_t)pc->optional;
	caps->compress = (uint32_t)pc->compress;
	caps->decompress = (uint32_t)pc->decompress;
	caps->checksum = (uint32_t)pc->checksum;
	caps->checksum_byteorder = (uint32_t)pc->checksum_byteorder;
	caps->raid = (uint32_t)pc->raid;
	caps->io = (uint32_t)pc->io;

	return (DPUSM_OK);
}

#define	ZIA_DPUSM_MV(zmv)	\
	{ .handle = (zmv)->handle, .offset = (zmv)->offset }

static int
zia_dpusm_copy_from_generic(zia_mv_t *mv, const void *buf, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.from.generic(&dmv, buf, size));
}

static int
zia_dpusm_copy_from_ptr(zia_mv_t *mv, const void *buf, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.from.ptr(&dmv, buf, size));
}

static int
zia_dpusm_copy_from_scatterlist(zia_mv_t *mv, struct scatterlist *sgl,
    unsigned int nents, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.from.scatterlist(&dmv, sgl, nents, size));
}

static int
zia_dpusm_copy_to_generic(zia_mv_t *mv, void *buf, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.to.generic(&dmv, buf, size));
}

static int
zia_dpusm_copy_to_ptr(zia_mv_t *mv, void *buf, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.to.ptr(&dmv, buf, size));
}

static int
zia_dpusm_copy_to_scatterlist(zia_mv_t *mv, struct scatterlist *sgl,
    unsigned int nents, size_t size)
{
	dpusm_mv_t dmv = ZIA_DPUSM_MV(mv);
	return (dpusm->copy.to.scatterlist(&dmv, sgl, nents, size));
}

static int
zia_dpusm_compress(zia_compress_t alg, int level,
    void *src, size_t s_len, void *dst, size_t *d_len)
{
	return (dpusm->compress((dpusm_compress_t)alg, level,
	    src, s_len, dst, d_len));
}

static int
zia_dpusm_decompress(zia_compress_t alg, int *level,
    void *src, size_t s_len, void *dst, size_t *d_len)
{
	return (dpusm->decompress((dpusm_decompress_t)alg, level,
	    src, s_len, dst, d_len));
}

static int
zia_dpusm_checksum(zia_checksum_t alg, zia_byteorder_t order,
    void *data, size_t size, void *cksum, size_t cksum_size)
{
	return (dpusm->checksum((dpusm_checksum_t)alg,
	    (dpusm_checksum_byteorder_t)order, data, size,
	    cksum, cksum_size));
}

static int
zia_dpusm_file_write(void *fp_handle, void *data, size_t size,
    size_t trailing_zeros, int64_t offset, uint8_t ashift,
    ssize_t *resid, int *err)
{
	return (dpusm->file.write(fp_handle, data, size, trailing_zeros,
	    (loff_t)offset, ashift, resid, err));
}

static zia_ops_t zia_dpusm_ops = {
	.zo_version = ZIA_OPS_VERSION,
	.zo_size = sizeof (zia_ops_t),
	.zo_name = "DPUSM",
};

/* fill in zia_dpusm_ops from the DPUSM user API */
static void
zia_dpusm_ops_init(const dpusm_uf_t *uf)
{
	zia_ops_t *ops = &zia_dpusm_ops;

	ops->get = zia_dpusm_get;
	ops->get_name = uf->get_name;
	ops->put = uf->put;
	ops->extract = uf->extract;
	ops->capabilities = zia_dpusm_capabilities;
	ops->alloc = uf->alloc;
	ops->alloc_ref = uf->alloc_ref;
	ops->free = uf->free;
	ops->associate_handle = uf->associate_handle;

	ops->copy.from.generic = zia_dpusm_copy_from_generic;
	ops->copy.from.ptr = uf->copy.from.ptr ?
	    zia_dpusm_copy_from_ptr : NULL;
	ops->copy.from.scatterlist = uf->copy.from.scatterlist ?
	    zia_dpusm_copy_from_scatterlist : NULL;
	ops->copy.to.generic = zia_dpusm_copy_to_generic;
	ops->copy.to.ptr = uf->copy.to.ptr ?
	    zia_dpusm_copy_to_ptr : NULL;
	ops->copy.to.scatterlist = uf->copy.to.scatterlist ?
	    zia_dpusm_copy_to_scatterlist : NULL;

	ops->zero_fill = uf->zero_fill;
	ops->all_zeros = uf->all_zeros;
	ops->compress = uf->compress ? zia_dpusm_compress : NULL;
	ops->decompress = uf->decompress ? zia_dpusm_decompress : NULL;
	ops->checksum = uf->checksum ? zia_dpusm_checksum : NULL;

	ops->raid.can_compute = uf->raid.can_compute;
	ops->raid.alloc = uf->raid.alloc;
	ops->raid.set_column = uf->raid.set_column;
	ops->raid.free = uf->raid.free;
	ops->raid.gen = uf->raid.gen;
	ops->raid.cmp = uf->raid.cmp;
	ops->raid.rec = uf->raid.rec;

	ops->file.open = uf->file.open;
	ops->file.write = uf->file.write ? zia_dpusm_file_write : NULL;
	ops->file.close = uf->file.close;

	ops->disk.open = uf->disk.open;
	ops->disk.invalidate = uf->disk.invalidate;
	ops->disk.write = uf->disk.write;
	ops->disk.flush = uf->disk.flush;
	ops->disk.close = uf->disk.close;
}

static int __init
zia_dpusm_init(void)
{
	/* dpusm_initialize() is declared weak by the DPUSM */
	if (dpusm_initialize == NULL) {
		printk(KERN_ERR "zia-dpusm: DPUSM is not loaded\n");
		return (-ENODEV);
	}

	const dpusm_uf_t *uf = dpusm_initialize();
	if (uf == NULL) {
		printk(KERN_ERR "zia-dpusm: DPUSM did not provide its API\n");
		return (-ENODEV);
	}

	dpusm = uf;
	zia_dpusm_ops_init(uf);

	/* zia_ops_register() returns a positive errno */
	const int err = zia_ops_register(&zia_dpusm_ops, THIS_MODULE);
	if (err != 0) {
		printk(KERN_ERR "zia-dpusm: could not attach Z.I.A. to the "
		    "DPUSM: %d\n", err);
		dpusm = NULL;
		if (dpusm_finalize != NULL)
			(void) dpusm_finalize();
		return (-err);
	}

	return (0);
}

static void __exit
zia_dpusm_exit(void)
{
	/*
	 * Z.I.A. holds a reference on this module for every provider a
	 * pool is using, so no pool can be using one now.
	 */
	const int err = zia_ops_unregister(&zia_dpusm_ops);
	if (err != 0) {
		printk(KERN_ERR "zia-dpusm: could not detach Z.I.A. from "
		    "the DPUSM: %d\n", err);
	}

	dpusm = NULL;
	if (dpusm_finalize != NULL)
		(void) dpusm_finalize();
}

module_init(zia_dpusm_init);
module_exit(zia_dpusm_exit);

MODULE_DESCRIPTION("Attaches Z.I.A. to the DPUSM");
MODULE_SOFTDEP("pre: dpusm");
MODULE_LICENSE("CDDL");
