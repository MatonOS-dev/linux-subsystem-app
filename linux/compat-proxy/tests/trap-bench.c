#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s MODE ITERATIONS PATH\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *path = argv[3];
    char *end = NULL;
    unsigned long iterations = strtoul(argv[2], &end, 10);
    if (!iterations || !end || *end) return 2;

    volatile long checksum = 0;
    struct stat st;
    uint64_t start = now_ns();
    for (unsigned long i = 0; i < iterations; ++i) {
        if (!strcmp(mode, "getpid")) {
            checksum += syscall(SYS_getpid);
        } else if (!strcmp(mode, "stat")) {
            long r = syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0);
            if (r) { perror("newfstatat"); return 1; }
            checksum += st.st_size;
        } else if (!strcmp(mode, "open")) {
            long fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
            if (fd < 0) { perror("openat"); return 1; }
            checksum += syscall(SYS_close, fd);
        } else {
            fprintf(stderr, "unknown mode: %s\n", mode);
            return 2;
        }
    }
    uint64_t elapsed = now_ns() - start;
    printf("mode=%s iterations=%lu elapsed_ns=%llu ns_per_op=%.2f checksum=%ld\n",
           mode, iterations, (unsigned long long)elapsed,
           (double)elapsed / (double)iterations, checksum);
    return 0;
}
