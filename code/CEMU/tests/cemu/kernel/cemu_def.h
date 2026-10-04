#ifndef CEMU_DEF_H
#define CEMU_DEF_H

typedef long long (*cemu_nvm_io_fn)(void *context, unsigned int nsid,
                                    unsigned long long byte_offset,
                                    void *buffer, unsigned long long length);
typedef long long (*cemu_cache_notify_fn)(void *context, unsigned int nsid,
                                          unsigned int relation_oid,
                                          unsigned long long physical_offset,
                                          unsigned long long logical_offset,
                                          unsigned long long length,
                                          unsigned long long generation);

struct cemu_args {
    int numr;
    void **mr_addr;
    long long *mr_len;
    long long cparam1;
    long long cparam2;
    void *data_buffer;
    long long buffer_len;
    void *service_context;
    cemu_nvm_io_fn nvm_read;
    cemu_nvm_io_fn nvm_write;
    cemu_cache_notify_fn cache_notify;
} __attribute__((packed));

// re-impl assert
#ifdef assert
#  undef assert
#endif

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr)                                                     \
    do {                                                                   \
      if (!(expr)) {                                                       \
        fprintf(stderr,                                                    \
                "ASSERT FAILED: %s\n  at %s:%d (%s)\n",                     \
                #expr, __FILE__, __LINE__, __func__);                      \
      }                                                                    \
    } while (0)
#endif

#endif
