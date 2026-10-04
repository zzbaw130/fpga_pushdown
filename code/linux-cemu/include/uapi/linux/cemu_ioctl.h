/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CEMU_IOCTL_H
#define _UAPI_LINUX_CEMU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * CEMU ioctl ABI
 *
 * Use standard Linux ioctl encoding macros so command values are unique and
 * carry argument size information.
 *
 * Pointer fields are represented as __u64 in the ABI. Kernel code should cast
 * them to (void __user *)(uintptr_t) before using copy_from_user helpers.
 */
#define CEMU_IOCTL_MAGIC  'C'
#define CEMU_EXEC_CACHE_TRACK        0x40
#define CEMU_EXEC_HOST_CACHE_OVERLAP 0x80

/* IOCTL_CEMU_DOWNLOAD / IOCTL_CEMU_UNLOAD / ACTIVATE / DEACTIVATE argument */
struct ioctl_download {
	__u64	name;          /* const char * (user pointer) */
	__u64	addr;          /* void * (user pointer) */
	__s32	size;
	__s32	ptype;
	__s32	runtime;
	__s32	runtime_scale;
	__s32	jit;
	__s32	indirect;
	__s32	pind;          /* out for DOWNLOAD */
};

/* IOCTL_CEMU_EXECUTE argument */
struct ioctl_execute {
	__u64	cparam1;
	__u64	cparam2;
	__u64	memory_fd;     /* int * (user pointer) */
	__u64	buffer;        /* void * (user pointer) */
	__u16	nr_fd;
	__u16	buffer_len;
	__u16	pind;
	__u16	rsid;
	__u32	runtime;
};

/* IOCTL_CEMU_CREATE_MRS / IOCTL_CEMU_DELETE_MRS argument */
struct ioctl_create_mrs {
	__s32	nr_fd;
	__u64	fd;            /* int * (user pointer) */
	__u64	off;           /* long long * (user pointer) */
	__u64	size;          /* long long * (user pointer) */
	__u16	rsid;          /* out for CREATE_MRS / in for DELETE_MRS */
};

/* Start a new best-effort AER cache-fill generation. */
struct ioctl_cemu_cache_begin {
	__u64	generation;
	__u32	flags;
	__u32	reserved;
};

/* Associate a PostgreSQL relation byte range with an open heap-segment fd. */
struct ioctl_cemu_cache_register {
	__u64	generation;
	__u64	logical_offset;
	__u64	length;
	__u32	relation_oid;
	__s32	fd;
};

/* Observable counters for experiments and asynchronous completion polling. */
struct ioctl_cemu_cache_stats {
	__u64	active_generation;
	__u64	completed_generation;
	__u64	last_sequence;
	__u64	events;
	__u64	notified_bytes;
	__u64	prefetch_requests;
	__u64	prefetched_bytes;
	__u64	unmapped_bytes;
	__u64	controller_dropped;
	__u64	errors;
	__u64	final_events;
	__u64	execute_start_ns;
	__u64	execute_complete_ns;
	__u64	first_prefetch_ns;
	__u64	last_prefetch_ns;
	__u64	overlap_prefetched_bytes;
};

/* Wait until the AER worker has completed a cache-fill batch for generation. */
struct ioctl_cemu_cache_wait {
	__u64	generation;
	__u32	timeout_ms;
	__u32	reserved;
};

#define IOCTL_CEMU_DOWNLOAD    _IOWR(CEMU_IOCTL_MAGIC, 0x00, struct ioctl_download)
#define IOCTL_CEMU_UNLOAD      _IOW (CEMU_IOCTL_MAGIC, 0x01, struct ioctl_download)
#define IOCTL_CEMU_ACTIVATE    _IOW (CEMU_IOCTL_MAGIC, 0x02, struct ioctl_download)
#define IOCTL_CEMU_DEACTIVATE  _IOW (CEMU_IOCTL_MAGIC, 0x03, struct ioctl_download)
#define IOCTL_CEMU_EXECUTE     _IOW (CEMU_IOCTL_MAGIC, 0x04, struct ioctl_execute)
#define IOCTL_CEMU_CREATE_MRS  _IOWR(CEMU_IOCTL_MAGIC, 0x05, struct ioctl_create_mrs)
#define IOCTL_CEMU_DELETE_MRS  _IOW (CEMU_IOCTL_MAGIC, 0x06, struct ioctl_create_mrs)
#define IOCTL_CEMU_CACHE_BEGIN _IOW (CEMU_IOCTL_MAGIC, 0x07, struct ioctl_cemu_cache_begin)
#define IOCTL_CEMU_CACHE_REGISTER _IOW(CEMU_IOCTL_MAGIC, 0x08, struct ioctl_cemu_cache_register)
#define IOCTL_CEMU_CACHE_STATS _IOR (CEMU_IOCTL_MAGIC, 0x09, struct ioctl_cemu_cache_stats)
#define IOCTL_CEMU_CACHE_WAIT  _IOW (CEMU_IOCTL_MAGIC, 0x0a, struct ioctl_cemu_cache_wait)

#endif /* _UAPI_LINUX_CEMU_IOCTL_H */

