/* dirent.h - directory listing over _findfirst, for the sharded-weights fallback. */
#ifndef QWEN_MSVC_DIRENT_H
#define QWEN_MSVC_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    char d_name[260];
};

typedef struct qwen_msvc_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif
