/* sys/mman.h - read-only file mapping over CreateFileMapping, which is all the weight
 * readers ask for. madvise is accepted and ignored: Windows manages its own page cache. */
#ifndef QWEN_MSVC_SYS_MMAN_H
#define QWEN_MSVC_SYS_MMAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_HUGEPAGE 14
#define POSIX_MADV_NORMAL 0
#define POSIX_MADV_DONTNEED 4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset);
int munmap(void *addr, size_t length);
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);

#ifdef __cplusplus
}
#endif

#endif
