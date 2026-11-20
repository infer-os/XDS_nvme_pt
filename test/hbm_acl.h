/*
 * Optional Ascend ACL helpers for physical-machine HBM buffers.
 * Resolved at runtime via dlopen("libascendcl.so") so the benches still
 * build on VMs without the Ascend toolkit.
 */
#ifndef XDS_HBM_ACL_H
#define XDS_HBM_ACL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Match aclrtMemMallocPolicy in CANN acl_rt.h */
enum xds_acl_malloc_policy {
	XDS_ACL_MEM_MALLOC_HUGE_FIRST = 0,
	XDS_ACL_MEM_MALLOC_HUGE_ONLY = 1,
	XDS_ACL_MEM_MALLOC_NORMAL_ONLY = 2,
	XDS_ACL_MEM_MALLOC_HUGE_FIRST_P2P = 3,
	XDS_ACL_MEM_MALLOC_HUGE_ONLY_P2P = 4,
};

struct xds_hbm_buf {
	void *addr;
	size_t size;
	int device_id;
	bool active;
};

/*
 * A group of independently allocated device buffers on one device. Each
 * buffer is a separate aclrtMalloc() call, so the VAs are separate ranges
 * unless the allocator happens to place them next to each other.
 */
struct xds_hbm_set {
	void **addrs;			/* nr entries, owned by the set */
	size_t size;			/* bytes per buffer */
	unsigned int nr;		/* buffer count */
	int device_id;
	bool active;
};

/* Returns 0 on success, negative errno-style or positive ACL error. */
int xds_hbm_alloc(struct xds_hbm_buf *buf, int device_id, size_t size,
		  int malloc_policy);
void xds_hbm_free(struct xds_hbm_buf *buf);

/*
 * Allocate @nr separate buffers of @size bytes each. The whole set shares one
 * ACL lifecycle, so do not mix it with xds_hbm_alloc() in the same process.
 * On failure the partially allocated set is released and all addresses are
 * NULL. Free with xds_hbm_free_set().
 */
int xds_hbm_alloc_set(struct xds_hbm_set *set, int device_id, size_t size,
		      unsigned int nr, int malloc_policy);
void xds_hbm_free_set(struct xds_hbm_set *set);

/*
 * Copy @n bytes from device HBM (@device) into host (@host).
 * Requires an active xds_hbm_alloc on the same device (set_device still in effect).
 */
int xds_hbm_memcpy_d2h(void *host, const void *device, size_t n);

#endif /* XDS_HBM_ACL_H */
