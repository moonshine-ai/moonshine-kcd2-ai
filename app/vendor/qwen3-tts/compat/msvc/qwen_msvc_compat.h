/* qwen_msvc_compat.h - what the engine assumes of a GCC/POSIX toolchain, said in MSVC.
 *
 * Force-included (/FI) into every C file of the engine and its vendored readers, and into
 * nothing else. The engine was written against gcc and clang on Linux and macOS: it leans
 * on __builtin_* and __attribute__, on <pthread.h> and <unistd.h>, on mmap, and on being
 * able to free() memory that came from posix_memalign(). MSVC has none of those as such.
 * Rather than scatter #ifdef _MSC_VER through forty files, this header maps each of them
 * once; the few places that needed a real code change are marked "MSVC:" in the sources.
 *
 * Allocation: every malloc/calloc/realloc/free/strdup in the engine is routed through the
 * _aligned_* family at 64 bytes. The engine mixes posix_memalign and malloc freely and
 * frees both with free(); on Windows an _aligned_malloc'd block cannot be freed with free(),
 * so the only way to keep that mixing legal is to make all of it aligned. The cost is a
 * header per allocation; the benefit is that every buffer in the engine is 64-byte aligned,
 * which the SIMD paths may assume. Memory that crosses to the C++ side (generated samples)
 * is released through qwen_tts_voice_free_samples(), never a bare free(). */
#ifndef QWEN_MSVC_COMPAT_H
#define QWEN_MSVC_COMPAT_H
#ifdef _MSC_VER

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS 1
#endif
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

/* The C runtime first, so the renames below do not rewrite its declarations. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <malloc.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <intrin.h>
#include <immintrin.h>

/* The engine tests the GCC architecture macros. MSVC says _M_X64 and, with /arch:AVX2,
 * __AVX2__; FMA and F16C come with that /arch level but have no macro of their own. */
#if defined(_M_X64) && !defined(__x86_64__)
#define __x86_64__ 1
#endif
#if defined(__AVX2__)
#ifndef __FMA__
#define __FMA__ 1
#endif
#ifndef __F16C__
#define __F16C__ 1
#endif
#endif
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#endif

/* GCC extensions with an MSVC spelling or no meaning here. Alignment attributes were
 * rewritten to _Alignas in the sources, so dropping the rest is safe. */
#define __attribute__(x)
#define __restrict__ __restrict
#define __thread __declspec(thread)
#define __builtin_expect(expr, value) (expr)
#define __builtin_memcpy memcpy
#define __builtin_memmove memmove
#define __builtin_cpu_init() ((void)0)
#define __builtin_ia32_stmxcsr() _mm_getcsr()
#define __builtin_ia32_ldmxcsr(v) _mm_setcsr(v)
#define QWEN_MSVC_PREFETCH_(p, ...) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#define __builtin_prefetch(...) QWEN_MSVC_PREFETCH_(__VA_ARGS__, 0, 0)

static __inline int __builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
static __inline int __builtin_clz(unsigned x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
static __inline int __builtin_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (int)i; }
static __inline int __builtin_clzll(unsigned long long x) { unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i; }

int qwen_msvc_cpu_supports(const char *feature);
#define __builtin_cpu_supports qwen_msvc_cpu_supports

/* POSIX names. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
typedef int pid_t;
/* MSVC: a static helper in the engine was named yn(), which <math.h> here declares as the
 * Bessel function of the second kind; the helper was renamed yes_no() in the source. */
#define _SC_NPROCESSORS_ONLN 1
#define _SC_PAGESIZE 2
#define _SC_CLK_TCK 3
long sysconf(int name);
int usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);
ptrdiff_t pread(int fd, void *buf, size_t count, long long offset);
#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_DONTNEED 4
static __inline int posix_fadvise(int fd, long long offset, long long len, int advice) {
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define getpid _getpid
#define strtok_r strtok_s
#define ftello _ftelli64
#define fseeko _fseeki64
static __inline struct tm *gmtime_r(const time_t *t, struct tm *out) {
    return gmtime_s(out, t) == 0 ? out : NULL;
}
static __inline struct tm *localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
/* Files past 2 GiB: the plain stat has a 32-bit st_size on Windows, and the talker
 * weights are 1.2 GB. The macro renames both the struct and the call. */
#define stat _stat64
#define fstat _fstat64
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
/* Binary mode, or the CRT translates line endings and stops at a 0x1A byte. */
#define open(path, flags) _open((path), (flags) | _O_BINARY)
FILE *qwen_msvc_fopen(const char *path, const char *mode);
#define fopen qwen_msvc_fopen

/* Allocation, all of it aligned (see the file comment). */
void *qwen_msvc_malloc(size_t n);
void *qwen_msvc_calloc(size_t count, size_t size);
void *qwen_msvc_realloc(void *p, size_t n);
void qwen_msvc_free(void *p);
char *qwen_msvc_strdup(const char *s);
int qwen_msvc_posix_memalign(void **out, size_t alignment, size_t size);
void *qwen_msvc_aligned_alloc(size_t alignment, size_t size);
#define malloc qwen_msvc_malloc
#define calloc qwen_msvc_calloc
#define realloc qwen_msvc_realloc
#define free qwen_msvc_free
#define strdup qwen_msvc_strdup
#define posix_memalign qwen_msvc_posix_memalign
#define aligned_alloc qwen_msvc_aligned_alloc

/* Time. */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif
typedef int clockid_t;
int clock_gettime(clockid_t clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* _MSC_VER */
#endif /* QWEN_MSVC_COMPAT_H */
