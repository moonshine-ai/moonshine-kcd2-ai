/* sys/time.h - gettimeofday, for the engine's wall-clock prints. */
#ifndef QWEN_MSVC_SYS_TIME_H
#define QWEN_MSVC_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The same guard winsock uses, so the two definitions never meet. */
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

struct timezone;
int gettimeofday(struct timeval *tv, struct timezone *tz);

#ifdef __cplusplus
}
#endif

#endif
