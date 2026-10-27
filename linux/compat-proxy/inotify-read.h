#ifndef MATONOS_INOTIFY_READ_H
#define MATONOS_INOTIFY_READ_H

#include <stddef.h>
#include <errno.h>

/* A positive-length read from a Linux inotify instance cannot report EOF.
 * Keep this policy separate from fd identification so the host regression can
 * inject the impossible result without relying on a kernel bug. */
static inline long proxy_inotify_read_result(size_t count, long result,
                                             int inotify_fd) {
    if (result == 0 && count != 0 && inotify_fd)
        return -EAGAIN;
    return result;
}

#endif
