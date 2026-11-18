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
 * Copyright (c) 2026, TrueNAS.
 */

#include <sys/zalgo.h>
#include <sys/avl.h>
#ifdef _KERNEL
#include <linux/module.h>
#endif

typedef enum {
	ZG_DUMMY = 0,	/* dummy type, for test */
	ZG_MAC,
	ZG_DIGEST,
	ZG_CHECKSUM,
	ZG_CIPHER,
	ZG_TYPE_MAX,
} zalgo_type_t;

static const uint_t zalgo_subtype_max[ZG_TYPE_MAX] = {
	ZG_DUMMY_SUBTYPE_MAX,
	ZG_MAC_SUBTYPE_MAX,
	ZG_DIGEST_SUBTYPE_MAX,
	ZG_CHECKSUM_SUBTYPE_MAX,
	ZG_CIPHER_SUBTYPE_MAX,
};

static const char *zalgo_type_str[ZG_TYPE_MAX] = {
	"dummy",
	"mac",
	"digest",
	"checksum",
	"cipher",
};

static const char *zalgo_dummy_subtype_str[ZG_DUMMY_SUBTYPE_MAX] = {
	"dummy-0",
	"dummy-1",
	"dummy-2",
};

static const char *zalgo_mac_subtype_str[ZG_MAC_SUBTYPE_MAX] = {
	"HMAC-SHA512"
};

static const char *zalgo_digest_subtype_str[ZG_DIGEST_SUBTYPE_MAX] = {
	"SHA256",
	"SHA512",
	"SHA512-256",
};

static const char *zalgo_checksum_subtype_str[ZG_CHECKSUM_SUBTYPE_MAX] = {
	"fletcher2",
	"fletcher2-byteswap",
	"fletcher4",
	"fletcher4-byteswap",
	"sha256",
	"sha256-byteswap",
	"sha512",
	"sha512-byteswap",
	"skein",
	"skein-byteswap",
	"edonr",
	"edonr-byteswap",
	"blake3",
	"blake3-byteswap",
};

static const char *zalgo_cipher_subtype_str[ZG_CIPHER_SUBTYPE_MAX] = {
	"AES-CCM",
	"AES-GCM",
};

static const char *unknown = "[unknown]";

static const char *
zalgo_type_to_str(zalgo_type_t type)
{
	return (type < ZG_TYPE_MAX ? zalgo_type_str[type] : unknown);
}

static const char *
zalgo_subtype_to_str(zalgo_type_t type, uint_t subtype)
{
	switch (type) {
	case ZG_DUMMY:
		return (subtype < ZG_DUMMY_SUBTYPE_MAX ?
		    zalgo_dummy_subtype_str[subtype] : unknown);
	case ZG_MAC:
		return (subtype < ZG_MAC_SUBTYPE_MAX ?
		    zalgo_mac_subtype_str[subtype] : unknown);
	case ZG_DIGEST:
		return (subtype < ZG_DIGEST_SUBTYPE_MAX ?
		    zalgo_digest_subtype_str[subtype] : unknown);
	case ZG_CHECKSUM:
		return (subtype < ZG_CHECKSUM_SUBTYPE_MAX ?
		    zalgo_checksum_subtype_str[subtype] : unknown);
	case ZG_CIPHER:
		return (subtype < ZG_CIPHER_SUBTYPE_MAX ?
		    zalgo_cipher_subtype_str[subtype] : unknown);
	default:
		return (unknown);
	}

	__builtin_unreachable();
}

typedef struct {
	const void	*zgn_ops;
	const char	*zgn_id;
	const char	*zgn_desc;
	uint_t		zgn_subtype;
	uint_t		zgn_flags;
	uint64_t	zgn_score;
	uint64_t	zgn_refcount;
	zalgo_type_t	zgn_type;
	void		*zgn_owner;
	boolean_t	zgn_draining;
	kcondvar_t	zgn_cv;
	list_node_t	zgn_link;
} zalgo_node_t;

struct zalgo_registration {
	zalgo_node_t	*zgr_node;
};

typedef struct {
	zalgo_node_t	*zr_current;
	zalgo_node_t	*zr_fallback;
	zalgo_node_t	*zr_best;
	list_t		zr_nodes;
} zalgo_registry_t;

/*
 * The registry is an array [ZG_TYPE_MAX] of arrays [ZG_XXX_SUBTYPE_MAX] of
 * zalgo_registry_t, which holds the atomic pointer for the currently selected
 * implementation, and a list of all registered nodes (in arbitrary order).
 */
static zalgo_registry_t *zg_registry[ZG_TYPE_MAX] = {};
static kmutex_t zg_registry_lock;

static int
zalgo_register_flags(zalgo_type_t type, uint_t subtype, const char *id,
    const char *desc, const void *ops, uint_t flags, void *owner,
    zalgo_registration_t **registration)
{
	ASSERT3U(type, <, ZG_TYPE_MAX);
	ASSERT3U(subtype, <, zalgo_subtype_max[type]);
	ASSERT3P(id, !=, NULL);
	ASSERT3P(desc, !=, NULL);
	ASSERT3P(ops, !=, NULL);
	if (owner != NULL && registration == NULL)
		return (EINVAL);

	zalgo_registry_t *reg = &zg_registry[type][subtype];

	zalgo_node_t *node = kmem_alloc(sizeof (zalgo_node_t), KM_SLEEP);
	node->zgn_subtype = subtype;
	node->zgn_type = type;
	node->zgn_id = id;
	node->zgn_desc = desc;
	node->zgn_ops = ops;
	node->zgn_flags = flags;
	node->zgn_score = 0;
	/* The registry list owns one reference.  Each role and operation hold
	 * takes an additional reference while it is published/active. */
	node->zgn_refcount = 1;
	node->zgn_owner = owner;
	node->zgn_draining = B_FALSE;
	cv_init(&node->zgn_cv, NULL, CV_DEFAULT, NULL);

	mutex_enter(&zg_registry_lock);
	for (zalgo_node_t *exist = list_head(&reg->zr_nodes); exist != NULL;
	    exist = list_next(&reg->zr_nodes, exist)) {
		if ((exist->zgn_ops == node->zgn_ops) ||
		    strcmp(exist->zgn_id, node->zgn_id) == 0) {
			/* Don't allow id or ops to be reused. */
			mutex_exit(&zg_registry_lock);
			cv_destroy(&node->zgn_cv);
			kmem_free(node, sizeof (zalgo_node_t));
			return (EEXIST);
		}
	}

	list_insert_tail(&reg->zr_nodes, node);

	boolean_t is_current = (reg->zr_current == NULL);
	if (is_current) {
		reg->zr_current = node;
		node->zgn_refcount++;
	}
	if ((flags & ZALGO_PROVIDER_ACCELERATED) == 0 &&
	    reg->zr_fallback == NULL) {
		reg->zr_fallback = node;
		node->zgn_refcount++;
	}
	mutex_exit(&zg_registry_lock);
	if (registration != NULL) {
		*registration = kmem_alloc(sizeof (**registration), KM_SLEEP);
		(*registration)->zgr_node = node;
	}

	cmn_err(CE_NOTE, "zalgo: registered '%s' (%s) for %s:%s%s",
	    id, desc, zalgo_type_to_str(type),
	    zalgo_subtype_to_str(type, subtype),
	    is_current ? " [selected]" : "");

	return (0);
}

static int
zalgo_unregister(zalgo_registration_t *registration)
{
	zalgo_node_t *node;
	zalgo_registry_t *reg;
	zalgo_node_t *replacement = NULL;
	boolean_t removed_current = B_FALSE;
	boolean_t removed_fallback = B_FALSE;
	boolean_t removed_best = B_FALSE;

	if (registration == NULL || (node = registration->zgr_node) == NULL)
		return (EINVAL);

	mutex_enter(&zg_registry_lock);
	reg = &zg_registry[node->zgn_type][node->zgn_subtype];
	if (node->zgn_draining) {
		mutex_exit(&zg_registry_lock);
		return (EALREADY);
	}
	node->zgn_draining = B_TRUE;

	for (zalgo_node_t *candidate = list_head(&reg->zr_nodes);
	    candidate != NULL;
	    candidate = list_next(&reg->zr_nodes, candidate)) {
		if (candidate != node && !candidate->zgn_draining &&
		    (candidate->zgn_flags & ZALGO_PROVIDER_ACCELERATED) == 0) {
			replacement = candidate;
			break;
		}
	}

	if (reg->zr_current == node) {
		reg->zr_current = replacement;
		removed_current = B_TRUE;
		if (replacement != NULL)
			replacement->zgn_refcount++;
	}
	if (reg->zr_fallback == node) {
		reg->zr_fallback = replacement;
		removed_fallback = B_TRUE;
		if (replacement != NULL)
			replacement->zgn_refcount++;
	}
	if (reg->zr_best == node) {
		reg->zr_best = replacement;
		removed_best = B_TRUE;
		if (replacement != NULL)
			replacement->zgn_refcount++;
	}

	list_remove(&reg->zr_nodes, node);
	/* Drop the list and role references before waiting for operation holds. */
	node->zgn_refcount--;
	if (removed_current)
		node->zgn_refcount--;
	if (removed_fallback)
		node->zgn_refcount--;
	if (removed_best)
		node->zgn_refcount--;
	while (node->zgn_refcount != 0)
		cv_wait(&node->zgn_cv, &zg_registry_lock);

	mutex_exit(&zg_registry_lock);
	cv_destroy(&node->zgn_cv);
	kmem_free(node, sizeof (*node));
	kmem_free(registration, sizeof (*registration));
	return (0);
}

static void *
zalgo_hold(zalgo_type_t type, uint_t subtype)
{
	ASSERT3U(type, <, ZG_TYPE_MAX);
	ASSERT3U(subtype, <, zalgo_subtype_max[type]);

	zalgo_registry_t *reg = &zg_registry[type][subtype];
	zalgo_node_t *node;

	mutex_enter(&zg_registry_lock);
	node = reg->zr_current;
	if (node == NULL || node->zgn_draining) {
		mutex_exit(&zg_registry_lock);
		return (NULL);
	}
	if (node->zgn_owner != NULL) {
#ifdef _KERNEL
		if (!try_module_get((struct module *)node->zgn_owner)) {
			mutex_exit(&zg_registry_lock);
			return (NULL);
		}
#endif
	}
	node->zgn_refcount++;
	mutex_exit(&zg_registry_lock);

	ASSERT3U(node->zgn_subtype, ==, subtype);

	return (node);
}

static void *
zalgo_hold_fallback(zalgo_type_t type, uint_t subtype)
{
	ASSERT3U(type, <, ZG_TYPE_MAX);
	ASSERT3U(subtype, <, zalgo_subtype_max[type]);

	zalgo_registry_t *reg = &zg_registry[type][subtype];
	zalgo_node_t *node;

	mutex_enter(&zg_registry_lock);
	node = reg->zr_fallback;
	if (node != NULL && !node->zgn_draining) {
		node->zgn_refcount++;
	}
	mutex_exit(&zg_registry_lock);

	return (node);
}

static void
zalgo_rele(void *nodep)
{
	zalgo_node_t *node = nodep;
	#ifdef _KERNEL
	if (node->zgn_owner != NULL)
		module_put((struct module *)node->zgn_owner);
	#endif
	mutex_enter(&zg_registry_lock);
	VERIFY3U(node->zgn_refcount, >, 0);
	node->zgn_refcount--;
	if (node->zgn_draining && node->zgn_refcount == 0)
		cv_broadcast(&node->zgn_cv);
	mutex_exit(&zg_registry_lock);
}

/*
 * XXX FreeBSD sys/amd64/include/atomic.h aliases atomic_*_ptr to
 *     atomic_*_long, which has u_long types, not void*, which trips
 *     -Wincompatible-pointer-types. hacking around it for now
 *       -- robn, 2026-08-13
 */
#if defined(__FreeBSD__) && defined(_KERNEL)
#define zg_atomic_swap_ptr(target, newval) \
	(void *)(uintptr_t)(atomic_swap_ptr((volatile void *)(target), (uintptr_t)(void *)newval))
#else
#define zg_atomic_swap_ptr atomic_swap_ptr
#endif

static int
zalgo_select(zalgo_type_t type, uint_t subtype, const char *id)
{
	ASSERT3U(type, <, ZG_TYPE_MAX);
	ASSERT3U(subtype, <, zalgo_subtype_max[type]);

	zalgo_registry_t *reg = &zg_registry[type][subtype];

	mutex_enter(&zg_registry_lock);
	for (zalgo_node_t *node = list_head(&reg->zr_nodes); node != NULL;
	    node = list_next(&reg->zr_nodes, node)) {
		if (!node->zgn_draining && strcmp(node->zgn_id, id) == 0) {
			if (node == reg->zr_current) {
				mutex_exit(&zg_registry_lock);
				return (0);
			}
			node->zgn_refcount++;
			zalgo_node_t *old = reg->zr_current;
			reg->zr_current = node;
			if (old != NULL)
				old->zgn_refcount--;
			cmn_err(CE_NOTE, "zalgo: selected '%s' (%s) for %s:%s",
			    node->zgn_id, node->zgn_desc,
			    zalgo_type_to_str(type),
			    zalgo_subtype_to_str(type, subtype));
			mutex_exit(&zg_registry_lock);
			return (0);
		}
	}
	mutex_exit(&zg_registry_lock);

	return (ENOENT);
}

static void zalgo_kstat_init(void);
static void zalgo_kstat_fini(void);

void
zalgo_init(void)
{
	for (zalgo_type_t type = 0; type < ZG_TYPE_MAX; type++) {
		zg_registry[type] = kmem_alloc(sizeof (zalgo_registry_t) *
		    zalgo_subtype_max[type], KM_SLEEP);
		for (uint_t subtype = 0;
		    subtype < zalgo_subtype_max[type]; subtype++) {
			zalgo_registry_t *reg = &zg_registry[type][subtype];
			reg->zr_current = NULL;
			reg->zr_fallback = NULL;
			reg->zr_best = NULL;
			list_create(&reg->zr_nodes, sizeof (zalgo_node_t),
			    offsetof(zalgo_node_t, zgn_link));
		}
	}
	mutex_init(&zg_registry_lock, NULL, MUTEX_DEFAULT, NULL);

	zalgo_kstat_init();
}

void
zalgo_fini(void)
{
	zalgo_kstat_fini();
	mutex_enter(&zg_registry_lock);

	for (zalgo_type_t type = 0; type < ZG_TYPE_MAX; type++) {
		for (uint_t subtype = 0;
		    subtype < zalgo_subtype_max[type]; subtype++) {
			zalgo_registry_t *reg = &zg_registry[type][subtype];
			zalgo_node_t *node;
			while ((node = list_remove_head(&reg->zr_nodes)) != NULL) {
				node->zgn_draining = B_TRUE;
				if (reg->zr_current == node) {
					reg->zr_current = NULL;
					node->zgn_refcount--;
				}
				if (reg->zr_fallback == node) {
					reg->zr_fallback = NULL;
					node->zgn_refcount--;
				}
				if (reg->zr_best == node) {
					reg->zr_best = NULL;
					node->zgn_refcount--;
				}
				node->zgn_refcount--;
				while (node->zgn_refcount != 0)
					cv_wait(&node->zgn_cv, &zg_registry_lock);
				cv_destroy(&node->zgn_cv);
				kmem_free(node, sizeof (zalgo_node_t));
			}
			list_destroy(&reg->zr_nodes);
		}
		kmem_free(zg_registry[type],
		    sizeof (zalgo_registry_t) * zalgo_subtype_max[type]);
	}
	mutex_exit(&zg_registry_lock);
	mutex_destroy(&zg_registry_lock);
}

/* ========== */

#define	ZALGO_DEFINE_API(ty, en)					\
int									\
zalgo_##ty##_register_flags(zalgo_##ty##_subtype_t subtype,	\
    const char *id, const char *desc, const zalgo_##ty##_ops_t *ops,	\
    uint_t flags)						\
{									\
	return (zalgo_register_flags(ZG_##en, subtype, id, desc, ops,	\
	    flags, NULL, NULL));					\
}									\
EXPORT_SYMBOL(zalgo_##ty##_register_flags);						\
									\
int									\
zalgo_##ty##_register_owner(zalgo_##ty##_subtype_t subtype,	\
    const char *id, const char *desc, const zalgo_##ty##_ops_t *ops,	\
    uint_t flags, void *owner, zalgo_registration_t **registration)	\
{									\
	return (zalgo_register_flags(ZG_##en, subtype, id, desc, ops,	\
    flags, owner, registration));					\
}									\
EXPORT_SYMBOL(zalgo_##ty##_register_owner);						\
									\
int									\
zalgo_##ty##_unregister(zalgo_registration_t *registration)	\
{									\
	return (zalgo_unregister(registration));					\
}									\
EXPORT_SYMBOL(zalgo_##ty##_unregister);							\
									\
int									\
zalgo_##ty##_register(zalgo_##ty##_subtype_t subtype, const char *id,	\
    const char *desc, const zalgo_##ty##_ops_t *ops)			\
{									\
	return (zalgo_register_flags(ZG_##en, subtype, id, desc, ops,	\
	    ZALGO_PROVIDER_SOFTWARE, NULL, NULL));				\
}									\
EXPORT_SYMBOL(zalgo_##ty##_register);					\
									\
zalgo_##ty##_hold_t *							\
zalgo_##ty##_hold(zalgo_##ty##_subtype_t subtype)			\
{									\
	return (zalgo_hold(ZG_##en, subtype));				\
}									\
zalgo_##ty##_hold_t *						\
zalgo_##ty##_hold_fallback(zalgo_##ty##_subtype_t subtype)	\
{									\
	return (zalgo_hold_fallback(ZG_##en, subtype));					\
}									\
									\
zalgo_##ty##_hold_t *								\
zalgo_##ty##_hold_spa(spa_t *spa, zalgo_##ty##_subtype_t subtype)	\
{									\
	(void) spa;								\
	return (zalgo_hold(ZG_##en, subtype));					\
}								\
\
void									\
zalgo_##ty##_rele(zalgo_##ty##_hold_t *hold)				\
{									\
	return (zalgo_rele(hold));					\
}									\
									\
int									\
zalgo_##ty##_select(zalgo_##ty##_subtype_t subtype, const char *id)	\
{									\
	return (zalgo_select(ZG_##en, subtype, id));			\
}									\

ZALGO_DEFINE_API(dummy, DUMMY)
ZALGO_DEFINE_API(mac, MAC)
ZALGO_DEFINE_API(digest, DIGEST)
ZALGO_DEFINE_API(checksum, CHECKSUM)
ZALGO_DEFINE_API(cipher, CIPHER)

int
zalgo_checksum_once_fallback(zalgo_checksum_subtype_t subtype,
    abd_t *abd, size_t datalen, zio_cksum_t *checksum)
{
	zalgo_checksum_hold_t *hold = zalgo_checksum_hold(subtype);
	int error;

	if (hold == NULL)
		goto fallback;
	error = zalgo_checksum_once_abd(hold, abd, datalen, checksum);
	zalgo_checksum_rele(hold);
	if (error == 0)
		return (0);

fallback:
	hold = zalgo_checksum_hold_fallback(subtype);
	if (hold == NULL)
		return (ZALGO_FALLBACK);
	error = zalgo_checksum_once_abd(hold, abd, datalen, checksum);
	zalgo_checksum_rele(hold);
	return (error == 0 ? 0 : ZALGO_FALLBACK);
}

int
zalgo_checksum_once_fallback_spa(spa_t *spa,
    zalgo_checksum_subtype_t subtype, abd_t *abd, size_t datalen,
    zio_cksum_t *checksum)
{
	zalgo_checksum_hold_t *hold = zalgo_checksum_hold_spa(spa, subtype);
	int error;

	if (hold == NULL)
		goto fallback;
	if (hold->zh_ops->zgc_op_once_spa != NULL)
		error = zalgo_checksum_once_spa(hold, spa, abd, datalen, checksum);
	else
		error = zalgo_checksum_once_abd(hold, abd, datalen, checksum);
	zalgo_checksum_rele(hold);
	if (error == 0)
		return (0);

fallback:
	hold = zalgo_checksum_hold_fallback(subtype);
	if (hold == NULL)
		return (ZALGO_FALLBACK);
	error = zalgo_checksum_once_abd(hold, abd, datalen, checksum);
	zalgo_checksum_rele(hold);
	return (error == 0 ? 0 : ZALGO_FALLBACK);
}

/* ========== */

typedef uint64_t (*zalgo_bench_fn_t)(zalgo_node_t *node);
static const zalgo_bench_fn_t zalgo_bench_fn[ZG_TYPE_MAX] = {
	NULL,
	NULL,
	NULL,
	(zalgo_bench_fn_t)zalgo_checksum_bench,
	(zalgo_bench_fn_t)zalgo_cipher_bench,
};

static void
zalgo_bench(zalgo_type_t type, uint_t subtype)
{
	zalgo_bench_fn_t fn = zalgo_bench_fn[type];
	if (fn == NULL)
		return;

	zalgo_registry_t *reg = &zg_registry[type][subtype];

	zalgo_node_t *best = NULL;

	mutex_enter(&zg_registry_lock);
	for (zalgo_node_t *node = list_head(&reg->zr_nodes); node != NULL;
	    node = list_next(&reg->zr_nodes, node)) {
		if (node->zgn_draining)
			continue;
		node->zgn_score = fn(node);
		if (best == NULL || node->zgn_score > best->zgn_score)
			best = node;
	}
	zalgo_node_t *fallback = NULL;
	for (zalgo_node_t *node = list_head(&reg->zr_nodes); node != NULL;
	    node = list_next(&reg->zr_nodes, node)) {
		if ((node->zgn_flags & ZALGO_PROVIDER_ACCELERATED) == 0 &&
		    (fallback == NULL || node->zgn_score > fallback->zgn_score))
			fallback = node;
	}
	if (best == NULL) {
		mutex_exit(&zg_registry_lock);
		return;
	}

	if (reg->zr_best != best) {
		zalgo_node_t *old_best = reg->zr_best;
		best->zgn_refcount++;
		reg->zr_best = best;
		if (old_best != NULL)
			old_best->zgn_refcount--;
	}
	if (reg->zr_fallback != fallback) {
		zalgo_node_t *old_fallback = reg->zr_fallback;
		if (fallback != NULL)
			fallback->zgn_refcount++;
		reg->zr_fallback = fallback;
		if (old_fallback != NULL)
			old_fallback->zgn_refcount--;
	}
	zalgo_node_t *old = reg->zr_current;
	if (old != best) {
		best->zgn_refcount++;
		reg->zr_current = best;
		if (old != NULL)
			old->zgn_refcount--;
	}
	if (old == NULL) {
		cmn_err(CE_NOTE,
		    "zalgo: selected '%s' (%s) for %s:%s [best]",
		    best->zgn_id, best->zgn_desc,
		    zalgo_type_to_str(type),
		    zalgo_subtype_to_str(type, subtype));
	} else if (best != old) {
		uint64_t ratio = (best->zgn_score << 8) / old->zgn_score;
		cmn_err(CE_NOTE, "zalgo: selected '%s' (%s) for %s:%s "
		    "[best] [from '%s', %llu.%03llux]",
		    best->zgn_id, best->zgn_desc,
		    zalgo_type_to_str(type),
		    zalgo_subtype_to_str(type, subtype),
		    old->zgn_id, (u_longlong_t)(ratio >> 8),
		    (u_longlong_t)(ratio & ((1 << 8)-1)));
	}

	mutex_exit(&zg_registry_lock);
}

void
zalgo_bench_all(void)
{
	for (zalgo_type_t type = 0; type < ZG_TYPE_MAX; type++)
		for (uint_t subtype = 0;
		    subtype < zalgo_subtype_max[type]; subtype++)
			zalgo_bench(type, subtype);
}

/* ========== */

static kstat_t *zalgo_ksp = NULL;

static int
zalgo_kstat_headers(char *buf, size_t size)
{
	size_t n = snprintf(buf, size,
	    "%-8s  %-20s  %-20s  %-6s  %s\n",
	    "TYPE", "SUBTYPE", "ID", "SCORE", "FLAGS");
	return (n >= size ? ENOMEM : 0);
}

static int
zalgo_kstat_data(char *buf, size_t size, void *data)
{
	(void)data;
	size_t n;
	int err = 0;

	mutex_enter(&zg_registry_lock);
	for (zalgo_type_t type = 0; err == 0 && type < ZG_TYPE_MAX; type++) {
		for (uint_t subtype = 0;
		    err == 0 && subtype < zalgo_subtype_max[type]; subtype++) {
			zalgo_registry_t *reg = &zg_registry[type][subtype];
			for (zalgo_node_t *node = list_head(&reg->zr_nodes);
			    err == 0 && node != NULL;
			    node = list_next(&reg->zr_nodes, node)) {
				n = snprintf(buf, size,
				    "%-8s  %-20s  %-20s  %-6llu %s%s\n",
				    zalgo_type_to_str(type),
				    zalgo_subtype_to_str(type, subtype),
				    node->zgn_id,
				    (u_longlong_t)node->zgn_score,
				    node == reg->zr_best ? " [best]" : "",
				    node == reg->zr_current ? " [selected]" : "");
				if (n >= size) {
					err = ENOMEM;
					break;
				}

				buf = &buf[n];
				size -= n;
			}
		}
	}
	mutex_exit(&zg_registry_lock);

	return (err);
}

static void
zalgo_kstat_init(void)
{
	kstat_t *ksp = kstat_create("zfs", 0, "zalgo", "misc",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);
	if (ksp == NULL)
		return;

	ksp->ks_data = (void *)(uintptr_t)1;
	ksp->ks_ndata = 1;
	kstat_set_raw_ops(ksp, zalgo_kstat_headers,
	    zalgo_kstat_data, NULL);
	kstat_install(ksp);

	zalgo_ksp = ksp;
}

static void
zalgo_kstat_fini(void)
{
	if (zalgo_ksp == NULL)
		return;

	kstat_delete(zalgo_ksp);
	zalgo_ksp = NULL;
}
