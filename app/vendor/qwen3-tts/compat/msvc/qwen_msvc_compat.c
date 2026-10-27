/* qwen_msvc_compat.c - the Win32 side of the POSIX shims declared in this directory.
 *
 * This is the one engine translation unit that includes <windows.h>; the headers keep it
 * out of the others by holding opaque pointers where Win32 would have its own types. */
#ifdef _MSC_VER

/* Windows first, before the compat header renames malloc and friends underneath it. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>

#include "qwen_msvc_compat.h"
#include "pthread.h"
#include "unistd.h"
#include "dirent.h"
#include "sys/time.h"
#include "sys/mman.h"

/* ---- allocation ------------------------------------------------------------------- */

#define QWEN_MSVC_ALIGN 64

void *qwen_msvc_malloc(size_t n) { return _aligned_malloc(n ? n : 1, QWEN_MSVC_ALIGN); }
void *qwen_msvc_calloc(size_t count, size_t size) {
    return _aligned_recalloc(NULL, count ? count : 1, size ? size : 1, QWEN_MSVC_ALIGN);
}
void *qwen_msvc_realloc(void *p, size_t n) { return _aligned_realloc(p, n ? n : 1, QWEN_MSVC_ALIGN); }
void qwen_msvc_free(void *p) { _aligned_free(p); }
char *qwen_msvc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)qwen_msvc_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
int qwen_msvc_posix_memalign(void **out, size_t alignment, size_t size) {
    if (alignment < QWEN_MSVC_ALIGN) alignment = QWEN_MSVC_ALIGN;
    *out = _aligned_malloc(size ? size : 1, alignment);
    return *out ? 0 : ENOMEM;
}
void *qwen_msvc_aligned_alloc(size_t alignment, size_t size) {
    if (alignment < QWEN_MSVC_ALIGN) alignment = QWEN_MSVC_ALIGN;
    return _aligned_malloc(size ? size : 1, alignment);
}

/* ---- cpu --------------------------------------------------------------------------- */

int qwen_msvc_cpu_supports(const char *feature) {
    int r1[4] = {0}, r7[4] = {0};
    __cpuid(r1, 1);
    __cpuidex(r7, 7, 0);
    const int osxsave = (r1[2] >> 27) & 1;
    unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
    const int ymm = (xcr0 & 0x6) == 0x6;
    const int zmm = ymm && (xcr0 & 0xe0) == 0xe0;
    if (!strcmp(feature, "sse2")) return 1;
    if (!strcmp(feature, "sse4.1")) return (r1[2] >> 19) & 1;
    if (!strcmp(feature, "sse4.2")) return (r1[2] >> 20) & 1;
    if (!strcmp(feature, "avx")) return ymm && ((r1[2] >> 28) & 1);
    if (!strcmp(feature, "f16c")) return ymm && ((r1[2] >> 29) & 1);
    if (!strcmp(feature, "fma")) return ymm && ((r1[2] >> 12) & 1);
    if (!strcmp(feature, "avx2")) return ymm && ((r7[1] >> 5) & 1);
    if (!strcmp(feature, "avx512f")) return zmm && ((r7[1] >> 16) & 1);
    if (!strcmp(feature, "avx512bw")) return zmm && ((r7[1] >> 30) & 1);
    if (!strcmp(feature, "avx512vl")) return zmm && ((r7[1] >> 31) & 1);
    if (!strcmp(feature, "avx512vnni")) return zmm && ((r7[2] >> 11) & 1);
    if (!strcmp(feature, "avx512bf16")) {
        int r71[4] = {0};
        __cpuidex(r71, 7, 1);
        return zmm && ((r71[0] >> 5) & 1);
    }
    if (!strcmp(feature, "amx-tile")) return (r7[3] >> 24) & 1;
    if (!strcmp(feature, "amx-int8")) return ((r7[3] >> 24) & 1) && ((r7[3] >> 25) & 1);
    if (!strcmp(feature, "amx-bf16")) return ((r7[3] >> 24) & 1) && ((r7[3] >> 22) & 1);
    return 0;
}

/* ---- time -------------------------------------------------------------------------- */

int clock_gettime(clockid_t clock, struct timespec *ts) {
    if (clock == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL; /* 1601 -> 1970, in 100ns ticks */
    ts->tv_sec = (time_t)(t / 10000000ULL);
    ts->tv_nsec = (long)((t % 10000000ULL) * 100);
    return 0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = (long)ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    long long ms = (long long)req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
    if (ms <= 0) {
        SwitchToThread();
    } else {
        Sleep((DWORD)ms);
    }
    return 0;
}

int usleep(unsigned int usec) {
    Sleep((usec + 999) / 1000);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    Sleep(seconds * 1000u);
    return 0;
}

int sched_yield(void) {
    SwitchToThread();
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN: {
        DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return n > 0 ? (long)n : 1;
    }
    case _SC_PAGESIZE: {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwPageSize;
    }
    case _SC_CLK_TCK:
        return 100;
    default:
        return -1;
    }
}

/* ---- files ------------------------------------------------------------------------- */

#undef fopen
FILE *qwen_msvc_fopen(const char *path, const char *mode) {
    char m[8] = {0};
    size_t n = 0;
    int binary = 0;
    for (const char *p = mode; *p && n < sizeof m - 2; p++) {
        if (*p == 't') continue;
        if (*p == 'b') binary = 1;
        m[n++] = *p;
    }
    if (!binary) m[n++] = 'b';
    m[n] = 0;
    return fopen(path, m);
}

ptrdiff_t pread(int fd, void *buf, size_t count, long long offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < count) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        unsigned long long at = (unsigned long long)offset + done;
        ov.Offset = (DWORD)(at & 0xffffffffu);
        ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD want = (DWORD)((count - done) > 0x40000000u ? 0x40000000u : (count - done));
        DWORD got = 0;
        if (!ReadFile(h, (char *)buf + done, want, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ptrdiff_t)done : -1;
        }
        if (got == 0) break;
        done += got;
    }
    return (ptrdiff_t)done;
}

/* A mapping remembers its section handle so munmap can close it. The engine only ever
 * maps a whole file read-only at offset zero (or whatever offset the reader computed), and
 * unmaps with the address it was given, so a small table suffices. */
typedef struct {
    void *addr;
    HANDLE section;
} mapping_t;
static mapping_t g_maps[64];
static SRWLOCK g_maps_lock = SRWLOCK_INIT;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset) {
    (void)addr;
    (void)flags;
    if (fd < 0 || (prot & PROT_WRITE)) return MAP_FAILED;
    HANDLE file = (HANDLE)_get_osfhandle(fd);
    if (file == INVALID_HANDLE_VALUE) return MAP_FAILED;
    HANDLE section = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!section) return MAP_FAILED;
    void *view = MapViewOfFile(section, FILE_MAP_READ, (DWORD)((unsigned long long)offset >> 32),
                               (DWORD)((unsigned long long)offset & 0xffffffffu), length);
    if (!view) {
        CloseHandle(section);
        return MAP_FAILED;
    }
    AcquireSRWLockExclusive(&g_maps_lock);
    int stored = 0;
    for (size_t i = 0; i < sizeof g_maps / sizeof g_maps[0]; i++) {
        if (!g_maps[i].addr) {
            g_maps[i].addr = view;
            g_maps[i].section = section;
            stored = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_maps_lock);
    if (!stored) {
        /* More live mappings than the table holds; keep the view, leak the section
         * handle. Sixty-four simultaneous weight files is not a case the engine has. */
    }
    return view;
}

int munmap(void *addr, size_t length) {
    (void)length;
    HANDLE section = NULL;
    AcquireSRWLockExclusive(&g_maps_lock);
    for (size_t i = 0; i < sizeof g_maps / sizeof g_maps[0]; i++) {
        if (g_maps[i].addr == addr) {
            section = g_maps[i].section;
            g_maps[i].addr = NULL;
            g_maps[i].section = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_maps_lock);
    UnmapViewOfFile(addr);
    if (section) CloseHandle(section);
    return 0;
}

int madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

int posix_madvise(void *addr, size_t length, int advice) {
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

struct qwen_msvc_dir {
    intptr_t handle;
    struct __finddata64_t data;
    struct dirent entry;
    int first;
};

DIR *opendir(const char *path) {
    size_t n = strlen(path);
    char *pattern = (char *)qwen_msvc_malloc(n + 3);
    if (!pattern) return NULL;
    memcpy(pattern, path, n);
    if (n && path[n - 1] != '/' && path[n - 1] != '\\') pattern[n++] = '/';
    pattern[n++] = '*';
    pattern[n] = 0;
    DIR *d = (DIR *)qwen_msvc_calloc(1, sizeof *d);
    if (!d) {
        qwen_msvc_free(pattern);
        return NULL;
    }
    d->handle = _findfirst64(pattern, &d->data);
    qwen_msvc_free(pattern);
    if (d->handle == -1) {
        qwen_msvc_free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->first) {
        d->first = 0;
    } else if (_findnext64(d->handle, &d->data) != 0) {
        return NULL;
    }
    strncpy(d->entry.d_name, d->data.name, sizeof d->entry.d_name - 1);
    d->entry.d_name[sizeof d->entry.d_name - 1] = 0;
    return &d->entry;
}

int closedir(DIR *d) {
    if (!d) return -1;
    _findclose(d->handle);
    qwen_msvc_free(d);
    return 0;
}

/* ---- threads ----------------------------------------------------------------------- */

struct qwen_msvc_thread {
    HANDLE handle;
    void *(*fn)(void *);
    void *arg;
    void *ret;
};

static unsigned __stdcall thread_trampoline(void *p) {
    struct qwen_msvc_thread *t = (struct qwen_msvc_thread *)p;
    t->ret = t->fn(t->arg);
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    struct qwen_msvc_thread *t = (struct qwen_msvc_thread *)qwen_msvc_calloc(1, sizeof *t);
    if (!t) return EAGAIN;
    t->fn = fn;
    t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!h) {
        qwen_msvc_free(t);
        return EAGAIN;
    }
    t->handle = (HANDLE)h;
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **ret) {
    if (!t) return EINVAL;
    WaitForSingleObject(t->handle, INFINITE);
    if (ret) *ret = t->ret;
    CloseHandle(t->handle);
    qwen_msvc_free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    if (!t) return EINVAL;
    /* The record is leaked on purpose: the running thread still writes its return
     * value into it. Detached threads in the engine are process-lifetime. */
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
}

pthread_t pthread_self(void) {
    /* Only ever printed or compared; a thread id in disguise. */
    return (pthread_t)(uintptr_t)GetCurrentThreadId();
}

int pthread_setname_np(const char *name) {
    (void)name;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    (void)attr;
    InitializeSRWLock((PSRWLOCK)&m->handle);
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->handle); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->handle); return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    (void)attr;
    InitializeConditionVariable((PCONDITION_VARIABLE)&c->handle);
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->handle, (PSRWLOCK)&m->handle, INFINITE, 0) ? 0 : EINVAL;
}
int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable((PCONDITION_VARIABLE)&c->handle); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->handle); return 0; }

static BOOL CALLBACK once_trampoline(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)context;
    ((void (*)(void))param)();
    return TRUE;
}

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    return InitOnceExecuteOnce((PINIT_ONCE)&once->handle, once_trampoline, (PVOID)fn, NULL) ? 0 : EINVAL;
}

#endif /* _MSC_VER */
