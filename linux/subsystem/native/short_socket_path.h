#ifndef MATON_SHORT_SOCKET_PATH_H
#define MATON_SHORT_SOCKET_PATH_H

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Keep dirfd open until all users of the returned path have stopped. */
static inline int maton_socket_dir_open(const char *directory) {
  return open(directory, O_PATH | O_DIRECTORY | O_CLOEXEC);
}

static inline int maton_socket_dir_path(char *out, size_t capacity, int dirfd) {
  int n = snprintf(out, capacity, "/proc/self/fd/%d", dirfd);
  return n > 0 && (size_t)n < capacity ? n : -1;
}

static inline int maton_socket_path(char *out, size_t capacity, int dirfd,
                                    const char *name) {
  if (!out || !name || !*name || strchr(name, '/')) return -1;
  int n = maton_socket_dir_path(out, capacity, dirfd);
  if (n < 0 || (size_t)n + 1 + strlen(name) >= capacity) return -1;
  out[n++] = '/';
  strcpy(out + n, name);
  return n + (int)strlen(name);
}

/* Open a full private path's parent and return a short proc-fd path. */
static inline int maton_socket_path_open(char *out, size_t capacity,
                                         const char *path, int *dirfd) {
  if (!path || path[0] != '/' || !dirfd) return -1;
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return -1;
  size_t directory_length = (size_t)(slash - path);
  char directory[PATH_MAX];
  if (directory_length == 0) {
    directory[0] = '/'; directory[1] = '\0';
  } else {
    if (directory_length >= sizeof(directory)) return -1;
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';
  }
  *dirfd = maton_socket_dir_open(directory);
  if (*dirfd < 0) return -1;
  if (maton_socket_path(out, capacity, *dirfd, slash + 1) < 0) {
    close(*dirfd); *dirfd = -1; return -1;
  }
  return (int)strlen(out);
}

#endif
