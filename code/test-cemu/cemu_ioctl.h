/* Userspace CEMU ioctl ABI
 *
 * Keep in sync with the kernel UAPI header:
 *   code/linux-cemu/include/uapi/linux/cemu_ioctl.h
 */
#ifndef _CEMU_TEST_IOCTL_H
#define _CEMU_TEST_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

#define CEMU_IOCTL_MAGIC  'C'
#define CEMU_EXEC_CACHE_TRACK        0x40
#define CEMU_EXEC_HOST_CACHE_OVERLAP 0x80

/* IOCTL_CEMU_DOWNLOAD / IOCTL_CEMU_UNLOAD / ACTIVATE / DEACTIVATE argument */
struct ioctl_download {
    const char *name;
    void       *addr;
    int32_t     size;
    int32_t     ptype;
    int32_t     runtime;
    int32_t     runtime_scale;
    int32_t     jit;
    int32_t     indirect;
    int32_t     pind;   /* out for DOWNLOAD */
};

/* IOCTL_CEMU_EXECUTE argument */
struct ioctl_execute {
    uint64_t    cparam1;
    uint64_t    cparam2;
    int        *memory_fd;
    void       *buffer;
    uint16_t    nr_fd;
    uint16_t    buffer_len;
    uint16_t    pind;
    uint16_t    rsid;
    uint32_t    runtime;
};

/* IOCTL_CEMU_CREATE_MRS / IOCTL_CEMU_DELETE_MRS argument */
struct ioctl_create_mrs {
    int         nr_fd;
    int        *fd;     /* fd array of FDMFS */
    long long  *off;    /* offset array */
    long long  *size;   /* size array */
    uint16_t    rsid;   /* out for CREATE_MRS / in for DELETE_MRS */
};

struct ioctl_cemu_cache_begin {
    uint64_t    generation;
    uint32_t    flags;
    uint32_t    reserved;
};

struct ioctl_cemu_cache_register {
    uint64_t    generation;
    uint64_t    logical_offset;
    uint64_t    length;
    uint32_t    relation_oid;
    int32_t     fd;
};

struct ioctl_cemu_cache_stats {
    uint64_t    active_generation;
    uint64_t    completed_generation;
    uint64_t    last_sequence;
    uint64_t    events;
    uint64_t    notified_bytes;
    uint64_t    prefetch_requests;
    uint64_t    prefetched_bytes;
    uint64_t    unmapped_bytes;
    uint64_t    controller_dropped;
    uint64_t    errors;
    uint64_t    final_events;
    uint64_t    execute_start_ns;
    uint64_t    execute_complete_ns;
    uint64_t    first_prefetch_ns;
    uint64_t    last_prefetch_ns;
    uint64_t    overlap_prefetched_bytes;
};

struct ioctl_cemu_cache_wait {
    uint64_t    generation;
    uint32_t    timeout_ms;
    uint32_t    reserved;
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

#endif /* _CEMU_TEST_IOCTL_H */
