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

#ifndef _SYS_ZIA_OPS_H
#define	_SYS_ZIA_OPS_H

#include <sys/types.h>

/*
 * Z.I.A. accelerator operations
 *
 * Z.I.A. does not link against any accelerator framework. Instead, a
 * separate bridge module (e.g. zia-dpusm.ko for the DPUSM) fills in a
 * zia_ops_t and passes it to zia_ops_register() once ZFS is loaded.
 * Until a table is registered, every Z.I.A. operation falls back to
 * the normal ZFS code path.
 *
 * All handles are opaque to Z.I.A. Provider handles come from get(),
 * offloader buffer handles from alloc()/alloc_ref(), and file/disk
 * handles from file.open()/disk.open().
 *
 * The constants below are part of the interface. Bridges must
 * translate their framework's values to these, or check at compile
 * time that they are identical.
 */

/* Interface version. Bump when zia_ops_t or any constant changes. */
#define	ZIA_OPS_VERSION	1

/* Return values of zia_ops_t functions that do not return errnos */
#define	ZIA_OPS_OK			0
#define	ZIA_OPS_ERROR			1
#define	ZIA_OPS_PROVIDER_NOT_EXISTS	2
#define	ZIA_OPS_PROVIDER_UNREGISTERED	3
#define	ZIA_OPS_PROVIDER_INVALIDATED	4
#define	ZIA_OPS_PROVIDER_MISMATCH	5
#define	ZIA_OPS_NOT_IMPLEMENTED		6
#define	ZIA_OPS_NOT_SUPPORTED		7
#define	ZIA_OPS_BAD_RESULT		8

/* zia_caps_t.optional */
#define	ZIA_CAP_COPY_FROM_PTR		(1ULL << 0)
#define	ZIA_CAP_COPY_TO_PTR		(1ULL << 1)
#define	ZIA_CAP_COPY_FROM_SCATTERLIST	(1ULL << 2)
#define	ZIA_CAP_COPY_TO_SCATTERLIST	(1ULL << 3)
#define	ZIA_CAP_ASSOCIATE_HANDLE	(1ULL << 4)
#define	ZIA_CAP_MEM_STATS		(1ULL << 5)
#define	ZIA_CAP_ZERO_FILL		(1ULL << 6)
#define	ZIA_CAP_ALL_ZEROS		(1ULL << 7)

/* zia_caps_t.compress and zia_caps_t.decompress */
typedef uint64_t zia_compress_t;
#define	ZIA_COMPRESS_GZIP_1	(1ULL << 0)
#define	ZIA_COMPRESS_GZIP_2	(1ULL << 1)
#define	ZIA_COMPRESS_GZIP_3	(1ULL << 2)
#define	ZIA_COMPRESS_GZIP_4	(1ULL << 3)
#define	ZIA_COMPRESS_GZIP_5	(1ULL << 4)
#define	ZIA_COMPRESS_GZIP_6	(1ULL << 5)
#define	ZIA_COMPRESS_GZIP_7	(1ULL << 6)
#define	ZIA_COMPRESS_GZIP_8	(1ULL << 7)
#define	ZIA_COMPRESS_GZIP_9	(1ULL << 8)
#define	ZIA_COMPRESS_LZ4	(1ULL << 9)

/* zia_caps_t.checksum */
typedef uint64_t zia_checksum_t;
#define	ZIA_CHECKSUM_FLETCHER_2	(1ULL << 0)
#define	ZIA_CHECKSUM_FLETCHER_4	(1ULL << 1)
#define	ZIA_CHECKSUM_SHA256	(1ULL << 3)

/* zia_caps_t.checksum_byteorder */
typedef uint64_t zia_byteorder_t;
#define	ZIA_BYTEORDER_INVALID	0ULL
#define	ZIA_BYTEORDER_NATIVE	(1ULL << 0)
#define	ZIA_BYTEORDER_BYTESWAP	(1ULL << 1)

/* zia_caps_t.io */
#define	ZIA_IO_FILE		(1ULL << 0)
#define	ZIA_IO_DISK		(1ULL << 1)

/* Capabilities of a provider. Each member is a bitmask of the above. */
typedef struct zia_caps {
	uint64_t optional;
	uint64_t compress;
	uint64_t decompress;
	uint64_t checksum;
	uint64_t checksum_byteorder;
	uint64_t raid;
	uint64_t io;
} zia_caps_t;

/* Location within an offloader buffer for copies */
typedef struct zia_mv {
	void *handle;
	size_t offset;
} zia_mv_t;

/* Called by the provider when an asynchronous disk write/flush ends */
typedef void (*zia_completion_t)(void *arg, int error);

struct block_device;
struct scatterlist;

typedef struct zia_ops {
	uint32_t zo_version;	/* ZIA_OPS_VERSION */
	uint32_t zo_size;	/* sizeof (zia_ops_t) */
	const char *zo_name;	/* name of the bridge, for messages */

	/*
	 * Required
	 */

	/* get a reference to a provider by name; NULL if not found */
	void *(*get)(const char *name);
	const char *(*get_name)(void *provider);
	/* return a reference obtained with get() */
	int (*put)(void *provider);
	/* provider that owns an offloader buffer handle */
	void *(*extract)(void *handle);
	int (*capabilities)(void *provider, zia_caps_t *caps);

	void *(*alloc)(void *provider, size_t size);
	void *(*alloc_ref)(void *src, size_t offset, size_t size);
	int (*free)(void *handle);
	int (*associate_handle)(void *handle, void *ptr);

	struct {
		/* memory -> offloader */
		struct {
			int (*generic)(zia_mv_t *mv, const void *buf,
			    size_t size);
			/* optional */
			int (*ptr)(zia_mv_t *mv, const void *buf,
			    size_t size);
			/* optional, kernel only */
			int (*scatterlist)(zia_mv_t *mv,
			    struct scatterlist *sgl, unsigned int nents,
			    size_t size);
		} from;

		/* offloader -> memory */
		struct {
			int (*generic)(zia_mv_t *mv, void *buf, size_t size);
			/* optional */
			int (*ptr)(zia_mv_t *mv, void *buf, size_t size);
			/* optional, kernel only */
			int (*scatterlist)(zia_mv_t *mv,
			    struct scatterlist *sgl, unsigned int nents,
			    size_t size);
		} to;
	} copy;

	/*
	 * Operations. A bridge must provide all of these, but they may
	 * return ZIA_OPS_NOT_IMPLEMENTED or ZIA_OPS_NOT_SUPPORTED. The
	 * provider's capabilities decide which ones Z.I.A. calls.
	 */
	int (*zero_fill)(void *handle, size_t offset, size_t size);
	int (*all_zeros)(void *handle, size_t offset, size_t size);

	int (*compress)(zia_compress_t alg, int level,
	    void *src, size_t s_len, void *dst, size_t *d_len);
	int (*decompress)(zia_compress_t alg, int *level,
	    void *src, size_t s_len, void *dst, size_t *d_len);

	int (*checksum)(zia_checksum_t alg, zia_byteorder_t order,
	    void *data, size_t size, void *cksum, size_t cksum_size);

	struct {
		int (*can_compute)(void *provider, size_t nparity,
		    size_t ndata, size_t *col_sizes, int rec);
		void *(*alloc)(void *provider, size_t nparity, size_t ndata);
		int (*set_column)(void *raid, uint64_t c, void *col,
		    size_t size);
		int (*free)(void *raid);
		int (*gen)(void *raid);
		int (*cmp)(void *lhs_handle, void *rhs_handle, int *diff);
		int (*rec)(void *raid, int *tgts, int ntgts);
	} raid;

	struct {
		/* returns NULL on error */
		void *(*open)(void *provider, const char *path, int flags,
		    int mode);
		/* returns an errno */
		int (*write)(void *fp_handle, void *data, size_t size,
		    size_t trailing_zeros, int64_t offset, uint8_t ashift,
		    ssize_t *resid, int *err);
		int (*close)(void *fp_handle);
	} file;

	struct {
		/* returns NULL on error */
		void *(*open)(void *provider, const char *path,
		    struct block_device *bdev);
		int (*invalidate)(void *disk_handle);
		/* returns an errno */
		int (*write)(void *disk_handle, void *data, size_t data_size,
		    size_t trailing_zeros, uint64_t io_offset, int flags,
		    zia_completion_t write_completion, void *wc_args);
		/* returns an errno */
		int (*flush)(void *disk_handle,
		    zia_completion_t flush_completion, void *fc_args);
		int (*close)(void *disk_handle);
	} disk;
} zia_ops_t;

/*
 * Attach/detach an operations table.
 *
 * Only one table can be registered at a time; it gives Z.I.A. access
 * to every provider behind it. owner is the registering kernel module
 * (THIS_MODULE on Linux). Z.I.A. holds a reference on it for as long
 * as any pool holds a provider, so the table stays valid while in use.
 *
 * zia_ops_register() returns 0, EINVAL if the table is incomplete or
 * for a different ZIA_OPS_VERSION, EBUSY if a table is already
 * registered, or ENOTSUP if ZFS was built without Z.I.A.
 *
 * zia_ops_unregister() returns 0, EINVAL if ops is not the registered
 * table, or EBUSY if a pool still holds a provider.
 */
int zia_ops_register(const zia_ops_t *ops, void *owner);
int zia_ops_unregister(const zia_ops_t *ops);

#endif /* _SYS_ZIA_OPS_H */
