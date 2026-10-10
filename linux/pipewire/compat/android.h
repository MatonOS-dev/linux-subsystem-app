/* Build-time compatibility for unmodified upstream PipeWire on bionic.
 * loop.cancel stays false: normal shutdown invokes the loop's stop callback.
 * Optional pthread affinity tuning is unavailable through the NDK. */
#pragma once
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>

/* Android's loader can map a stored, aligned DSO directly from an APK,
 * while libc stat cannot see APK entries. PipeWire probes module files with
 * stat before dlopen. Confirm such a path with the loader itself; leave all
 * real filesystem paths and failures to libc. No extraction to app data. */
static inline int maton_pw_stat(const char *path, struct stat *info) {
    int result = stat(path, info);
    if (result == 0 || !strstr(path, ".apk!/lib/")) return result;
    int saved_errno = errno;
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) { errno = saved_errno; return -1; }
    dlclose(library);
    memset(info, 0, sizeof(*info));
    info->st_mode = S_IFREG | 0444;
    return 0;
}
#define stat(path, info) maton_pw_stat(path, info)
static inline int maton_pw_cancel(pthread_t thread) {
    (void)thread;
    return ENOTSUP;
}
static inline int maton_pw_affinity(pthread_t thread, size_t size, const void *set) {
    (void)thread; (void)size; (void)set;
    return ENOTSUP;
}
#define pthread_cancel maton_pw_cancel
#define pthread_setaffinity_np maton_pw_affinity
